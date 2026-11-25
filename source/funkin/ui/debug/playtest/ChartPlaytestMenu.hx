package funkin.ui.debug.playtest;

#if sys
import flixel.text.FlxText;
import flixel.util.FlxColor;
import flixel.util.FlxTimer;
import flixel.tweens.FlxTween;
import funkin.util.file.FNFCUtil;
import funkin.play.song.Song;
import funkin.graphics.FunkinSprite;
import funkin.graphics.FunkinCamera;
import funkin.ui.debug.playtest.ChartPlaytestMenuButton;
import funkin.ui.debug.playtest.ChartPlaytestMenuButton.ChartPlaytestMenuButtonListToggle;
#if NO_FEATURE_TOUCH_CONTROLS
import funkin.input.Cursor;
#end

/**
 * When playtesting from an FNFC file, we display a debug UI to choose a difficulty first.
 */
class ChartPlaytestMenu extends MusicBeatState
{
  var filePath:String;
  var targetSong:Song;
  var songName:FlxText;
  var variationButton:ChartPlaytestMenuButtonListToggle;
  var difficultyButton:ChartPlaytestMenuButtonListToggle;
  var playtestButton:ChartPlaytestMenuButton;
  var currentVariation:String;
  var currentDifficulty:String;
  var playtestCam:FunkinCamera;

  public function new(filePath:String)
  {
    final loadedSong:Song = FNFCUtil.loadSongFromFNFCPath(filePath);

    super();

    this.targetSong = loadedSong;

    FlxG.state.persistentDraw = true;
    FlxG.state.persistentUpdate = false;

    playtestCam = new FunkinCamera('playtestCam');
    playtestCam.bgColor = 0x0;
    // playtestCam.alpha = 0;
    FlxG.cameras.add(playtestCam, false);

    var blackBG:FunkinSprite = new FunkinSprite(0, 0);
    blackBG.makeSolidColor(FlxG.width * 3, FlxG.height * 3, FlxColor.BLACK);
    blackBG.scrollFactor.set();
    blackBG.alpha = 0.4;
    blackBG.cameras = [playtestCam];
    add(blackBG);

    this.filePath = filePath;

    songName = new FlxText(0, FlxG.height * 0.1, 0, 'Loaded Song: ${targetSong.songName}',
      30).setFormat(Paths.font('vcr.ttf'), 50, FlxColor.WHITE, FlxTextAlign.CENTER);
    songName.screenCenter(X);
    songName.cameras = [playtestCam];
    add(songName);

    variationButton = new ChartPlaytestMenuButtonListToggle(0, FlxG.height * 0.55, "Variation", targetSong.variations, function(value:String)
    {
      currentVariation = value;
      variationButton.screenCenter(X);
    });
    variationButton.screenCenter(X);
    variationButton.cameras = [playtestCam];
    add(variationButton);

    difficultyButton = new ChartPlaytestMenuButtonListToggle(0, FlxG.height * 0.45, "Difficulty",
      targetSong.listDifficulties(null, targetSong.variations, true, true), function(value:String)
    {
      currentDifficulty = value;
      difficultyButton.screenCenter(X);
    });
    difficultyButton.screenCenter(X);
    difficultyButton.cameras = [playtestCam];
    add(difficultyButton);

    playtestButton = new ChartPlaytestMenuButton(0, FlxG.height * 0.8, "Playtest Song", function()
    {
      try
      {
        FNFCUtil.playSongFromFNFCPath(filePath, currentDifficulty, currentVariation);
      }
      catch (e)
      {
        lime.app.Application.current.window.alert('$e', 'Could Not Playtest Chart');
      }
    });
    playtestButton.screenCenter(X);
    playtestButton.cameras = [playtestCam];
    add(playtestButton);

    // Auto-start the playtest after a short delay so `--song` launches
    // reach gameplay without requiring UI interaction. Any difficulty /
    // variation picked before the timer fires is still respected.
    new FlxTimer().start(2.0, function(_)
    {
      trace('ChartPlaytestMenu: auto-playtest timer fired, state=${FlxG.state}');
      if (FlxG.state != this) return;
      var diff = currentDifficulty;
      var vari = currentVariation;
      if (diff == null || vari == null)
      {
        if (vari == null) vari = (targetSong.variations.length > 0) ? targetSong.variations[0] : null;
        if (diff == null)
        {
          var diffs = targetSong.listDifficulties(null, targetSong.variations, true, true);
          diff = (diffs.length > 0) ? diffs[0] : null;
        }
      }
      trace('ChartPlaytestMenu: playtesting diff=$diff vari=$vari');
      try
      {
        FNFCUtil.playSongFromFNFCPath(filePath, diff, vari);
        trace('ChartPlaytestMenu: playSongFromFNFCPath returned');
      }
      catch (e)
      {
        trace('ChartPlaytestMenu: playtest failed: $e');
        lime.app.Application.current.window.alert('$e', 'Could Not Playtest Chart');
      }
    });

    FlxTween.tween(playtestCam, {alpha: 1}, 0.5);

    #if NO_FEATURE_TOUCH_CONTROLS
    Cursor.show();
    #end
  }

  override function update(elapsed:Float):Void
  {
    // PlayState calls FlxG.cameras.reset() on entry, which destroys this
    // menu's camera (scroll is nulled on destroy). When the playtest
    // substate closes we resume with buttons still bound to the dead
    // camera, so rebuild it before children update.
    if (playtestCam.scroll == null)
    {
      playtestCam = new FunkinCamera('playtestCam');
      playtestCam.bgColor = 0x0;
      FlxG.cameras.add(playtestCam, false);
      for (member in members)
      {
        if (member != null) member.cameras = [playtestCam];
      }
    }
    super.update(elapsed);
  }
}
#end
