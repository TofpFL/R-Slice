package funkin.external.raid;

#if (cpp && (windows || android))
import funkin.data.song.SongData.SongChartData;
import funkin.data.song.SongData.SongCharacterData;
import funkin.data.song.SongData.SongMetadata;
import funkin.data.song.SongData.SongNoteData;
import funkin.data.song.SongData.SongTimeChange;
import funkin.play.notes.Strumline;
import funkin.play.song.Song;
import funkin.util.Constants;
import openfl.Assets;
import flixel.FlxG;
import funkin.Paths;

/**
 * Streaming note provider for `.raid` (and JSON-family) charts, per
 * docs/ICHART_SOURCE.md step 4. Wraps the `gh_engine_*` C ABI and feeds
 * V-Slice's existing Strumline/NoteSprite/judgement pipeline incrementally:
 * each frame it polls events out to the engine's 4s lookahead and appends
 * `SongNoteData` in time order, so noteData stays a bounded rolling window
 * instead of a fully materialized chart.
 *
 * Engine side field map (see raid_bridge.cpp): 0 timeMs, 1 lane,
 * 2 side (0 opponent, 1 player, 2 gf), 3 lengthMs, 4 kind,
 * 5 noteKind (1 = mine — currently treated as a normal note),
 * 6 noteFlags, 7 noteType, 8 stack, 9 id.
 */
class RaidChartStream
{
	public var path:String;
	public var engine:cpp.Pointer<cpp.Void>;
	public var kind:Int = -1;
	public var logicalEvents:Float = 0;
	/** Stack-weighted player-side note total for the results screen (0 when unknown). */
	public var noteTotal:Float = 0;
	/** Stack-weighted opponent-side note total (0 when unknown). */
	public var opponentTotal:Float = 0;
	/** Stack-weighted chart total across all sides (0 when unknown). */
	public var chartTotal:Float = 0;
	public var bpm:Float = 120;
	/** Smoothed note-event rate from the last pump window (events/sec). */
	public var eventsPerSec:Float = 0;
	/** Last native chunk-cache stats string (`hits=… misses=… decodes=… cached=… cap=…`). */
	public var cacheStats:String = '';

	var closed:Bool = false;

	public function new(path:String)
	{
		this.path = resolveEnginePath(path);
		engine = RaidEngine.open(this.path);
		if (engine == null) throw 'RaidChartStream: gh_engine_open failed for $path';
		kind = RaidEngine.kind(engine);
		logicalEvents = RaidEngine.logicalEvents(engine);
		noteTotal = RaidEngine.noteTotal(engine, 1);
		opponentTotal = RaidEngine.noteTotal(engine, 0);
		chartTotal = RaidEngine.noteTotal(engine, -1);
		var b:Float = RaidEngine.bpm(engine);
		if (b > 0) bpm = b;
	}

	public function seek(tMs:Float):Void
	{
		if (engine != null) RaidEngine.seek(engine, tMs);
		callLogMark = -1; // re-log the op stream after a rewind
	}

	public function close():Void
	{
		if (closed || engine == null) return;
		RaidEngine.close(engine);
		engine = null;
		closed = true;
	}

