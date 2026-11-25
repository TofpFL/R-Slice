package funkin;

#if mobile
import funkin.mobile.ui.FunkinHitbox;
import funkin.mobile.util.InAppPurchasesUtil;
#end
import funkin.save.Save;
import funkin.util.WindowUtil;
import funkin.util.HapticUtil.HapticsMode;
import funkin.ui.debug.FunkinDebugDisplay.DebugDisplayMode;
#if FEATURE_DISCORD_RPC
import funkin.api.discord.DiscordClient;
#end

/**
 * A core class which provides a store of user-configurable, globally relevant values.
 */
@:nullSafety
class Preferences
{
  /**
   * FPS
   * Always the refresh rate of the display on mobile, or 60 on web.
   * @default `60`
   */
  public static var framerate(get, set):Int;

  static function get_framerate():Int
  {
    #if web
    return 60;
    #elseif mobile
    var refreshRate:Int = FlxG.stage.window.displayMode.refreshRate;

    if (refreshRate < 60) refreshRate = 60;

    return refreshRate;
    #else
    return Save?.instance?.options?.framerate ?? 60;
    #end
  }

  static function set_framerate(value:Int):Int
  {
    #if web
    return 60;
    #elseif mobile
    var refreshRate:Int = FlxG.stage.window.displayMode.refreshRate;

    if (refreshRate < 60) refreshRate = 60;

    return refreshRate;
    #else
    var save:Save = Save.instance;
    save.options.framerate = value;
    Save.system.flush();

    if (!unlockedFramerate)
    {
      FlxG.updateFramerate = value;
      FlxG.drawFramerate = value;
    }

    return value;
    #end
  }

  /**
   * Whether some particularly foul language is displayed.
   * @default `true`
   */
  public static var naughtyness(get, set):Bool;

  static function get_naughtyness():Bool
  {
    #if NO_FEATURE_NAUGHTYNESS
    return false;
    #else
    return Save?.instance?.options?.naughtyness ?? true;
    #end
  }

  static function set_naughtyness(value:Bool):Bool
  {
    #if NO_FEATURE_NAUGHTYNESS
    return false;
    #else
    var save:Save = Save.instance;
    save.options.naughtyness = value;
    Save.system.flush();
    return value;
    #end
  }

  /**
   * If enabled, the strumline is at the bottom of the screen rather than the top.
   * @default `false`
   */
  public static var downscroll(get, set):Bool;

  static function get_downscroll():Bool
  {
    return Save?.instance?.options?.downscroll #if mobile ?? true #else ?? false #end;
  }

  static function set_downscroll(value:Bool):Bool
  {
    var save:Save = Save.instance;
    save.options.downscroll = value;
    Save.system.flush();
    return value;
  }

  /**
   * If disabled, flashing lights in the main menu and other areas will be less intense.
   * @default `true`
   */
  public static var flashingLights(get, set):Bool;

  static function get_flashingLights():Bool
  {
    return Save?.instance?.options?.flashingLights ?? true;
  }

  static function set_flashingLights(value:Bool):Bool
  {
    var save:Save = Save.instance;
    save.options.flashingLights = value;
    Save.system.flush();
    return value;
  }

  /**
   * If disabled, the camera bump synchronized to the beat.
   * @default `false`
   */
  public static var zoomCamera(get, set):Bool;

  static function get_zoomCamera():Bool
  {
    return Save?.instance?.options?.zoomCamera ?? true;
  }

  static function set_zoomCamera(value:Bool):Bool
  {
    var save:Save = Save.instance;
    save.options.zoomCamera = value;
    Save.system.flush();
    return value;
  }

  /**
   * If enabled, an FPS and memory counter will be displayed even if this is not a debug build.
   * Always disabled on release mobile builds.
   * @default `Off`
   */
  public static var debugDisplay(get, set):DebugDisplayMode;

  static function get_debugDisplay():DebugDisplayMode
  {
    #if NO_FEATURE_DEBUG_DISPLAY
    return DebugDisplayMode.Off;
    #else
    return Save?.instance?.options?.debugDisplay ?? 'Off';
    #end
  }

