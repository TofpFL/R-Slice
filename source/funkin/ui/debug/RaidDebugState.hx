package funkin.ui.debug;

#if (cpp && (windows || android))

import flixel.FlxG;
import flixel.FlxState;
import flixel.text.FlxText;
import funkin.external.raid.RaidEngine;
import openfl.display.Shape;

/**
 * Debug state for the RAID playback-engine spike (docs/ICHART_SOURCE.md
 * step 3): opens a .raid chart through the C ABI, advances a simulated
 * song clock, polls events each frame and draws upcoming notes.
 *
 * Build/run with `-DRAID` (see InitState). Chart path comes from the
 * RAID_CHART environment variable, else a built-in test file.
 * R = seek back to 0, ESC = back to title.
 */
class RaidDebugState extends FlxState
{
	var engine:cpp.Pointer<cpp.Void> = null;
	var overlay:Shape;
	var hud:FlxText;
	var songMs:Float = 0;
	var fedTotal:Float = 0;
	var lastLogSec:Int = -1;
	var path:String = "";
	var autoPlay:Bool = false;

	// Retained upcoming notes: [timeMs, lane, side, lengthMs] quads.
	var notes:Array<Float> = [];

	static inline var MAX_NOTES = 60000;
	static inline var KEEP_MS = 2000.0;

	override function create():Void
	{
		super.create();

		path = Sys.getEnv("RAID_CHART");
		if (path == null || path.length == 0)
			path = "raid_debug.raid";

		engine = RaidEngine.open(path);
		autoPlay = Sys.getEnv("RAID_PLAYSTATE") != null;

		overlay = new Shape();
		FlxG.game.addChild(overlay);

		hud = new FlxText(10, 10, 900, "", 16);
		add(hud);

		if (engine == null)
			hud.text = "raid open FAILED: " + path;
		else
		{
			var kindName:String = RaidEngine.kind(engine) == 1 ? "raid" : "json";
			hud.text = kindName + " engine: " + path +
				"  events=" + Std.int(RaidEngine.logicalEvents(engine)) +
				"  seekable=" + RaidEngine.seekable(engine);
		}
	}

	override function update(elapsed:Float):Void
	{
		super.update(elapsed);

		if (engine == null) return;

		if (FlxG.keys.justPressed.R)
		{
			songMs = 0;
			RaidEngine.seek(engine, 0);
			notes.resize(0);
		}
		if (FlxG.keys.justPressed.ESCAPE)
		{
			RaidEngine.close(engine);
			engine = null;
			FlxG.switchState(() -> new funkin.ui.title.TitleState());
			return;
		}

		// ENTER = launch the real PlayState with this chart streaming
		// through the engine (SongDifficulty.raidChartPath).
		// RAID_PLAYSTATE env var auto-launches on the first frame.
		if (FlxG.keys.justPressed.ENTER || autoPlay)
		{
			autoPlay = false;
			launchPlayState();
			return;
		}

		songMs += elapsed * 1000;
		RaidEngine.prefetch(engine, songMs, 16000);

		var n:Int = RaidEngine.poll(engine, songMs);
		fedTotal += n;

		for (i in 0...n)
		{
			notes.push(RaidEngine.field(i, 0));
			notes.push(RaidEngine.field(i, 1));
			notes.push(RaidEngine.field(i, 2));
			notes.push(RaidEngine.field(i, 3));
		}

		// Compact: drop notes more than KEEP_MS behind the playhead.
		var cutoff:Float = songMs - KEEP_MS;
		var w:Int = 0;
		var r:Int = 0;
		while (r + 3 < notes.length)
		{
			if (notes[r] >= cutoff)
			{
				notes[w] = notes[r];
				notes[w + 1] = notes[r + 1];
				notes[w + 2] = notes[r + 2];
				notes[w + 3] = notes[r + 3];
				w += 4;
			}
			r += 4;
		}
		notes.resize(w);

		hud.text = 't=' + Std.int(songMs) + 'ms  poll=' + n +
			'  fed=' + Std.int(fedTotal) + '  kept=' + (w / 4) +
			'  buf=' + notes.length / 4 + '   [ENTER=playstate R=restart ESC=exit]';

		var sec:Int = Std.int(songMs / 2000);
		if (sec != lastLogSec)
		{
			lastLogSec = sec;
			trace('[RAID] t=' + Std.int(songMs) + 'ms poll=' + n +
				' fed=' + Std.int(fedTotal) + ' kept=' + (w / 4));
		}

		drawOverlay();
	}

	function launchPlayState():Void
	{
		var donorId:String = Sys.getEnv("RAID_SONG_ID") ?? "roses";
		var song = funkin.external.raid.RaidChartStream.createSong(path, donorId);
		RaidEngine.close(engine);
		engine = null;
		trace('[RAID] launchPlayState botplay-env=' + Sys.getEnv("RAID_BOTPLAY"));
		FlxG.switchState(() -> new funkin.play.PlayState({
			targetSong: song,
			targetDifficulty: funkin.util.Constants.DEFAULT_DIFFICULTY,
			targetVariation: funkin.util.Constants.DEFAULT_VARIATION,
			botPlayMode: Sys.getEnv("RAID_BOTPLAY") != null,
			playtestResults: true
		}));
	}

	function drawOverlay():Void
	{
		overlay.graphics.clear();

		var pxPerMs:Float = 0.08;
		var laneH:Float = 26;
		var baseX:Float = 140;
		var baseY:Float = 120;
		var horizon:Float = 8000; // draw 8 s ahead

		var g = overlay.graphics;
		var count:Int = 0;

		for (i in 0...notes.length)
		{
			if ((i & 3) != 0) continue;
			var t:Float = notes[i];
			var dt:Float = t - songMs;
			if (dt < -500 || dt > horizon) continue;

			var lane:Int = Std.int(notes[i + 1]);
			var side:Int = Std.int(notes[i + 2]);
			var len:Float = notes[i + 3];

			var x:Float = baseX + dt * pxPerMs;
			var y:Float = baseY + lane * laneH + side * (laneH / 2);
			var w:Float = Math.max(3, len * pxPerMs);

			if (side == 1)
				g.beginFill(0x40FF40, 0.9);
			else if (side == 2)
				g.beginFill(0xFFDD40, 0.9);
			else
				g.beginFill(0xFF6040, 0.9);
			g.drawRect(x, y, w, laneH * 0.4);
			g.endFill();

			if (++count > 4000) break; // draw budget
		}
	}

	override function destroy():Void
	{
		if (engine != null) RaidEngine.close(engine);
		engine = null;
		if (overlay != null && overlay.parent != null) overlay.parent.removeChild(overlay);
		overlay = null;
		super.destroy();
	}
}

#end