	/**
	 * Poll the engine once and append due note events to the strumlines.
	 * Call once per frame with the conductor's song position.
	 */
	public function pump(player:Strumline, opponent:Strumline, nowMs:Float):Void
	{
		if (engine == null) return;

		// Frame-time tracking for the perf log: EMA of frame delta plus the
		// worst frame seen since the last log tick, and a ring of recent
		// frame deltas for p50/p95/p99 percentiles (RAID_PROFILE).
		var frameMs:Float = FlxG.elapsed * 1000;
		frameEmaMs = frameEmaMs <= 0 ? frameMs : frameEmaMs * 0.9 + frameMs * 0.1;
		if (frameMs > frameWorstMs) frameWorstMs = frameMs;
		if (frameRing.length < 4096)
			frameRing.push(frameMs);
		else
		{
			frameRing[frameRingPos] = frameMs;
			frameRingPos = (frameRingPos + 1) % 4096;
		}

		RaidProf.begin("stream");
		RaidEngine.prefetch(engine, nowMs, 16000);

		// The native side buffers converted events internally; a dense
		// burst can exceed the 8192-event poll capacity, so drain until
		// empty (bounded) rather than letting stragglers arrive a frame
		// late and get skipped as "past" notes.
		var polled:Int = 0;
		for (round in 0...8)
		{
			var n:Int = RaidEngine.poll(engine, nowMs);
			if (n <= 0) break;
			polled += n;
			// Bulk-read path: one native buffer, 6 doubles/event —
			// replaces ~8 extern calls per event on dense charts.
			var flat = RaidEngine.eventsFlat();
			for (i in 0...n)
			{
				var b:Int = i * 6;
				// kind 0 = note; camera/character/stage events are not
				// produced by the backend yet.
				if (flat[b + 4] != 0) continue;

				var lane:Int = Std.int(flat[b + 1]);
				if (lane < 0 || lane > 3) continue; // 4K strumlines only

				var side:Int = Std.int(flat[b + 2]);
				var target:Strumline = side == 1 ? player : side == 0 ? opponent : null;
				if (target == null) continue; // gf notes: no third strumline

				// SongNoteData.data = strumlineIndex * 4 + direction.
				var data:Int = (side == 1 ? 0 : 4) + lane;
				var note = new SongNoteData(
					flat[b], data,
					flat[b + 3], "", null);
				// Stacked notes: one press hits the whole stack; score,
				// combo and note counts multiply by it downstream.
				note.stack = Std.int(Math.max(1.0, flat[b + 5]));
				target.addNoteData(note, false); // events arrive time-sorted
			}
		}

		compact(player);
		compact(opponent);
		RaidProf.end("stream");
		RaidProf.count("ev", polled);

		polledTotal += polled;
		var sec:Int = Std.int(nowMs / 2000);
		if (sec != lastLogSec)
		{
			lastLogSec = sec;
			var rate:Float = polledTotal / 2;
			polledTotal = 0;
			var stats:String = "";
			var statsPtr = RaidEngine.cacheStats(engine);
			if (statsPtr != null) stats = statsPtr;
			eventsPerSec = rate;
			cacheStats = stats;
			var errPtr = RaidEngine.lastError(engine);
			var errStr:String = errPtr != null ? errPtr : "";
			trace('[RAID-STREAM] t=' + Std.int(nowMs) + 'ms kind=' + kind +
				' ev/s=' + Std.int(rate) +
				(errStr.length > 0 ? ' ERR=' + errStr : '') +
				' events=' + Std.int(logicalEvents) +
				' frame=' + Std.int(frameEmaMs * 10) / 10 + 'ms' +
				' worst=' + Std.int(frameWorstMs) + 'ms' +
				' playerData=' + player.noteData.length +
				' oppData=' + opponent.noteData.length +
				' playerSprites=' + player.notes.members.length +
				' oppSprites=' + opponent.notes.members.length +
				' cache{' + stats + '}' +
				framePercentiles() + RaidProf.reportAndReset());
			logCalls(nowMs);
			frameWorstMs = 0;
		}
	}

	var lastLogSec:Int = -1;
	var callLogMark:Float = -1;
	var polledTotal:Int = 0;
	var frameEmaMs:Float = 0;
	var frameWorstMs:Float = 0;
	var frameRing:Array<Float> = [];
	var frameRingPos:Int = 0;

	/** ` f50=.. f95=.. f99=.. fmax=..` over the recent-frame ring. */
	function framePercentiles():String
	{
		var n:Int = frameRing.length;
		if (n < 30) return "";
		var s:Array<Float> = frameRing.slice(0, n);
		s.sort(function(a, b) return a < b ? -1 : a > b ? 1 : 0);
		var p = function(q:Float):Float {
			var i:Int = Std.int(n * q);
			if (i >= n) i = n - 1;
			return s[i];
		};
		return ' f50=' + Std.int(p(0.50) * 10) / 10 +
			' f95=' + Std.int(p(0.95) * 10) / 10 +
			' f99=' + Std.int(p(0.99) * 10) / 10 +
			' fmax=' + Std.int(s[n - 1]) + 'ms';
	}

	/**
	 * Reads the .raid op log — the pattern-call view of the chart — for
	 * the engine's lookahead window and traces each pattern instance as
	 * its spawn time approaches. Literal runs are counted but not logged
	 * individually; pattern segments carry the patternId, repeat count
	 * and pitch/lane transforms the expansion applies.
	 */
	function logCalls(nowMs:Float):Void
	{
		if (kind != 1) return; // op streams only exist for raid charts
		var n:Int = RaidEngine.opLog(engine, nowMs, nowMs + 4000);
		var lastStart:Float = callLogMark;
		for (i in 0...n)
		{
			var startMs:Float = RaidEngine.opField(i, 0);
			var endMs:Float = RaidEngine.opField(i, 1);
			if (startMs > lastStart) lastStart = startMs;
			if (startMs <= callLogMark) continue;
			if (Std.int(RaidEngine.opField(i, 3)) != 2) continue; // literals skipped
			var repeats:Int = Std.int(RaidEngine.opField(i, 5));
			trace('[RAID-CALL] t=' + Std.int(startMs) + 'ms pattern=#' +
				Std.int(RaidEngine.opField(i, 4)) +
				' chunk=' + Std.int(RaidEngine.opField(i, 2)) +
				' x' + (repeats > 0 ? repeats + 1 : 1) +
				' pitch' + (RaidEngine.opField(i, 6) > 0 ? '+' : '') +
				Std.int(RaidEngine.opField(i, 6)) +
				' lane' + (RaidEngine.opField(i, 7) > 0 ? '+' : '') +
				Std.int(RaidEngine.opField(i, 7)) +
				' span=' + Std.int(endMs - startMs) + 'ms');
		}
		// Advance the watermark to the last segment START seen, not the
		// window end: a long pattern instance can span many windows, and
		// marking past its endMs would suppress every call inside it.
		callLogMark = lastStart;
	}