  static function set_debugDisplay(value:DebugDisplayMode):DebugDisplayMode
  {
    #if NO_FEATURE_DEBUG_DISPLAY
    return DebugDisplayMode.Off;
    #else
    if (value != Save.instance.options.debugDisplay) setDebugDisplayMode(value);

    var save = Save.instance;
    save.options.debugDisplay = value;
    Save.system.flush();
    return value;
    #end
  }

  /**
   * Opacity of the debug display's background.
   * @default `50`
   */
  public static var debugDisplayBGOpacity(get, set):Int;

  static function get_debugDisplayBGOpacity():Int
  {
    return Save?.instance?.options?.debugDisplayBGOpacity ?? 50;
  }

  static function set_debugDisplayBGOpacity(value:Int):Int
  {
    setDebugDisplayBGOpacity(value / 100);

    var save:Save = Save.instance;
    save.options.debugDisplayBGOpacity = value;
    Save.system.flush();
    return value;
  }

  /**
   * If enabled, haptic feedback will be enabled.
   * @default `All`
   */
  public static var hapticsMode(get, set):HapticsMode;

  static function get_hapticsMode():HapticsMode
  {
    var value = Save?.instance?.options?.hapticsMode ?? 'All';

    return switch (value)
    {
      case 'None':
        HapticsMode.NONE;
      case 'Notes Only':
        HapticsMode.NOTES_ONLY;
      default:
        HapticsMode.ALL;
    };
  }

  static function set_hapticsMode(value:HapticsMode):HapticsMode
  {
    var string;

    switch (value)
    {
      case HapticsMode.NONE:
        string = 'None';
      case HapticsMode.NOTES_ONLY:
        string = 'Notes Only';
      default:
        string = 'All';
    };

    var save:Save = Save.instance;
    save.options.hapticsMode = string;
    Save.system.flush();
    return value;
  }

  /**
   * Multiplier of intensity for all the haptic feedback effects.
   * @default `2.5`
   */
  public static var hapticsIntensityMultiplier(get, set):Float;

  static function get_hapticsIntensityMultiplier():Float
  {
    return Save?.instance?.options?.hapticsIntensityMultiplier ?? 1;
  }

  static function set_hapticsIntensityMultiplier(value:Float):Float
  {
    var save:Save = Save.instance;
    save.options.hapticsIntensityMultiplier = value;
    Save.system.flush();
    return value;
  }

  /**
   * If enabled, the game will automatically pause when tabbing out.
   * Always enabled on mobile.
   * @default `true`
   */
  public static var autoPause(get, set):Bool;

  static function get_autoPause():Bool
  {
    #if mobile
    return false;
    #else
    return Save?.instance?.options?.autoPause ?? true;
    #end
  }

  static function set_autoPause(value:Bool):Bool
  {
    #if mobile
    return false;
    #else
    if (value != Save.instance.options.autoPause) FlxG.autoPause = value;

    var save:Save = Save.instance;
    save.options.autoPause = value;
    Save.system.flush();
    return value;
    #end
  }

  /**
   * If enabled, the game will automatically launch in fullscreen on startup.
   * @default `true`
   */
  public static var autoFullscreen(get, set):Bool;

  static function get_autoFullscreen():Bool
  {
    return Save?.instance?.options?.autoFullscreen ?? true;
  }

  static function set_autoFullscreen(value:Bool):Bool
  {
    var save:Save = Save.instance;
    save.options.autoFullscreen = value;
    Save.system.flush();
    return value;
  }

  /**
   * A global audio offset in milliseconds.
   * This is used to sync the audio.
   * @default `0`
   */
  public static var globalOffset(get, set):Int;

  static function get_globalOffset():Int
  {
    return Save?.instance?.options?.globalOffset ?? 0;
  }

  static function set_globalOffset(value:Int):Int
  {
    var save:Save = Save.instance;
    save.options.globalOffset = value;
    Save.system.flush();
    return value;
  }

  /**
   * If enabled, the game will utilize VSync (or adaptive VSync) on startup.
   * @default `OFF`
   */
  public static var vsyncMode(get, set):lime.ui.WindowVSyncMode;

