package funkin.external.raid;

import flixel.FlxCamera;
import flixel.FlxG;
import flixel.FlxSprite;
import flixel.graphics.frames.FlxFrame;
import flixel.graphics.tile.FlxDrawBaseItem;
import openfl.Vector;
import openfl.display.BlendMode;
import openfl.display.BitmapData;
import openfl.display.Graphics;
import openfl.display.TriangleCulling;
import openfl.geom.Point;
import lime.utils.DataPointer;
import funkin.Conductor;
import funkin.data.song.SongData.SongNoteData;
import funkin.play.notes.NoteSprite;
import funkin.play.notes.Strumline;

/**
 * GPU-batched note renderer for `.raid` charts (Phase 5B).
 *
 * Instead of one `NoteSprite` per visible note in the render path, this
 * batches every streamed note into a single `drawTriangles` call per
 * strumline, injected into `camStrumlines`' draw stack at the falling-notes
 * z-position. Note sprites are still built for CPU-side judgement but the
 * `notes` group is hidden from the Flixel draw traversal.
 *
 * NOTE: positions are currently computed on the CPU each frame (same math as
 * `Strumline.updateNotes`/`NoteSprite`). Moving the transform into a custom
 * vertex shader is the Phase 5D follow-up; the batch structure is already in
 * place for it.
 */
class RaidGLNotes
{
	/** Pixels per millisecond of scroll travel (Constants.PIXELS_PER_MS). */
	static var PIXELS_PER_MS:Float = 0.45;
	/** Strumline.INITIAL_OFFSET is -0.275 * 104 = -28.6; note.y subtracts it. */
	static var INITIAL_OFFSET_Y:Float = 28.6;

	public var strumline:Strumline;

	/** Per-note static data, appended as notes enter the stream window. */
	public var times:Array<Float> = [];
	public var dirs:Array<Int> = [];
	public var noteObjs:Array<SongNoteData> = [];

	/** Notes removed by judgement (hit/miss) — skipped at render time.
	 *  Keyed by SongNoteData identity because NoteSprites are recycled. */
	var deadNotes:Map<SongNoteData, Bool> = new Map<SongNoteData, Bool>();

	/** First index that can still be on screen; earlier entries are compacted away. */
	var head:Int = 0;

	/** Per-direction render constants captured from real NoteSprites. */
	var dirData:Array<RaidGLNotesDir> = [];

	public var drawItem:RaidGLNotesDrawItem;
	var hook:RaidGLNotesHook;

	// ---- Phase 5D: instanced GL backend (CPU path kept as fallback) ----
	/** Renderer slot, -1 = unavailable/disabled. */
	var glHandle:Int = -1;
	var glTried:Bool = false;
	/** Atlas bitmap last uploaded to GL; re-upload on change. */
	var glBitmap:BitmapData = null;
	/** Per-direction constants already pushed to the GL renderer. */
	var glDirSent:Array<Bool> = [false, false, false, false, false, false, false, false];
	/** Env override: RAID_GL_NOTES=0 forces the CPU path. */
	static var glNotesEnv:Null<Bool> = null;
	static var glDebugPrinted:Int = 0;
	static var glDebugPixelDumped:Bool = false;
	/** GL draw is deferred to the canvas's RENDER_OPENGL event — lime
	 *  flushes its own GL work there, so an immediate draw inside
	 *  drawItem.render lands in the wrong render target. */
	var glPending:Bool = false;
	var glListenerCanvas:Dynamic = null;
	var glRenderCb:openfl.events.Event->Void = null;
	// Staged draw params (set in renderInner, consumed by onGLRender).
	var glNow:Float = 0;
	var glPxPerMs:Float = 0;
	var glDirSign:Float = 1;
	var glStrumX:Float = 0;
	var glStrumY:Float = 0;
	var glScrollX:Float = 0;
	var glScrollY:Float = 0;
	var glR00:Float = 1;
	var glR01:Float = 0;
	var glR02:Float = 0;
	var glR10:Float = 0;
	var glR11:Float = 1;
	var glR12:Float = 0;
	var glCullX0:Float = 0;
	var glCullY0:Float = 0;
	var glCullX1:Float = 0;
	var glCullY1:Float = 0;
	// Reused per-frame buffers (avoid realloc).
	var vertices:Vector<Float> = new Vector<Float>();
	var indices:Vector<Int> = new Vector<Int>();
	var uvtData:Vector<Float> = new Vector<Float>();