	/**
	 * Drop the consumed prefix of `noteData` so resident note data stays
	 * bounded on dense charts. Entries below `nextNoteIndex` were already
	 * scanned by Strumline.updateNotes (spawned or skipped as past); live
	 * NoteSprites keep their own reference to the SongNoteData object.
	 */
	static function compact(strumline:Strumline):Void
	{
		var n:Int = strumline.nextNoteIndex;
		if (n > 8192 && n * 2 > strumline.noteData.length)
		{
			strumline.noteData.splice(0, n);
			strumline.nextNoteIndex = 0;
		}
	}

	/**
	 * Build a playable `Song` for a `.raid` chart. The song keeps the
	 * donor song's id so instrumental/vocals/stage/character assets
	 * resolve from its asset folder; notes stream from the chart instead
	 * of a JSON chart file. The returned difficulty's `raidChartPath`
	 * tells PlayState to feed it through a RaidChartStream.
	 *
	 * The .raid meta section supplies the display title and, when present,
	 * character/stage names. Names are only honored when they resolve to
	 * real V-Slice assets — unknown names fall back to the bf/gf/mainStage
	 * defaults so charts from other engines still launch.
	 *
	 * @param raidPath Path to the .raid (or JSON-family) chart file.
	 * @param donorId  Song id whose assets (audio, stage, characters)
	 *                 should back the raid chart.
	 */
	public static function createSong(raidPath:String, donorId:String):Song
	{
		var probe = new RaidChartStream(raidPath);
		var bpm:Float = probe.bpm;
		probe.close();

		var title:String = 'RAID Chart';
		var opponent:String = 'gf';
		var player:String = 'bf';
		var gf:String = 'gf';
		var stage:String = 'mainStage';

		var metaPtr = RaidEngine.metaJson(resolveEnginePath(raidPath));
		if (metaPtr != null)
		{
			try
			{
				var j:Dynamic = haxe.Json.parse(metaPtr);
				if (j != null)
				{
					if (Std.is(String, j.title) && j.title.length > 0) title = j.title;
					if (validCharName(j.opponent)) opponent = j.opponent;
					if (validCharName(j.player)) player = j.player;
					if (validCharName(j.gf)) gf = j.gf;
					if (Std.is(String, j.stage) && j.stage.length > 0 &&
						funkin.data.stage.StageRegistry.instance.fetchEntry(j.stage) != null)
						stage = j.stage;
				}
			}
			catch (_) {}
		}

		var meta = new SongMetadata(title, 'R-Slice Engine', 'Devin');
		meta.playData.difficulties = ['normal'];
		meta.playData.songVariations = [Constants.DEFAULT_VARIATION];
		meta.playData.stage = stage;
		meta.playData.characters = new SongCharacterData(player, gf, opponent);
		meta.timeChanges = [new SongTimeChange(0, bpm)];

		var emptyNotes:Array<SongNoteData> = [];
		var chartData = new SongChartData(
			['normal' => 1.0, 'default' => 1.0],
			[],
			['normal' => emptyNotes]);
		chartData.variation = Constants.DEFAULT_VARIATION;

		var charts = new Map<String, SongChartData>();
		charts.set(Constants.DEFAULT_VARIATION, chartData);

		var song = Song.buildRaw(donorId, [meta], Constants.DEFAULT_VARIATION, charts, false);
		var diff = song.getDifficulty(Constants.DEFAULT_DIFFICULTY, Constants.DEFAULT_VARIATION);
		if (diff != null) diff.raidChartPath = raidPath;
		return song;
	}

	/**
	 * On Android the chart lives inside the APK's assets — there is no
	 * filesystem path the engine's fopen() can reach (Sys.programPath()
	 * resolves to /system/bin, so gameDirectory-based paths are garbage).
	 * Rebuild the openfl asset id from the `songs/<id>/<file>` tail,
	 * copy the bytes into app storage once, and return the real path.
	 * Desktop paths and genuine filesystem paths pass through unchanged.
	 */
	static var enginePathCache:Map<String, String> = new Map();
	static function resolveEnginePath(path:String):String
	{
		#if android
		var cached = enginePathCache.get(path);
		if (cached != null) return cached;
		if (!sys.FileSystem.exists(path))
		{
			var songsIdx:Int = path.indexOf('songs/');
			if (songsIdx >= 0)
			{
				var assetId:String = 'songs:assets/' + path.substr(songsIdx);
				try
				{
					var bytes:haxe.io.Bytes = openfl.utils.Assets.getBytes(assetId);
					if (bytes != null)
					{
						var dir:String = lime.system.System.applicationStorageDirectory
							+ '/raidcache';
						sys.FileSystem.createDirectory(dir);
						var real:String = dir + '/' + path.substr(path.lastIndexOf('/') + 1);
						sys.io.File.saveBytes(real, bytes);
						enginePathCache.set(path, real);
						return real;
					}
				}
				catch (_) {}
			}
		}
		enginePathCache.set(path, path);
		return path;
		#else
		return path;
		#end
	}

	/** Character ids resolve to data/characters/<id>.json; reject unknowns. */
	static function validCharName(id:Dynamic):Bool
	{
		if (!Std.is(String, id) || id.length == 0) return false;
		return Assets.exists(Paths.json('characters/$id'));
	}
}
#end