  static function get_vsyncMode():lime.ui.WindowVSyncMode
  {
    #if (mobile || web)
    return lime.ui.WindowVSyncMode.OFF;
    #else
    var value = Save?.instance?.options?.vsyncMode ?? 'Off';

    return switch (value)
    {
      case 'Off':
        lime.ui.WindowVSyncMode.OFF;
      case 'On':
        lime.ui.WindowVSyncMode.ON;
      case 'Adaptive':
        lime.ui.WindowVSyncMode.ADAPTIVE;
      default:
        lime.ui.WindowVSyncMode.OFF;
    };
    #end
  }

  static function set_vsyncMode(value:lime.ui.WindowVSyncMode):lime.ui.WindowVSyncMode
  {
    #if (mobile || web)
    return lime.ui.WindowVSyncMode.OFF;
    #else
    var string;

    switch (value)
    {
      case lime.ui.WindowVSyncMode.OFF:
        string = 'Off';
      case lime.ui.WindowVSyncMode.ON:
        string = 'On';
      case lime.ui.WindowVSyncMode.ADAPTIVE:
        string = 'Adaptive';
      default:
        string = 'Off';
    };

    WindowUtil.setVSyncMode(value);

    var save:Save = Save.instance;
    save.options.vsyncMode = string;
    Save.system.flush();
    return value;
    #end
  }

  public static var unlockedFramerate(get, set):Bool;

  static function get_unlockedFramerate():Bool
  {
    #if (mobile || web)
    return false;
    #else
    return Save?.instance?.options?.unlockedFramerate ?? false;
    #end
  }

  static function set_unlockedFramerate(value:Bool):Bool
  {
    #if (mobile || web)
    return false;
    #else
    if (value != Save.instance.options.unlockedFramerate)
    {
      toggleFramerateCap(value);
    }

    var save:Save = Save.instance;
    save.options.unlockedFramerate = value;
    Save.system.flush();
    return value;
    #end
  }

  public static var enabledDiscordRPC(get, set):Bool;

  static function get_enabledDiscordRPC():Bool
  {
    return Save?.instance?.options?.enabledDiscordRPC ?? true;
  }

  static function set_enabledDiscordRPC(value:Bool):Bool
  {
    #if FEATURE_DISCORD_RPC
    toggleDiscordRPC(value);
    #end

    var save:Save = Save.instance;
    save.options.enabledDiscordRPC = value;
    Save.system.flush();
    return value;
  }

  #if FEATURE_DISCORD_RPC
  public static function toggleDiscordRPC(enable:Bool)
  {
    if (DiscordClient.instance == null) return;

    if (enable)
    {
      DiscordClient.instance.init();

      if (DiscordClient.presenceParamsCache != null)
      {
        DiscordClient.instance.setPresence(DiscordClient.presenceParamsCache);
      }
    }
    else
    {
      DiscordClient.instance.shutdown();
    }
  }
  #end

  /**
   * If >0, the game will display a semi-opaque background under the notes.
   * `0` for no background, `100` for solid black if you're freaky like that
   * @default `0`
   */
  public static var strumlineBackgroundOpacity(get, set):Int;

  static function get_strumlineBackgroundOpacity():Int
  {
    return (Save?.instance?.options?.strumlineBackgroundOpacity ?? 0);
  }

  static function set_strumlineBackgroundOpacity(value:Int):Int
  {
    var save:Save = Save.instance;
    save.options.strumlineBackgroundOpacity = value;
    Save.system.flush();
    return value;
  }

  /**
   * If enabled, the game will hide the mouse when taking a screenshot.
   * @default `true`
   */
  public static var shouldHideMouse(get, set):Bool;

  static function get_shouldHideMouse():Bool
  {
    return Save?.instance?.options?.screenshot?.shouldHideMouse ?? true;
  }

  static function set_shouldHideMouse(value:Bool):Bool
  {
    var save:Save = Save.instance;
    save.options.screenshot.shouldHideMouse = value;
    Save.system.flush();
    return value;
  }

  /**
   * If enabled, the game will show a preview after taking a screenshot.
   * @default `true`
   */
  public static var fancyPreview(get, set):Bool;

  static function get_fancyPreview():Bool
  {
    return Save?.instance?.options?.screenshot?.fancyPreview ?? true;
  }

