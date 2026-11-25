package funkin.external.raid;

import funkin.data.song.SongData.SongNoteData;
import funkin.play.notes.NoteSprite;
import funkin.play.notes.Strumline;
import funkin.util.Constants;
import flixel.FlxG;

/**
 * Data-driven judgement tracker for `.raid` charts (Phase 7).
 *
 * Standard tap notes no longer get a `NoteSprite` when they enter the
 * render window. Instead they live here as compact records until their
 * judgement moment:
 *
 *  - auto-hit side (opponent, or player in botplay): sprite materializes
 *    when `songPosition >= note.time`
 *  - miss side: sprite materializes when `songPosition > note.time + HIT_WINDOW_MS`
 *  - human hits: `processInputQueue` materializes the chosen record on press
 *
 * Once materialized, the sprite joins `strumline.notes` and is judged by
 * PlayState's unmodified sprite loops (`checkNoteWindow`, `hitNote`,
 * `onNoteMiss`, `goodNoteHit`), so scoring, combo, stack weighting, script
 * events and hold covers keep identical semantics. Sprites are recycled by
 * the existing pool, so only ~1 frame of sprites per judged note exists.
 *
 * `compareMode` keeps the normal sprite path AND tracks records, verifying
 * predicted vs actual outcomes (used by `RAID_JUDGE_COMPARE=1`).
 */
class RaidNoteJudge
{
	public var strumline:Strumline;

	/** Pending records, sorted by note time (same order as `noteData`). */
	public var datas:Array<SongNoteData> = [];
	var times:Array<Float> = [];
	/** Parallel sprite refs — null until the note is judged/materialized. */
	var sprites:Array<Null<NoteSprite>> = [];
	/** Compare-mode: record already outcome-checked (count once). */
	var checked:Array<Bool> = [];
	var head:Int = 0;
	/**
	 * Forward-only cursor over records already evaluated for due-ness.
	 * Records are time-sorted and the due threshold grows monotonically
	 * with songPosition, so tick() only scans newly-due records instead of
	 * the whole window every frame.
	 */
	var scanCursor:Int = 0;
	var lastTickPos:Float = -1e30;

	/** Max NoteSprites materialized per `tick()` call. */
	static inline var MAX_MATERIALIZE_PER_TICK:Int = 512;

	public var compareMode:Bool = false;

	// Comparison-mode counters (logged periodically).
	var predHits:Int = 0;
	var predMisses:Int = 0;
	var actHits:Int = 0;
	var actMisses:Int = 0;
	var mismatches:Int = 0;
	var lastLogPos:Float = -999999;

	public function new(sl:Strumline)
	{
		strumline = sl;
	}

	public function reset():Void
	{
		datas.resize(0);
		times.resize(0);
		sprites.resize(0);
		checked.resize(0);
		head = 0;
		scanCursor = 0;
		lastTickPos = -1e30;
		predHits = predMisses = actHits = actMisses = mismatches = 0;
		lastLogPos = -999999;
	}

	/**
	 * Record a judge-eligible note (standard tap, no kind, no hold length).
	 * Called from `Strumline.updateNotes` in place of `buildNoteSprite`.
	 * In compareMode the sprite is also supplied so outcomes can be verified.
	 */
	public function add(data:SongNoteData, ?sprite:Null<NoteSprite>):Void
	{
		datas.push(data);
		times.push(data.time);
		sprites.push(sprite ?? null);
		checked.push(false);
	}

	/**
	 * Materialize the `NoteSprite` for record `i` and hand it to the normal
	 * sprite judgement path. Idempotent — returns the existing sprite if the
	 * record was already materialized.
	 */
	public function materialize(i:Int):Null<NoteSprite>
	{
		if (i < head || i >= datas.length) return null;
		var s:Null<NoteSprite> = sprites[i];
		if (s != null) return s;

		var data = datas[i];
		s = strumline.buildNoteSprite(data);
		if (s == null) return null;
		if (data.length > 0) s.holdNoteSprite = strumline.buildHoldNoteSprite(data);
		sprites[i] = s;
		strumline.onNoteIncoming.dispatch(s);
		RaidProf.count("jmat");
		return s;
	}

	/** Materialize by note-data identity (binary search on sorted times). */
	public function materializeData(data:SongNoteData):Null<NoteSprite>
	{
		var i = findData(data);
		return i < 0 ? null : materialize(i);
	}

	function findData(data:SongNoteData):Int
	{
		var lo:Int = head;
		var hi:Int = datas.length - 1;
		var t:Float = data.time;
		while (lo <= hi)
		{
			var mid:Int = (lo + hi) >> 1;
			var mt:Float = times[mid];
			if (mt < t) lo = mid + 1;
			else if (mt > t) hi = mid - 1;
			else
			{
				// Scan the equal-time run for the identical object.
				var j:Int = mid;
				while (j >= head && times[j] == t)
				{
					if (datas[j] == data) return j;
					j--;
				}
				j = mid + 1;
				while (j < datas.length && times[j] == t)
				{
					if (datas[j] == data) return j;
					j++;
				}
				return -1;
			}
		}
		return -1;
	}

