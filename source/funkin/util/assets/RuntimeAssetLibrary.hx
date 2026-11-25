package funkin.util.assets;

import lime.app.Future;
import lime.graphics.Image;
import lime.media.AudioBuffer;
import lime.text.Font;
import lime.utils.AssetLibrary;
import lime.utils.AssetType;
import lime.utils.Bytes;
#if sys
import lime.utils.Assets;
import sys.FileSystem;
import funkin.util.FileUtil;
#end

/**
 * Filesystem-backed asset layer.
 *
 * OpenFL/lime resolves assets through build-time manifests (`bin/manifest/*.json`),
 * so files dropped next to the executable at runtime are invisible to
 * `Assets.exists`/`getText`/`getSound`/etc. This library wraps each preloaded
 * manifest library: lookups check the real `assets/` directory on disk first
 * (covering runtime-added songs, noteskins data, levels, albums, media), then
 * delegate to the original packed library for bundled content.
 *
 * Asset IDs keep their usual `library:symbol` form; symbol names are always
 * `assets/...` paths relative to the game directory, so a single filesystem
 * index serves every library namespace.
 */
class RuntimeAssetLibrary extends AssetLibrary
{
  var inner:Null<AssetLibrary>;

  static var installed:Bool = false;
  static var scanned:Bool = false;
  static var fsPaths:Map<String, String> = new Map();
  static var fsTypes:Map<String, AssetType> = new Map();

  public function new(inner:Null<AssetLibrary>)
  {
    super();
    this.inner = inner;
  }

  /**
   * Wrap every already-loaded asset library so runtime files resolve through
   * the normal `Assets.*` API. Must run after the preloader has registered the
   * manifest libraries and before registries scan `data/`.
   */
  public static function install():Void
  {
    #if sys
    if (installed) return;
    installed = true;

    scanFilesystem();

    var manifestDir:String = FileUtil.gameDirectory + '/manifest';
    if (!FileSystem.exists(manifestDir) || !FileSystem.isDirectory(manifestDir)) return;

    for (fileName in FileSystem.readDirectory(manifestDir))
    {
      if (fileName.length < 6 || !fileName.endsWith('.json')) continue;
      var libName:String = fileName.substr(0, fileName.length - 5);
      var inner:Null<AssetLibrary> = Assets.getLibrary(libName);
      if (inner == null) continue; // not preloaded; leave its lazy path untouched
      Assets.registerLibrary(libName, new RuntimeAssetLibrary(inner));
    }
    #end
  }

  /**
   * Re-scan `assets/` so files written after startup (e.g. by the song
   * generator) become resolvable without a restart.
   */
  public static function refresh():Void
  {
    #if sys
    scanned = false;
    scanFilesystem();
    #end
  }

  #if sys
  static function scanFilesystem():Void
  {
    if (scanned) return;
    scanned = true;

    var root:String = FileUtil.gameDirectory.split('\\').join('/');
    while (root.endsWith('/')) root = root.substr(0, root.length - 1);
    scanDir(root + '/assets', 'assets');
  }

  static function scanDir(dir:String, prefix:String):Void
  {
    if (!FileSystem.exists(dir) || !FileSystem.isDirectory(dir)) return;

    for (name in FileSystem.readDirectory(dir))
    {
      var path:String = dir + '/' + name;
      if (FileSystem.isDirectory(path))
      {
        scanDir(path, prefix + '/' + name);
        continue;
      }

      var id:String = prefix + '/' + name;
      fsPaths.set(id, path);
      fsTypes.set(id, typeForPath(id));
    }
  }