	public function new(sl:Strumline)
	{
		strumline = sl;
		drawItem = new RaidGLNotesDrawItem(this);
		hook = new RaidGLNotesHook(this);

		// Insert the hook right after the `notes` group so GPU notes draw at
		// the falling-notes z-position (above hold trails, below vwoosh/cover/splash).
		var idx:Int = sl.members.indexOf(sl.notes);
		if (idx >= 0)
			sl.insert(idx + 1, hook);
		else
			sl.add(hook);
	}

	/**
	 * Record a note for GPU batching. Called from `Strumline.updateNotes`
	 * alongside `buildNoteSprite` (sprites remain for judgement).
	 */
	public function addNote(note:SongNoteData, sprite:Null<NoteSprite>):Void
	{
		var d:Int = note.getDirection() % Strumline.KEY_COUNT;
		times.push(note.time);
		dirs.push(d);
		noteObjs.push(note);
		if (dirData[d] == null && sprite == null)
			sprite = strumline.buildDirProbeSprite(d);
		if (sprite != null) captureDir(d, sprite);
		#if (windows && cpp)
		if (glHandle >= 0)
		{
			// Always append — staging indices must stay aligned with
			// times[]/noteObjs[] even when dir constants aren't captured yet
			// (unset dirs produce degenerate zero-area quads = invisible,
			// matching the CPU path's `d == null` skip).
			if (d < glDirSent.length)
			{
				var dd = dirData[d];
				if (dd != null && !glDirSent[d]) { glDirSent[d] = true; pushDirToGL(d, dd); }
			}
			RaidGLSmoke.notesAdd(glHandle, note.time, d);
		}
		#end
	}

	/**
	 * Called from `Strumline.killNote` when a note's sprite is retired
	 * (hit, missed and scrolled off, or cleaned up). The GPU copy of the
	 * note stops rendering; judgement state stays on the CPU sprite path.
	 */
	public function noteKilled(note:SongNoteData):Void
	{
		if (note != null) deadNotes.set(note, true);
		if (glHandle >= 0 && note != null)
		{
			var idx:Int = findNoteIndex(note);
			#if (windows && cpp)
			if (idx >= 0) RaidGLSmoke.notesKill(glHandle, idx);
			#end
		}
	}

	/**
	 * Index of `note` in the time-sorted arrays (binary search on time,
	 * identity scan inside equal-time runs). -1 when absent.
	 */
	function findNoteIndex(note:SongNoteData):Int
	{
		var lo:Int = head;
		var hi:Int = times.length - 1;
		var t:Float = note.time;
		while (lo <= hi)
		{
			var mid:Int = (lo + hi) >> 1;
			if (times[mid] < t) lo = mid + 1;
			else if (times[mid] > t) hi = mid - 1;
			else
			{
				// Scan the equal-time run for identity.
				var j:Int = mid;
				while (j > head && times[j - 1] == t) j--;
				while (j < times.length && times[j] == t)
				{
					if (noteObjs[j] == note) return j;
					j++;
				}
				return -1;
			}
		}
		return -1;
	}

	public function reset():Void
	{
		times.resize(0);
		dirs.resize(0);
		noteObjs.resize(0);
		deadNotes.clear();
		head = 0;
		dirData = [];
		glBitmap = null;
		glPending = false;
		for (i in 0...glDirSent.length) glDirSent[i] = false;
		#if (windows && cpp)
		if (glHandle >= 0) RaidGLSmoke.notesReset(glHandle);
		#end
	}