  static function set_fancyPreview(value:Bool):Bool
  {
    var save:Save = Save.instance;
    save.options.screenshot.fancyPreview = value;
    Save.system.flush();
    return value;
  }

  /**
   * If enabled, the game will show the preview only after a screenshot is saved.
   * @default `true`
   */
  public static var previewOnSave(get, set):Bool;

  static function get_previewOnSave():Bool
  {
    return Save?.instance?.options?.screenshot?.previewOnSave ?? true;
  }

  static function set_previewOnSave(value:Bool):Bool
  {
    var save:Save = Save.instance;
    save.options.screenshot.previewOnSave = value;
    Save.system.flush();
    return value;
  }

  /**
   * Loads the user's preferences from the save data and apply them.
   */
  public static function init():Void
  {
    // Apply the autoPause setting (enables automatic pausing on focus lost).
    FlxG.autoPause = Preferences.autoPause;

    // Apply the debugDisplay setting (enables the FPS and RAM display).
    setDebugDisplayMode(Preferences.debugDisplay);
    setDebugDisplayBGOpacity(Preferences.debugDisplayBGOpacity / 100);

    toggleFramerateCap(Preferences.unlockedFramerate);

    #if mobile
    // Apply the allowScreenTimeout setting.
    lime.system.System.allowScreenTimeout = Preferences.screenTimeout;
    #end
  }

  static function toggleFramerateCap(unlocked:Bool):Void
  {
    #if !(mobile || web)
    FlxG.drawFramerate = unlocked ? 0 : framerate;
    FlxG.updateFramerate = unlocked ? 0 : framerate;
    #end
  }

  public static function setDebugDisplayMode(mode:DebugDisplayMode):Void
  {
    if (FlxG.game.contains(Main.debugDisplay)) FlxG.game.removeChild(Main.debugDisplay);

    if (mode == DebugDisplayMode.Off) return;

    Main.debugDisplay.isAdvanced = (mode == DebugDisplayMode.Advanced);

    FlxG.game.addChild(Main.debugDisplay);
  }

  static function setDebugDisplayBGOpacity(value:Float):Void
  {
    if (Main.debugDisplay == null) return;

    Main.debugDisplay.backgroundOpacity = value;
  }

  /**
   * If enabled, subtitles will appear during some songs and cutscenes.
   * @default `true`
   */
  public static var subtitles(get, set):Bool;

  static function get_subtitles():Bool
  {
    return Save?.instance?.options?.subtitles ?? true;
  }

  static function set_subtitles(value:Bool):Bool
  {
    var save:Save = Save.instance;
    save.options.subtitles = value;
    Save.system.flush();
    return value;
  }

  #if mobile
  /**
   * If enabled, device will be able to sleep on its own.
   * @default `false`
   */
  public static var screenTimeout(get, set):Bool;

  static function get_screenTimeout():Bool
  {
    return Save?.instance?.mobileOptions?.screenTimeout ?? false;
  }

  static function set_screenTimeout(value:Bool):Bool
  {
    if (value != Save.instance.mobileOptions.screenTimeout) lime.system.System.allowScreenTimeout = value;

    var save:Save = Save.instance;
    save.mobileOptions.screenTimeout = value;
    Save.system.flush();
    return value;
  }

  /**
   * Controls Scheme for the hitbox.
   * @default `4 Lanes`
   */
  public static var controlsScheme(get, set):String;

  static function get_controlsScheme():String
  {
    return Save?.instance?.mobileOptions?.controlsScheme ?? FunkinHitboxControlSchemes.Arrows;
  }

  static function set_controlsScheme(value:String):String
  {
    var save:Save = Save.instance;
    save.mobileOptions.controlsScheme = value;
    Save.system.flush();
    return value;
  }

  #if FEATURE_MOBILE_IAP
  /**
   * If bought, the game will not show any ads.
   * @default `false`
   */
  @:unreflective
  public static var noAds(get, set):Bool;

  @:unreflective
  static function get_noAds():Bool
  {
    if (InAppPurchasesUtil.hasInitialized) noAds = InAppPurchasesUtil.isPurchased(InAppPurchasesUtil.UPGRADE_PRODUCT_ID);
    var returnedValue = Save?.instance?.mobileOptions?.noAds ?? false;
    return returnedValue;
  }