  static function typeForPath(id:String):AssetType
  {
    var ext:String = '';
    var dot:Int = id.lastIndexOf('.');
    if (dot >= 0) ext = id.substr(dot + 1).toLowerCase();

    switch (ext)
    {
      case 'json', 'txt', 'xml', 'srt', 'frag', 'vert', 'lua', 'cfg', 'ini',
           'conf', 'csv', 'tsv', 'md', 'markdown', 'css', 'html', 'htm', 'js',
           'ts', 'h', 'cpp', 'c', 'hpp', 'hxx', 'cs', 'java', 'py', 'glsl',
           'geom', 'fnt', 'yaml', 'yml', 'toml', 'properties', 'gitignore':
        return AssetType.TEXT;

      case 'png', 'jpg', 'jpeg', 'gif', 'bmp', 'svg', 'tga', 'astc', 'ktx',
           'ktx2', 'pvr', 'basis', 'dds':
        return AssetType.IMAGE;

      case 'ogg', 'wav', 'mp3', 'flac', 'opus', 'm4a', 'aac', 'aiff', 'aif',
           'mid', 'midi':
        return id.indexOf('assets/music/') == 0 ? AssetType.MUSIC : AssetType.SOUND;

      default:
        return AssetType.BINARY;
    }
  }

  function fsTypeMatches(id:String, type:String):Bool
  {
    var assetType = fsTypes.get(id);
    var requestedType:Null<AssetType> = type != null ? cast(type, AssetType) : null;

    if (assetType == requestedType) return true;

    if ((requestedType == AssetType.SOUND || requestedType == AssetType.MUSIC)
      && (assetType == AssetType.MUSIC || assetType == AssetType.SOUND)) return true;

    if (requestedType == AssetType.BINARY || requestedType == null
      || (assetType == AssetType.BINARY && requestedType == AssetType.TEXT)) return true;

    return false;
  }
  #end

  override public function exists(id:String, type:String):Bool
  {
    #if sys
    if (fsTypes.exists(id) && fsTypeMatches(id, type)) return true;
    #end
    return inner != null && inner.exists(id, type);
  }

  override public function getAsset(id:String, type:String):Dynamic
  {
    #if sys
    if (fsPaths.exists(id))
    {
      return switch (cast(type, AssetType))
      {
        case AssetType.BINARY: getBytes(id);
        case AssetType.FONT: getFont(id);
        case AssetType.IMAGE: getImage(id);
        case AssetType.MUSIC, AssetType.SOUND: getAudioBuffer(id);
        case AssetType.TEXT: getText(id);
        case AssetType.TEMPLATE: throw 'Not sure how to get template: $id';
        default: throw 'Unknown asset type: $type';
      };
    }
    #end
    return inner != null ? inner.getAsset(id, type) : null;
  }

  override public function getAudioBuffer(id:String):AudioBuffer
  {
    #if sys
    if (fsPaths.exists(id)) return AudioBuffer.fromFile(fsPaths.get(id));
    #end
    return inner != null ? inner.getAudioBuffer(id) : null;
  }

  override public function getBytes(id:String):Bytes
  {
    #if sys
    if (fsPaths.exists(id)) return Bytes.fromFile(fsPaths.get(id));
    #end
    return inner != null ? inner.getBytes(id) : null;
  }

  override public function getFont(id:String):Font
  {
    #if sys
    if (fsPaths.exists(id)) return Font.fromFile(fsPaths.get(id));
    #end
    return inner != null ? inner.getFont(id) : null;
  }

  override public function getImage(id:String):Image
  {
    #if sys
    if (fsPaths.exists(id)) return Image.fromFile(fsPaths.get(id));
    #end
    return inner != null ? inner.getImage(id) : null;
  }

  override public function getPath(id:String):String
  {
    #if sys
    if (fsPaths.exists(id)) return fsPaths.get(id);
    #end
    return inner != null ? inner.getPath(id) : null;
  }

  override public function getText(id:String):String
  {
    var bytes:Bytes = getBytes(id);
    return bytes == null ? null : bytes.getString(0, bytes.length);
  }

  override public function isLocal(id:String, type:String):Bool
  {
    #if sys
    if (fsPaths.exists(id)) return true;
    #end
    return inner != null ? inner.isLocal(id, type) : false;
  }

  override public function list(type:String):Array<String>
  {
    var result:Array<String> = inner != null ? inner.list(type).copy() : [];

    #if sys
    for (id in fsTypes.keys())
    {
      if (fsTypeMatches(id, type) && (inner == null || !inner.exists(id, type)))
      {
        result.push(id);
      }
    }
    #end

    return result;
  }

  override public function load():Future<AssetLibrary>
  {
    return inner != null ? inner.load() : Future.withValue(cast this);
  }

  override public function unload():Void
  {
    if (inner != null) inner.unload();
  }
}