	function captureDir(d:Int, sprite:Null<NoteSprite>):Void
	{
		if (d < 0 || d >= Strumline.KEY_COUNT || dirData[d] != null || sprite == null) return;
		var frame:FlxFrame = sprite.frame;
		if (frame == null) return;

		var dd = new RaidGLNotesDir();
		dd.u0 = frame.uv.left; dd.v0 = frame.uv.top;
		dd.u1 = frame.uv.right; dd.v1 = frame.uv.bottom;
		dd.fw = frame.frame.width;
		dd.fh = frame.frame.height;
		dd.sx = sprite.scale.x;
		dd.sy = sprite.scale.y;
		dd.ox = sprite.origin.x;
		dd.oy = sprite.origin.y;
		dd.offX = sprite.offset.x;
		dd.offY = sprite.offset.y;

		// sprite.x = strumline.x + laneOff + LANE_CENTER - (fw*sx)/2
		// => world-space x base relative to strumline.x
		dd.xBase = sprite.x - strumline.x;
		dd.bitmap = sprite.frame.parent != null ? sprite.frame.parent.bitmap : null;

		dirData[d] = dd;
	}

	/**
	 * Called from the injected draw item during `camStrumlines.render()`.
	 * Builds view-space quads for every streamed note and submits one
	 * `drawTriangles` batch per strumline.
	 */
	public function render(camera:FlxCamera):Void
	{
		RaidProf.begin("glnotes");
		renderInner(camera);
		RaidProf.end("glnotes");
	}

	function renderInner(camera:FlxCamera):Void
	{
		if (times.length == 0) return;

		// Lazy atlas resolution: all captured dirs should share one bitmap.
		var bitmap:BitmapData = null;
		for (d in dirData)
		{
			if (d != null) { bitmap = d.bitmap; break; }
		}
		if (bitmap == null) return;

		var now:Float = strumline.conductorInUse.getTimeWithDelta();
		var scrollSpeed:Float = strumline.scrollSpeed;
		var dirSign:Float = strumline.isDownscroll ? 1.0 : -1.0;
		var strumX:Float = strumline.x;
		var strumY:Float = strumline.y;
		var scrollX:Float = camera.scroll.x;
		var scrollY:Float = camera.scroll.y;

		// Conservative view-space bounds (view coords are pre-zoom).
		var minX:Float = -512;
		var minY:Float = -512;
		var maxX:Float = camera.viewWidth + 512;
		var maxY:Float = camera.viewHeight + 512;

		// Advance the head past notes that can never be visible again.
		// Notes are time-sorted; a note exits the far side once
		// now - time exceeds the scroll distance covering the viewport.
		var pxPerMs:Float = PIXELS_PER_MS * scrollSpeed;
		var aheadCut:Float = 1e30;
		if (pxPerMs > 0.0001)
		{
			var travel:Float = dirSign > 0
				? (camera.viewHeight + 512 + scrollY - strumY - INITIAL_OFFSET_Y) / pxPerMs
				: (512 + strumY + INITIAL_OFFSET_Y - scrollY) / pxPerMs;
			var cutoff:Float = now - travel;
			var count0:Int = times.length;
			while (head < count0 && times[head] < cutoff)
			{
				deadNotes.remove(noteObjs[head]);
				head++;
			}
			if (head >= 4096)
			{
				times.splice(0, head);
				dirs.splice(0, head);
				noteObjs.splice(0, head);
				#if (windows && cpp)
				if (glHandle >= 0) RaidGLSmoke.notesRemovePrefix(glHandle, head);
				#end
				head = 0;
			}
			// Ahead cutoff: notes further than the far viewport edge can
			// never be visible this frame. `times` is sorted, so the scan
			// can stop at the first too-far-ahead note instead of walking
			// the whole stream window (Phase CPU opt — was ~85% waste).
			var aheadPx:Float = dirSign > 0
				? (strumY + INITIAL_OFFSET_Y + 576 - scrollY)
				: (camera.viewHeight + 576 + scrollY - strumY - INITIAL_OFFSET_Y);
			aheadCut = now + aheadPx / pxPerMs;
		}
		var count:Int = times.length - head;
		if (count <= 0) return;

		// Phase 5D: instanced GL path — zero per-frame vertex work. The
		// actual draw is deferred to the canvas's RENDER_OPENGL event
		// (lime's render pass); here we only stage params and skip the
		// CPU batch. Falls through whenever the renderer isn't ready.
		if (tryGLDraw(camera, now, scrollSpeed, dirSign, strumX, strumY,
				scrollX, scrollY, minX, minY, maxX, maxY)) return;

		vertices.length = 0;
		indices.length = 0;
		uvtData.length = 0;

		var vi:Int = 0;
		var it:Int = 0;

		for (k in 0...count)
		{
			var i:Int = head + k;
			if (times[i] > aheadCut) break;
			it++;
			// Dead (judged) notes can only exist near the judgement
			// instant — skip the map lookup for everything else.
			if (times[i] <= now + 500 && deadNotes.get(noteObjs[i]) == true) continue;
			var d = dirData[dirs[i]];
			if (d == null || d.bitmap != bitmap) continue;

			var worldX:Float = strumX + d.xBase;
			var worldY:Float = strumY + INITIAL_OFFSET_Y
				+ PIXELS_PER_MS * (now - times[i]) * scrollSpeed * dirSign;

			// View space = world - camera.scroll (scrollFactor = 1).
			var vx:Float = worldX - scrollX;
			var vy:Float = worldY - scrollY;

			// Screen = view - offset + origin + scale*(corner - origin)
			var x0:Float = vx - d.offX + d.ox * (1 - d.sx);
			var y0:Float = vy - d.offY + d.oy * (1 - d.sy);
			var x1:Float = x0 + d.sx * d.fw;
			var y1:Float = y0 + d.sy * d.fh;

			if (x1 < minX || x0 > maxX || y1 < minY || y0 > maxY) continue;

			// TL, TR, BL, BR
			vertices.push(x0, y0, x1, y0, x0, y1, x1, y1);
			uvtData.push(d.u0, d.v0, d.u1, d.v0, d.u0, d.v1, d.u1, d.v1);
			indices.push(vi, vi + 1, vi + 2, vi + 1, vi + 2, vi + 3);
			vi += 4;
		}
		RaidProf.count("glscan", it);

		if (vi == 0) return;
		RaidProf.count("glquad", vi >> 2);

		var g:Graphics = camera.canvas.graphics;
		g.overrideBlendMode(BlendMode.NORMAL);
		g.beginBitmapFill(bitmap, null, true, camera.antialiasing);
		g.drawTriangles(vertices, indices, uvtData, TriangleCulling.NONE);
		g.endFill();
	}

