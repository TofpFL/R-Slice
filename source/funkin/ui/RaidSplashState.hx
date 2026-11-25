package funkin.ui;

import flixel.FlxG;
import flixel.FlxSprite;
import flixel.FlxState;
import flixel.sound.FlxSound;
import flixel.text.FlxText;
import flixel.util.FlxColor;
import flixel.addons.transition.FlxTransitionableState;
import funkin.Paths;

#if hxvlc
import funkin.graphics.video.FunkinVideoSprite;
#end

/**
 * R-Slice boot splash: plays assets/videos/videos/startup.mp4 through
 * hxvlc (the same VideoPlayer path R-Slice uses natively), with a
 * sprite-sheet fallback (raidSplash{0..3}) when video playback is
 * compiled out.
 *
 * ENTER / SPACE / ESC / click skips once playback has started;
 * video end or load failure advances to TitleState.
 */
class RaidSplashState extends FlxState
{
	#if hxvlc
	static inline var SHEET_COUNT:Int = 0;
	#else
	static inline var SHEET_COUNT:Int = 4;
	#end
	static inline var FRAME_RATE:Float = 24;

	#if hxvlc
	var vid:FunkinVideoSprite;
	#else
	var sprites:Array<FlxSprite> = [];
	var sheetIndex:Int = 0;
	#end
	var bg:FlxSprite;
	var hint:FlxText;
	var sfx:FlxSound;
	var canSkip:Bool = false;
	var leaving:Bool = false;
	var loadFailed:Bool = false;

	override function create():Void
	{
		super.create();

		// The camera persists across states — clear any follow target and
		// reset the view so splash coordinates map 1:1 to the screen.
		FlxG.camera.target = null;
		FlxG.camera.scroll.set(0, 0);
		FlxG.camera.zoom = 1;

		bg = new FlxSprite(0, 0).makeGraphic(FlxG.width, FlxG.height, FlxColor.BLACK);
		bg.scrollFactor.set(0, 0);
		add(bg);

		#if hxvlc
		playVideo();
		#else
		playSheets();
		playSplashAudio();
		#end

		hint = new FlxText(0, FlxG.height - 40, FlxG.width - 20, 'PRESS ENTER TO SKIP', 16);
		hint.scrollFactor.set(0, 0);
		hint.alignment = FlxTextAlign.RIGHT;
		hint.color = 0x80FFFFFF;
		add(hint);
	}

	#if !hxvlc
	function playSplashAudio():Void
	{
		var path:String = Paths.music('raidSplash/raidSplash');
		try
		{
			sfx = FlxG.sound.play(path, 0.9, false, null, true);
		}
		catch (e:Dynamic)
		{
			trace('RAID splash: audio failed to play ($path)');
		}
	}
	#end

	#if hxvlc
	function playVideo():Void
	{
		vid = new FunkinVideoSprite(0, 0);
		vid.zIndex = 0;
		vid.active = false;

		vid.bitmap.onFormatSetup.add(function():Void
		{
			if (vid != null && vid.bitmap != null && vid.bitmap.bitmapData != null)
			{
				final bw:Float = vid.bitmap.bitmapData.width;
				final bh:Float = vid.bitmap.bitmapData.height;
				final sc:Float = Math.min(FlxG.width / bw, FlxG.height / bh);
				vid.setGraphicSize(bw * sc, bh * sc);
				vid.updateHitbox();
				vid.screenCenter();
			}
		});

		vid.bitmap.onEncounteredError.add(function(msg:String):Void
		{
			trace('RAID splash: video error: $msg');
			loadFailed = true;
		});

		vid.bitmap.onEndReached.add(function():Void
		{
			leave();
		});

		add(vid);

		var path:String = Paths.videos('startup');
		if (vid.load(path, []) && vid.play())
		{
			canSkip = true;
			trace('RAID splash: video playing ($path)');
		}
		else
		{
			trace('RAID splash: video load/play failed ($path)');
			loadFailed = true;
		}
	}
	#end

	#if !hxvlc
	function playSheets():Void
	{
		var fw:Float = 0;
		var fh:Float = 0;
		var firstNames = ['f0000', 'f0028', 'f0056', 'f0084'];
		for (i in 0...SHEET_COUNT)
		{
			var atlas = Paths.getSparrowAtlas('raidSplash$i');
			var probe = atlas != null ? atlas.getByName(firstNames[i]) : null;
			if (probe == null)
			{
				trace('RAID splash: raidSplash$i atlas/frame missing, skipping sheet');
				continue;
			}
			if (fw == 0) { fw = probe.sourceSize.x; fh = probe.sourceSize.y; }

			var sprite = new FlxSprite(0, 0);
			sprite.frames = atlas;
			sprite.scrollFactor.set(0, 0);
			sprite.animation.addByPrefix('splash', 'f', FRAME_RATE, false);
			sprite.visible = (sprites.length == 0);
			sprites.push(sprite);
		}

		if (sprites.length == 0 || fw == 0)
		{
			trace('RAID splash: no playable sheets, going straight to title');
			canSkip = true;
			return;
		}

		var sc:Float = Math.max(FlxG.width / fw, FlxG.height / fh);
		for (sprite in sprites)
		{
			sprite.origin.set(0, 0);
			sprite.scale.set(sc, sc);
			sprite.x = (FlxG.width - fw * sc) / 2;
			sprite.y = (FlxG.height - fh * sc) / 2;
			add(sprite);
		}
		sprites[0].animation.play('splash');
		canSkip = true;
	}
	#end

	function leave():Void
	{
		if (leaving) return;
		leaving = true;

		#if hxvlc
		if (vid != null) vid.stop();
		#end
		#if !hxvlc
		if (sfx != null) sfx.stop();
		#end

		FlxTransitionableState.skipNextTransIn = true;
		FlxG.switchState(() -> new funkin.ui.title.TitleState());
	}

	override function update(elapsed:Float):Void
	{
		super.update(elapsed);

		if (leaving) return;

		#if !hxvlc
		if (sprites.length == 0) { leave(); return; }
		var cur = sprites[sheetIndex];
		if (cur != null && cur.animation != null && cur.animation.curAnim != null
			&& cur.animation.curAnim.finished)
		{
			cur.visible = false;
			sheetIndex++;
			if (sheetIndex < sprites.length)
			{
				sprites[sheetIndex].visible = true;
				sprites[sheetIndex].animation.play('splash');
			}
		}
		#end

		var skip:Bool = FlxG.keys.justPressed.ENTER || FlxG.keys.justPressed.SPACE
			|| FlxG.keys.justPressed.ESCAPE || FlxG.mouse.justPressed;

		#if hxvlc
		if (loadFailed) leave();
		else if ((canSkip && skip)) leave();
		#else
		if ((canSkip && skip) || sheetIndex >= sprites.length) leave();
		#end
	}
}
