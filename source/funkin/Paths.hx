package funkin;

import flixel.graphics.frames.FlxAtlasFrames;
import animate.FlxAnimateFrames;
import funkin.graphics.FunkinSprite.AtlasSpriteSettings;
import openfl.utils.AssetType;
import funkin.util.macro.ConsoleMacro;
import haxe.io.Path;

/**
 * A core class which handles determining asset paths.
 */
@:nullSafety
class Paths implements ConsoleClass
{
  /**
   * Audio file extensions the engine can decode, in priority order.
   * Lime's AudioDecoder auto-detects the codec from file contents, so any of
   * these may be used for Inst/Voices/SFX/music assets regardless of name.
   * `.ogg` stays first so existing songs keep their current file when multiple
   * candidates exist.
   */
  public static final AUDIO_EXTENSIONS:Array<String> = ['ogg', 'wav', 'flac', 'mp3', 'opus'];

  static var currentLevel:Null<String> = null;

  public static function setCurrentLevel(name:Null<String>):Void
  {
    if (name == null)
    {
      currentLevel = null;
    }
    else
    {
      currentLevel = name.toLowerCase();
    }
  }

  public static function getCurrentLevel():Null<String>
  {
    return currentLevel;
  }

  public static function stripLibrary(path:String):String
  {
    var parts:Array<String> = path.split(':');
    if (parts.length < 2) return path;
    return parts[1];
  }

  public static function getLibrary(path:String):String
  {
    var parts:Array<String> = path.split(':');
    if (parts.length < 2) return 'preload';
    return parts[0];
  }

  static function getPath(file:String, type:AssetType, library:Null<String>):String
  {
    if (library != null) return getLibraryPath(file, library);

    if (currentLevel != null)
    {
      var levelPath:String = getLibraryPathForce(file, currentLevel);
      if (Assets.exists(levelPath, type)) return levelPath;
    }

    var levelPath:String = getLibraryPathForce(file, 'shared');
    if (Assets.exists(levelPath, type)) return levelPath;

    return getPreloadPath(file);
  }

  public static function getLibraryPath(file:String, library = 'preload'):String
  {
    return if (library == 'preload' || library == 'default') getPreloadPath(file); else getLibraryPathForce(file, library);
  }

  static inline function getLibraryPathForce(file:String, library:String):String
  {
    return '$library:assets/$library/$file';
  }

  static inline function getPreloadPath(file:String):String
  {
    return 'assets/$file';
  }

  public static function file(file:String, type:AssetType = TEXT, ?library:String):String
  {
    return getPath(file, type, library);
  }

  public static function animateAtlas(path:String, ?library:String):String
  {
    return getLibraryPath('images/$path', library);
  }

  public static function txt(key:String, ?library:String):String
  {
    return getPath('data/$key.txt', TEXT, library);
  }

  public static function frag(key:String, ?library:String):String
  {
    return getPath('shaders/$key.frag', TEXT, library);
  }

  public static function vert(key:String, ?library:String):String
  {
    return getPath('shaders/$key.vert', TEXT, library);
  }

  public static function xml(key:String, ?library:String):String
  {
    return getPath('data/$key.xml', TEXT, library);
  }

  public static function json(key:String, ?library:String):String
  {
    return getPath('data/$key.json', TEXT, library);
  }

  public static function srt(key:String, ?library:String, ?directory:String = 'data/'):String
  {
    return getPath('$directory$key.srt', TEXT, library);
  }

  /**
   * Resolve an audio asset that may use any supported extension.
   * `fileNoExt` is the full asset path WITHOUT extension (e.g.
   * `sounds/ui/confirmMenu` or `songs:assets/songs/bopeebo/Inst-erect`).
   * Probes `AUDIO_EXTENSIONS` in order and returns the first existing asset.
   * Falls back to `.ogg` (via `Constants.EXT_SOUND`) when nothing is found so
   * missing-file behavior and error messages stay unchanged.
   */
  static function resolveAudioPath(fileNoExt:String, type:AssetType, library:Null<String>):String
  {
    if (library != null)
    {
      for (ext in AUDIO_EXTENSIONS)
      {
        var candidate:String = getLibraryPath('$fileNoExt.$ext', library);
        if (Assets.exists(candidate, type)) return candidate;
      }
      return getLibraryPath('$fileNoExt.${Constants.EXT_SOUND}', library);
    }

    if (currentLevel != null)
    {
      for (ext in AUDIO_EXTENSIONS)
      {
        var levelPath:String = getLibraryPathForce('$fileNoExt.$ext', currentLevel);
        if (Assets.exists(levelPath, type)) return levelPath;
      }
    }

    for (ext in AUDIO_EXTENSIONS)
    {
      var sharedPath:String = getLibraryPathForce('$fileNoExt.$ext', 'shared');
      if (Assets.exists(sharedPath, type)) return sharedPath;
    }

    for (ext in AUDIO_EXTENSIONS)
    {
      var preloadPath:String = getPreloadPath('$fileNoExt.$ext');
      if (Assets.exists(preloadPath, type)) return preloadPath;
    }

    return getPreloadPath('$fileNoExt.${Constants.EXT_SOUND}');
  }