	// ------------------------------------------------------------------
	// Phase 5D instanced GL backend
	// ------------------------------------------------------------------

	#if (windows && cpp)

	/** Lazily create the GL renderer once (needs a current GL context —
	 *  render() runs inside the draw traversal, so it is current here). */
	function tryInitGL():Void
	{
		if (glTried) return;
		glTried = true;
		if (glNotesEnv == null)
			glNotesEnv = Sys.getEnv("RAID_GL_NOTES") != "0";
		if (!glNotesEnv) return;
		// Required: VAO + instancing + divisor. Shader compile is verified
		// inside rs_notes_create (330 with 120 fallback).
		var caps:Int = RaidGLSmoke.caps();
		var need:Int = (1 << 0) | (1 << 1) | (1 << 2);
		if ((caps & need) != need) return;
		glHandle = RaidGLSmoke.notesCreate();
		if (glHandle < 0) glHandle = -1;
		if (glHandle < 0) return;

		// Backfill: notes recorded before the renderer existed.
		for (i in 0...times.length)
		{
			var d:Int = dirs[i];
			if (d < glDirSent.length)
			{
				var dd = dirData[d];
				if (dd != null && !glDirSent[d]) { glDirSent[d] = true; pushDirToGL(d, dd); }
			}
			RaidGLSmoke.notesAdd(glHandle, times[i], d);
		}
		for (n in deadNotes.keys())
		{
			var idx:Int = findNoteIndex(n);
			if (idx >= 0) RaidGLSmoke.notesKill(glHandle, idx);
		}
	}