  @:unreflective
  static function set_noAds(value:Bool):Bool
  {
    var save:Save = Save.instance;
    save.mobileOptions.noAds = value;
    Save.system.flush();
    return value;
  }
  #end
  #end

  // ===== R-Slice style gameplay, performance and HUD options =====
  // Backed by optional SaveDataOptions fields; accessors supply defaults for old saves.

  /** Ghost tapping: allow inputs before notes reach receptors. @default `true` */
  public static var ghostTapping(get, set):Bool;
  static function get_ghostTapping():Bool return Save?.instance?.options?.ghostTapping ?? true;
  static function set_ghostTapping(v:Bool):Bool { var s:Save = Save.instance; s.options.ghostTapping = v; Save.system.flush(); return v; }

  /** Note splashes on hit. @default `true` */
  public static var noteSplashes(get, set):Bool;
  static function get_noteSplashes():Bool return Save?.instance?.options?.noteSplashes ?? true;
  static function set_noteSplashes(v:Bool):Bool { var s:Save = Save.instance; s.options.noteSplashes = v; Save.system.flush(); return v; }

  /** Show opponent notes/receptors. @default `true` */
  public static var opponentNotes(get, set):Bool;
  static function get_opponentNotes():Bool return Save?.instance?.options?.opponentNotes ?? true;
  static function set_opponentNotes(v:Bool):Bool { var s:Save = Save.instance; s.options.opponentNotes = v; Save.system.flush(); return v; }

  /** Hide gameplay HUD. @default `false` */
  public static var hideHud(get, set):Bool;
  static function get_hideHud():Bool return Save?.instance?.options?.hideHud ?? false;
  static function set_hideHud(v:Bool):Bool { var s:Save = Save.instance; s.options.hideHud = v; Save.system.flush(); return v; }

  /** Post-processing shaders / screen effects. @default `true` */
  public static var shaders(get, set):Bool;
  static function get_shaders():Bool return Save?.instance?.options?.shaders ?? true;
  static function set_shaders(v:Bool):Bool { var s:Save = Save.instance; s.options.shaders = v; Save.system.flush(); return v; }

  /** Low detail mode for weak GPUs. @default `false` */
  public static var lowDetail(get, set):Bool;
  static function get_lowDetail():Bool return Save?.instance?.options?.lowDetail ?? false;
  static function set_lowDetail(v:Bool):Bool { var s:Save = Save.instance; s.options.lowDetail = v; Save.system.flush(); return v; }

  /** Per-note Lua/script callbacks. @default `true` */
  public static var luaNoteCallbacks(get, set):Bool;
  static function get_luaNoteCallbacks():Bool return Save?.instance?.options?.luaNoteCallbacks ?? true;
  static function set_luaNoteCallbacks(v:Bool):Bool { var s:Save = Save.instance; s.options.luaNoteCallbacks = v; Save.system.flush(); return v; }

  /** Notes emanate from screen middle. @default `false` */
  public static var middleScroll(get, set):Bool;
  static function get_middleScroll():Bool return Save?.instance?.options?.middleScroll ?? false;
  static function set_middleScroll(v:Bool):Bool { var s:Save = Save.instance; s.options.middleScroll = v; Save.system.flush(); return v; }

  /** Force charts to 4 lanes. @default `false` */
  public static var extraKeys(get, set):Bool;
  static function get_extraKeys():Bool return Save?.instance?.options?.extraKeys ?? false;
  static function set_extraKeys(v:Bool):Bool { var s:Save = Save.instance; s.options.extraKeys = v; Save.system.flush(); return v; }

  /** Global scroll-speed multiplier; `0` = chart speed. @default `1` */
  public static var scrollSpeedMultiplier(get, set):Float;
  static function get_scrollSpeedMultiplier():Float return Save?.instance?.options?.scrollSpeedMultiplier ?? 1.0;
  static function set_scrollSpeedMultiplier(v:Float):Float { var s:Save = Save.instance; s.options.scrollSpeedMultiplier = v; Save.system.flush(); return v; }