  /**
   * Resolve a `songs:`-library audio asset (Inst/Voices) by basename.
   * `baseNoExt` is the full asset ID without extension, e.g.
   * `songs:assets/songs/bopeebo/Inst-erect`.
   */
  static function resolveAudioAsset(baseNoExt:String):String
  {
    for (ext in AUDIO_EXTENSIONS)
    {
      var candidate:String = '$baseNoExt.$ext';
      if (Assets.exists(candidate)) return candidate;
    }
    return '$baseNoExt.${Constants.EXT_SOUND}';
  }

  public static function sound(key:String, ?library:String):String
  {
    return resolveAudioPath('sounds/$key', SOUND, library);
  }

  public static function soundRandom(key:String, min:Int, max:Int, ?library:String):String
  {
    return sound(key + FlxG.random.int(min, max), library);
  }

  public static function music(key:String, ?library:String):String
  {
    return resolveAudioPath('music/$key', MUSIC, library);
  }

  public static function videos(key:String, ?library:String):String
  {
    final path:Path = new Path(key);

    if (path.ext != null)
    {
      return getPath('videos/${path.file}.${path.ext}', BINARY, library ?? 'videos');
    }

    return getPath('videos/$key.${Constants.EXT_VIDEO}', BINARY, library ?? 'videos');
  }

  public static function voices(song:String, ?suffix:String = ''):String
  {
    if (suffix == null) suffix = ''; // no suffix, for a sorta backwards compatibility with older-ish voice files

    return resolveAudioAsset('songs:assets/songs/${song.toLowerCase()}/Voices$suffix');
  }

  /**
   * Gets the path to an `Inst.*` song instrumental from songs:assets/songs/`song`/
   * @param song name of the song to get instrumental for
   * @param suffix any suffix to add to end of song name, used for `-erect` variants usually
   * @param withExtension if it should return with the audio file extension (`.ogg`, `.wav`, `.mp3`, etc.).
   * @return String
   */
  public static function inst(song:String, ?suffix:String = '', withExtension:Bool = true):String
  {
    var base:String = 'songs:assets/songs/${song.toLowerCase()}/Inst$suffix';
    return withExtension ? resolveAudioAsset(base) : base;
  }

  public static function image(key:String, ?library:String):String
  {
    return getPath('images/$key.png', IMAGE, library);
  }

  public static function font(key:String):String
  {
    return 'assets/fonts/$key';
  }

  public static function ui(key:String, ?library:String):String
  {
    return xml('ui/$key', library);
  }

  public static function getSparrowAtlas(key:String, ?library:String):FlxAtlasFrames
  {
    return FlxAtlasFrames.fromSparrow(image(key, library), file('images/$key.xml', library));
  }

  public static function getAnimateAtlas(key:String, ?library:String, settings:AtlasSpriteSettings):FlxAnimateFrames
  {
    var assetLibrary:String = library ?? '';
    var graphicKey:String = '';

    if (assetLibrary != '')
    {
      graphicKey = Paths.animateAtlas(key, assetLibrary);
    }
    else
    {
      graphicKey = Paths.animateAtlas(key);
    }

    var validatedSettings:AtlasSpriteSettings = {
      swfMode: settings?.swfMode ?? false,
      cacheOnLoad: settings?.cacheOnLoad ?? false,
      filterQuality: settings?.filterQuality ?? MEDIUM,
      spritemaps: settings?.spritemaps ?? null,
      metadataJson: settings?.metadataJson ?? null,
      cacheKey: settings?.cacheKey ?? null,
      uniqueInCache: settings?.uniqueInCache ?? false,
      onSymbolCreate: settings?.onSymbolCreate ?? null,
      applyStageMatrix: settings?.applyStageMatrix ?? false,
      useRenderTexture: settings?.useRenderTexture ?? false
    };

    // Validate asset path.
    if (!Assets.exists('${graphicKey}/Animation.json'))
    {
      throw 'No Animation.json file exists at the specified path (${graphicKey})';
    }

    return FlxAnimateFrames.fromAnimate(graphicKey, validatedSettings.spritemaps, validatedSettings.metadataJson, validatedSettings.cacheKey,
      validatedSettings.uniqueInCache, {
        swfMode: validatedSettings.swfMode,
        cacheOnLoad: validatedSettings.cacheOnLoad,
        filterQuality: validatedSettings.filterQuality,
        onSymbolCreate: validatedSettings.onSymbolCreate
      });
  }

  public static function getPackerAtlas(key:String, ?library:String):FlxAtlasFrames
  {
    return FlxAtlasFrames.fromSpriteSheetPacker(image(key, library), file('images/$key.txt', library));
  }
}

enum abstract PathsFunction(String)
{
  public var MUSIC;
  public var INST;
  public var VOICES;
  public var SOUND;
}