	/**
	 * Indices of pending (sprite-less) records inside the may-hit window
	 * `[songPosition - HIT_WINDOW_MS, songPosition + HIT_WINDOW_MS]`.
	 * Mirrors the `mayHit` flag that `checkNoteWindow` sets on sprites.
	 */
	public function getPendingMayHit(songPosition:Float):Array<Int>
	{
		var out:Array<Int> = [];
		var w:Float = Constants.HIT_WINDOW_MS;
		var count:Int = datas.length;
		for (i in head...count)
		{
			if (sprites[i] != null) continue;
			var t:Float = times[i];
			if (t < songPosition - w) continue;
			if (t > songPosition + w) break;
			out.push(i);
		}
		return out;
	}

	/**
	 * Per-frame driver, called at the top of `PlayState.processNotes`.
	 *
	 * @param songPosition current conductor song position (ms)
	 * @param isControlled `true` for the human player side (hits come from
	 *        input), `false` for auto-judged sides (opponent, botplay)
	 */
	public function tick(songPosition:Float, isControlled:Bool):Void
	{
		if (compareMode)
		{
			verify(songPosition, isControlled);
			compact(songPosition);
			return;
		}

		var count:Int = datas.length;
		// Backward seek invalidates the cursor — re-evaluate from head.
		if (songPosition < lastTickPos) scanCursor = head;
		lastTickPos = songPosition;
		var scannedFrom:Int = scanCursor;
		var missEdge:Float = Constants.HIT_WINDOW_MS;
		// Bound sprite construction per tick. Dense charts can put tens of
		// thousands of records into the due set after a lag spike; without a
		// budget a single tick materializes all of them and the frame never
		// recovers (E404C death spiral). Deferred records simply materialize
		// on subsequent frames — auto-side hits are perfect regardless of a
		// few frames' delay, and the miss side was already late by definition.
		var budget:Int = MAX_MATERIALIZE_PER_TICK;
		var i:Int = scanCursor;
		while (i < count)
		{
			var t:Float = times[i];
			// Due check identical to the old full scan: controlled side
			// materializes once the hit window closes; auto side at t.
			var due:Bool = isControlled
				? songPosition > t + missEdge
				: songPosition >= t;
			if (!due) break; // times sorted — nothing later can be due
			if (sprites[i] == null)
			{
				if (budget <= 0) break; // resume here next tick
				materialize(i);
				budget--;
			}
			i++;
		}
		scanCursor = i;
		RaidProf.count("jscan", scanCursor - scannedFrom);
		compact(songPosition);
	}

	function compact(songPosition:Float):Void
	{
		var travel:Float = travelMs();
		var count:Int = datas.length;
		while (head < count && times[head] < songPosition - travel)
			head++;
		if (head >= 4096)
		{
			var removed:Int = head;
			datas.splice(0, head);
			times.splice(0, head);
			sprites.splice(0, head);
			checked.splice(0, head);
			head = 0;
			// Splice shifted every index down by `removed`.
			scanCursor = scanCursor > removed ? scanCursor - removed : 0;
		}
	}

	/** Milliseconds of song time for a note to scroll completely off screen. */
	function travelMs():Float
	{
		var pxPerMs:Float = 0.45 * strumline.scrollSpeed;
		if (pxPerMs <= 0.0001) return 60000;
		return (FlxG.height + 1024) / pxPerMs;
	}

	/**
	 * Comparison-mode verification: records were added alongside real
	 * sprites, so compare the engine's predicted outcome against the actual
	 * sprite judgement. Predictions: auto side hits at `t`, everyone misses
	 * at `t + HIT_WINDOW_MS`; controlled side hits must land inside the
	 * window. Mismatches are traced as `RAID-JUDGE-MISMATCH`.
	 */
	function verify(songPosition:Float, isControlled:Bool):Void
	{
		var count:Int = datas.length;
		var w:Float = Constants.HIT_WINDOW_MS;
		for (i in head...count)
		{
			if (checked[i]) continue;
			var s:Null<NoteSprite> = sprites[i];
			if (s == null) continue;
			if (s.noteData != datas[i]) { checked[i] = true; continue; }
			var t:Float = times[i];

			if (s.hasBeenHit)
			{
				checked[i] = true;
				actHits++;
				if (!isControlled)
				{
					// Auto side must hit at strumTime, inside the window.
					if (songPosition < t - w || songPosition > t + w)
					{
						mismatches++;
						trace('RAID-JUDGE-MISMATCH autohit-oob t=${datas[i].time} pos=${songPosition}');
					}
					else predHits++;
				}
				else
				{
					// Controlled side: hit must be inside the window.
					if (songPosition < t - w || songPosition > t + w)
					{
						mismatches++;
						trace('RAID-JUDGE-MISMATCH hit-oob t=${datas[i].time} pos=${songPosition}');
					}
					else predHits++;
				}
			}
			else if (s.hasMissed || s.handledMiss)
			{
				checked[i] = true;
				actMisses++;
				// Misses are only legal after the window closes.
				if (songPosition <= t + w)
				{
					mismatches++;
					trace('RAID-JUDGE-MISMATCH early-miss t=${datas[i].time} pos=${songPosition}');
				}
				else predMisses++;
			}
			else if (songPosition > t + w + 500)
			{
				// Far past the window with no outcome — sprite path stalled.
				checked[i] = true;
				mismatches++;
				trace('RAID-JUDGE-MISMATCH no-outcome t=${datas[i].time} pos=${songPosition}');
			}
		}

		if (songPosition - lastLogPos > 2000)
		{
			lastLogPos = songPosition;
			trace('RAID-JUDGE-CMP predH=$predHits actH=$actHits predM=$predMisses actM=$actMisses mism=$mismatches');
		}
	}
}