  /** Removes the scroll-speed multiplier cap (extends range to 50x) for chart inspection. @default `false` */
  public static var unlimitedScrollSpeed(get, set):Bool;
  static function get_unlimitedScrollSpeed():Bool return Save?.instance?.options?.unlimitedScrollSpeed ?? false;
  static function set_unlimitedScrollSpeed(v:Bool):Bool { var s:Save = Save.instance; s.options.unlimitedScrollSpeed = v; Save.system.flush(); return v; }

  /** Note render detail: `Low`, `Medium`, `High`. @default `High` */
  public static var noteDetail(get, set):String;
  static function get_noteDetail():String return Save?.instance?.options?.noteDetail ?? 'High';
  static function set_noteDetail(v:String):String { var s:Save = Save.instance; s.options.noteDetail = v; Save.system.flush(); return v; }

  /** Note counter: `Off`, `Player`, `Both`. @default `Off` */
  public static var noteCounter(get, set):String;
  static function get_noteCounter():String return Save?.instance?.options?.noteCounter ?? 'Off';
  static function set_noteCounter(v:String):String { var s:Save = Save.instance; s.options.noteCounter = v; Save.system.flush(); return v; }

  /** Stack combo popups. @default `true` */
  public static var comboStacking(get, set):Bool;
  static function get_comboStacking():Bool return Save?.instance?.options?.comboStacking ?? true;
  static function set_comboStacking(v:Bool):Bool { var s:Save = Save.instance; s.options.comboStacking = v; Save.system.flush(); return v; }

  /** Hit rating popups. @default `true` */
  public static var hitPopups(get, set):Bool;
  static function get_hitPopups():Bool return Save?.instance?.options?.hitPopups ?? true;
  static function set_hitPopups(v:Bool):Bool { var s:Save = Save.instance; s.options.hitPopups = v; Save.system.flush(); return v; }

  /** Score text scale. @default `1` */
  public static var scoreZoom(get, set):Float;
  static function get_scoreZoom():Float return Save?.instance?.options?.scoreZoom ?? 1.0;
  static function set_scoreZoom(v:Float):Float { var s:Save = Save.instance; s.options.scoreZoom = v; Save.system.flush(); return v; }

  /** Top time bar: `Off`, `Time Left`, `Elapsed`, `Song`. @default `Off` */
  public static var timeBar(get, set):String;
  static function get_timeBar():String return Save?.instance?.options?.timeBar ?? 'Off';
  static function set_timeBar(v:String):String { var s:Save = Save.instance; s.options.timeBar = v; Save.system.flush(); return v; }

  /** Health bar opacity percent. @default `100` */
  public static var healthBarAlpha(get, set):Int;
  static function get_healthBarAlpha():Int return Save?.instance?.options?.healthBarAlpha ?? 100;
  static function set_healthBarAlpha(v:Int):Int { var s:Save = Save.instance; s.options.healthBarAlpha = v; Save.system.flush(); return v; }

  /** Forced note skin ID. Empty = use each chart's own noteskin. @default `` */
  public static var noteSkin(get, set):String;
  static function get_noteSkin():String return Save?.instance?.options?.noteSkin ?? '';
  static function set_noteSkin(v:String):String { var s:Save = Save.instance; s.options.noteSkin = v; Save.system.flush(); return v; }

  /** Center the gameplay composition horizontally without moving the HUD. @default `false` */
  public static var showcaseMode(get, set):Bool;
  static function get_showcaseMode():Bool return Save?.instance?.options?.showcaseMode ?? false;
  static function set_showcaseMode(v:Bool):Bool { var s:Save = Save.instance; s.options.showcaseMode = v; Save.system.flush(); return v; }

  /** Bot Play: the player side hits every note perfectly. Scores are invalidated. @default `false` */
  public static var botPlay(get, set):Bool;
  static function get_botPlay():Bool return Save?.instance?.options?.botPlay ?? false;
  static function set_botPlay(v:Bool):Bool { var s:Save = Save.instance; s.options.botPlay = v; Save.system.flush(); return v; }

  /** Bot Play display text. @default `'BOTPLAY'` */
  public static var botPlayText(get, set):String;
  static function get_botPlayText():String return Save?.instance?.options?.botPlayText ?? 'BOTPLAY';
  static function set_botPlayText(v:String):String { var s:Save = Save.instance; s.options.botPlayText = v; Save.system.flush(); return v; }
}