	/** Upload per-direction constants (16-float layout matching the C++ side). */
	function pushDirToGL(d:Int, dd:RaidGLNotesDir):Void
	{
		if (glHandle < 0) return;
		RaidGLSmoke.notesSetDir(glHandle, d,
			dd.fw, dd.fh, dd.sx, dd.sy,
			dd.ox, dd.oy, dd.offX, dd.offY,
			dd.xBase, dd.u0, dd.v0, dd.u1,
			dd.v1, 0, 0, 0);
	}

	/**
	 * Upload the atlas bitmap as a straight-alpha RGBA texture.
	 * `BitmapData.getPixels` unmultiplies, so the GL side premultiplies
	 * in-shader and blends premultiplied — same result as OpenFL.
	 */
	function ensureGLTexture(bitmap:BitmapData):Bool
	{
		if (glHandle < 0 || bitmap == null) return false;
		if (glBitmap == bitmap) return true;
		var rect = new openfl.geom.Rectangle(0, 0, bitmap.width, bitmap.height);
		var ba:openfl.utils.ByteArray = bitmap.getPixels(rect);
		if (ba == null || ba.length < bitmap.width * bitmap.height * 4) return false;
		// getPixels requests ARGB32 → native path writes A,R,G,B bytes
		// (lime RGBA::WriteUInt8). GL wants R,G,B,A — rotate each u32
		// left by one byte. ByteArray is big-endian here, so a BE u32
		// read gives 0xAARRGGBB and writing 0xRRGGBBAA yields R,G,B,A.
		var pxCount:Int = bitmap.width * bitmap.height;
		for (i in 0...pxCount)
		{
			ba.position = i * 4;
			var v:Int = ba.readInt();
			ba.position = i * 4;
			ba.writeInt((v << 8) | (v >>> 24));
		}
		// ByteArray->DataPointer resolves at byteArray.position — reset to
		// 0 or the GL upload reads past the end of the buffer (segfault).
		ba.position = 0;
		if (Sys.getEnv("RAID_GL_DEBUG") == "1" && !glDebugPixelDumped)
		{
			glDebugPixelDumped = true;
			var argb:Int = bitmap.getPixel(0, 0);
			ba.position = 0;
			var b0:Int = ba.readByte();
			var b1:Int = ba.readByte();
			var b2:Int = ba.readByte();
			var b3:Int = ba.readByte();
			Sys.println('RAID-PIX argb=$argb bytes=$b0,$b1,$b2,$b3'
				+ ' len=${ba.length} w=${bitmap.width} h=${bitmap.height}');
		}
		var dp:DataPointer = ba;
		var ptrF:Float = dp;
		if (RaidGLSmoke.notesSetTexture(glHandle, ptrF, bitmap.width, bitmap.height) != 1)
			return false;
		glBitmap = bitmap;
		return true;
	}

	/**
	 * Attempt the instanced draw. Returns true when the GL batch was
	 * submitted; false → caller runs the CPU path.
	 */
	function tryGLDraw(camera:FlxCamera, now:Float, scrollSpeed:Float,
			dirSign:Float, strumX:Float, strumY:Float,
			scrollX:Float, scrollY:Float,
			minX:Float, minY:Float, maxX:Float, maxY:Float):Bool
	{
		tryInitGL();
		if (glHandle < 0) return false;

		// Atlas: same rule as the CPU path — first non-null dirData bitmap.
		var bitmap:BitmapData = null;
		for (d in dirData)
		{
			if (d != null) { bitmap = d.bitmap; break; }
		}
		if (bitmap == null) return false;
		// Dirs whose bitmap differs can't be drawn by this single-texture
		// batch — mirror the CPU path's skip by leaving them dead.
		for (d in dirData)
			if (d != null && d.bitmap != bitmap) return false;
		if (!ensureGLTexture(bitmap)) return false;

		// View -> device-px affine, probed through the canvas transform so
		// camera zoom, scrollRect offsets and scaleMode are all captured.
		var cv = camera.canvas;
		if (cv == null) return false;
		var m:openfl.geom.Matrix = cv.transform.concatenatedMatrix;
		// dev = M * viewPoint, M = canvas world transform (concatenated).
		glR00 = m.a; glR01 = m.c; glR02 = m.tx;
		glR10 = m.b; glR11 = m.d; glR12 = m.ty;
		glNow = now;
		glPxPerMs = PIXELS_PER_MS * scrollSpeed;
		glDirSign = dirSign;
		glStrumX = strumX; glStrumY = strumY;
		glScrollX = scrollX; glScrollY = scrollY;
		glCullX0 = minX; glCullY0 = minY; glCullX1 = maxX; glCullY1 = maxY;

		// Stage the draw for the canvas's RENDER_OPENGL event — that fires
		// inside lime's GL render pass with the correct target/viewport.
		glPending = true;
		if (glListenerCanvas != cv)
		{
			if (glListenerCanvas != null && glRenderCb != null)
				glListenerCanvas.removeEventListener(openfl.events.RenderEvent.RENDER_OPENGL, glRenderCb);
			if (glRenderCb == null) glRenderCb = onGLRender;
			cv.addEventListener(openfl.events.RenderEvent.RENDER_OPENGL, glRenderCb);
			glListenerCanvas = cv;
		}
		return true;
	}

	/** Fires during lime's GL render of camera.canvas — GL state here is
	 *  the same target/viewport the canvas graphics batch just drew into. */
	function onGLRender(e:openfl.events.Event):Void
	{
		if (!glPending || glHandle < 0) return;
		glPending = false;
		RaidGLSmoke.notesDraw(glHandle,
			glNow, glPxPerMs, glDirSign,
			glStrumX, glStrumY, glScrollX, glScrollY,
			glR00, glR01, glR02, glR10, glR11, glR12,
			glCullX0, glCullY0, glCullX1, glCullY1);
	}

	#else

	function tryInitGL():Void {}
	function pushDirToGL(d:Int, dd:RaidGLNotesDir):Void {}
	function ensureGLTexture(bitmap:BitmapData):Bool { return false; }
	function tryGLDraw(camera:FlxCamera, now:Float, scrollSpeed:Float,
			dirSign:Float, strumX:Float, strumY:Float,
			scrollX:Float, scrollY:Float,
			minX:Float, minY:Float, maxX:Float, maxY:Float):Bool { return false; }

	#end
}

/** Per-direction constants captured from a representative NoteSprite. */
class RaidGLNotesDir
{
	public var u0:Float = 0;
	public var v0:Float = 0;
	public var u1:Float = 1;
	public var v1:Float = 1;
	public var fw:Float = 0;
	public var fh:Float = 0;
	public var sx:Float = 1;
	public var sy:Float = 1;
	public var ox:Float = 0;
	public var oy:Float = 0;
	public var offX:Float = 0;
	public var offY:Float = 0;
	/** noteSprite.x - strumline.x for this direction. */
	public var xBase:Float = 0;
	public var bitmap:BitmapData;

	public function new() {}
}

/**
 * FlxSprite whose only job is to inject `RaidGLNotesDrawItem` into the
 * strumline camera's draw stack once per frame, at the notes z-position.
 */
@:access(flixel.FlxCamera)
class RaidGLNotesHook extends FlxSprite
{
	var renderer:RaidGLNotes;

	public function new(r:RaidGLNotes)
	{
		renderer = r;
		super();
		visible = true;
		exists = true;
	}

	override public function draw():Void
	{
		for (cam in getCameras())
		{
			var item = renderer.drawItem;
			item.next = null;
			if (cam._headOfDrawStack == null)
				cam._headOfDrawStack = item;
			if (cam._currentDrawItem != null)
				cam._currentDrawItem.next = item;
			cam._currentDrawItem = item;
		}
	}
}

/** Draw-stack item that submits the batched note quads. */
class RaidGLNotesDrawItem extends FlxDrawBaseItem<RaidGLNotesDrawItem>
{
	var renderer:RaidGLNotes;

	public function new(r:RaidGLNotes)
	{
		renderer = r;
		super();
	}

	override public function render(camera:FlxCamera):Void
	{
		renderer.render(camera);
		super.render(camera);
	}
}
