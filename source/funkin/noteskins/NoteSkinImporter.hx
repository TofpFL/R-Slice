package funkin.noteskins;

import sys.FileSystem;
import sys.io.File;
import haxe.io.Path;
import haxe.Json;
import funkin.Paths;
import funkin.data.notestyle.NoteStyleRegistry;
import lime.utils.AssetManifest;
import openfl.utils.AssetLibrary;
import openfl.utils.AssetType;
import openfl.utils.Assets;

/**
 * A discovered noteskin package on disk: a sprite atlas (PNG + Sparrow XML)
 * plus an optional `skin.json` for overrides.
 */
typedef SkinPackage =
{
  /** Stable id used as the registry key / JSON filename. */
  var id:String;
  /** Human-readable display name. */
  var name:String;
  /** Directory the package was found in. */
  var dir:String;
  /** Absolute path to the atlas PNG. */
  var pngPath:String;
  /** Absolute path to a Sparrow .xml atlas, when present. */
  var xmlPath:Null<String>;
  /** Absolute path to a TexturePacker .json atlas, when present (no .xml). */
  var atlasJsonPath:Null<String>;
  /** Optional absolute path to a skin.json override file. */
  var jsonPath:Null<String>;
}

/** Normalized atlas metadata: Sparrow XML text + provenance tag. */
typedef AtlasXmlResult =
{
  var text:String;
  /** 'sparrow-xml' | 'texturepacker-json' */
  var source:String;
}

/**
 * Discovers and imports custom noteskins dropped into the engine's `noteskins/`
 * directory at runtime — no rebuild required.
 *
 * Layout supported (Psych/JS-Engine compatible):
 *   <exe>/noteskins/
 *     <Anything>.png + <Anything>.xml        // flat Sparrow atlas pair = one skin
 *     <Anything>.png + <Anything>.json       // flat TexturePacker JSON atlas = one skin
 *     <SkinFolder>/
 *        *.png + *.xml or *.json               // each pair = one skin
 *        skin.json                            // optional overrides (name/scale/author/bakePalette)
 *        list.txt                             // optional, ignored for now
 *
 * For every discovered atlas the importer:
 *   1. Normalizes atlas metadata to Sparrow XML (TexturePacker JSON is
 *      converted; trim data is preserved as frameX/frameY/frameWidth/Height).
 *   2. Parses frames, probes conventional Psych noteskin prefixes, and builds
 *      a V-Slice `NoteStyleData` JSON (non-skin slots inherit funkin).
 *   3. Post-processes: bakes RGB channel-mask palettes into the PNG when
 *      detected, and normalizes receptor frames to native logical sizes.
 *   4. Copies the atlas into `assets/noteskins/images/<id>.{png,xml}` and
 *      writes `assets/noteskins/data/notestyles/<id>.json`.
 *   5. Writes a conversion report to `assets/noteskins/reports/<id>.txt`.
 *   6. Registers the files in a runtime `noteskins` asset library and the
 *      entry into `NoteStyleRegistry`, so it appears in the noteskin selector.
 *
 * Source files are never modified — all output goes to `assets/noteskins/`.
 */
@:nullSafety
class NoteSkinImporter
{
  /** Runtime asset library name used for generated skin data/textures. */
  static final LIBRARY_NAME:String = 'noteskins';

  /** Directory (relative to the exe dir) scanned for skin packages. */
  public static final SKINS_DIR:String = 'noteskins';

  /** Sub-directory under assets where imported atlas files are written. */
  public static final ATLAS_OUT_DIR:String = 'assets/noteskins/images';

  /** Sub-directory under assets where generated notestyle JSON is written. */
  public static final DATA_OUT_DIR:String = 'assets/noteskins/data/notestyles';

  /** Asset entries registered into the runtime `noteskins` library. */
  static var skinAssetEntries(default, null):Array<Dynamic> = [];

  /** Parsed base funkin.json used as the template for non-skin slots. */
  static var baseTemplate:Dynamic = null;

  /** Ids of skins imported this session (for logging / editor). */
  public static var importedIds(default, null):Array<String> = [];

  /** Conversion reports keyed by skin id, for the editor / compatibility UI. */
  public static var reports(default, null):Map<String, NoteSkinReport> = new Map();

  /**
   * Directory the game executable lives in. Unlike `Sys.getCwd()` this is
   * stable regardless of where the process was launched from, so skins are
   * always found next to `R-Slice.exe`.
   */
  static function gameDir():String
  {
    #if sys
    var dir:String = Path.directory(Sys.programPath());
    if (dir != null && dir.length > 0) return Path.addTrailingSlash(dir);
    #end
    return Path.addTrailingSlash(Sys.getCwd());
  }

  /**
   * Scan `noteskins/` and import every valid skin found.
   * Safe to call once at boot; returns the ids that were imported.
   */
  public static function importAll(?log:Null<String->Void>):Array<String>
  {
    importedIds = [];
    var skinsRoot:String = Path.join([gameDir(), SKINS_DIR]);

    if (!FileSystem.exists(skinsRoot))
    {
      logMessage(log, 'No noteskins directory found at ${skinsRoot} — nothing to import.');
      return importedIds;
    }

    var packages:Array<SkinPackage> = discover(skinsRoot);
    logMessage(log, 'Found ${packages.length} candidate noteskin atlas(es) in ${skinsRoot}');

    for (pkg in packages)
    {
      var id:Null<String> = importPackage(pkg, log);
      if (id != null) importedIds.push(id);
    }

    return importedIds;
  }

  /**
   * Recursively find atlas PNG+XML pairs under `dir`.
   */
  public static function discover(dir:String):Array<SkinPackage>
  {
    var result:Array<SkinPackage> = [];
    if (!FileSystem.exists(dir)) return result;

    var entries:Array<String> = FileSystem.readDirectory(dir);
    var pngs = new Map<String, String>(); // basename(no ext) -> full path

    for (name in entries)
    {
      var full:String = Path.join([dir, name]);
      if (FileSystem.isDirectory(full))
      {
        // Recurse into subfolders — but also treat the folder itself as a package root.
        for (sub in discover(full)) result.push(sub);
        continue;
      }
      var lname:String = name.toLowerCase();
      if (lname.endsWith('.png'))
      {
        pngs.set(stripExt(name).toLowerCase(), full);
      }
    }

    // Match each PNG with a sibling atlas: Sparrow XML first, then a
    // TexturePacker JSON atlas (hash or array format).
    for (key => pngPath in pngs)
    {
      var baseName:String = stripExt(Path.withoutDirectory(pngPath));
      var xmlPath:Null<String> = Path.join([dir, baseName + '.xml']);
      // XML might differ only by case; fall back to a case-insensitive sibling search.
      if (xmlPath == null || !FileSystem.exists(xmlPath))
      {
        xmlPath = findSiblingXml(dir, pngPath);
      }

      var atlasJsonPath:Null<String> = null;
      var overrideJsonPath:Null<String> = findOverrideJson(dir, pngPath);
      if (xmlPath == null)
      {
        // No XML — look for a TexturePacker JSON atlas with the same basename.
        var candidate = findAtlasJson(dir, pngPath);
        if (candidate != null)
        {
          var content:String = File.getContent(candidate);
          if (content != null && content.indexOf('"frames"') >= 0)
          {
            atlasJsonPath = candidate;
            // A JSON atlas can't double as the skin.json overrides file.
            if (overrideJsonPath == candidate) overrideJsonPath = null;
          }
        }
      }
      if (xmlPath == null && atlasJsonPath == null) continue; // no atlas metadata -> can't map frames

      var pkg:SkinPackage = {
        id: sanitizeId(baseName),
        name: prettyName(baseName),
        dir: dir,
        pngPath: pngPath,
        xmlPath: xmlPath,
        atlasJsonPath: atlasJsonPath,
        jsonPath: overrideJsonPath
      };
      result.push(pkg);
    }

    return result;
  }

  /**
   * Import a single discovered package. Returns the registry id on success.
   */
  public static function importPackage(pkg:SkinPackage, ?log:Null<String->Void>):Null<String>
  {
    var id:String = pkg.id;
    try
    {
      // 1. Load atlas metadata as Sparrow XML — TexturePacker JSON atlases are
      //    normalized to the same representation so every downstream stage
      //    (converter, baker, patcher, runtime loader) sees one format.
      var atlasXml = loadAtlasXml(pkg);
      if (atlasXml == null)
      {
        logMessage(log, '  [${id}] no readable atlas metadata (need .xml or TexturePacker .json) — skipped');
        return null;
      }
      var xmlText:String = atlasXml.text;
      var atlasSource:String = atlasXml.source;

      var atlas = new SkinAtlasParser(xmlText);
      atlas.parse();
      if (atlas.frames.length == 0)
      {
        logMessage(log, '  [${id}] atlas has no frames — skipped');
        return null;
      }

      // 2. Load skin.json overrides if present.
      var overrides:Dynamic = null;
      if (pkg.jsonPath != null && FileSystem.exists(pkg.jsonPath))
      {
        overrides = try Json.parse(File.getContent(pkg.jsonPath)) catch (e:Dynamic) null;
      }

      var displayName:String = (overrides != null && overrides.name != null) ? Std.string(overrides.name) : pkg.name;
      var scale:Float = (overrides != null && overrides.scale != null) ? Std.parseFloat(Std.string(overrides.scale)) : 0.7;
      var author:String = (overrides != null && overrides.author != null) ? Std.string(overrides.author) : 'Imported';

      var report = new NoteSkinReport(id, displayName, pkg.dir, atlasSource);
      reports.set(id, report);

      // 3. Build the NoteStyleData document.
      var assetPath:String = '${LIBRARY_NAME}:${id}';
      var base:Dynamic = loadBaseTemplate(log);
      var converter = new NoteSkinConverter(atlas, base);
      var doc:Dynamic = converter.build(id, displayName, assetPath, author, scale);
      var prefixes:Dynamic = converter.resolvePrefixes();

      // 3b. Post-process the atlas for V-Slice's renderer:
      //   - palette-mask atlases (Psych/JS convention) get lane colors baked in
      //   - receptor frames get trimmed-source metadata matching the native
      //     strumline atlas so receptors align with the note columns
      var pngBytes:haxe.io.Bytes = File.getBytes(pkg.pngPath);
      var bakeOverride:Null<Bool> = null;
      if (overrides != null && overrides.bakePalette != null)
      {
        bakeOverride = overrides.bakePalette == true || overrides.bakePalette == 'true';
      }
      var baked:Null<haxe.io.Bytes> = NoteSkinBaker.bake(pngBytes, atlas, prefixes, bakeOverride);
      if (baked != null)
      {
        pngBytes = baked;
        logMessage(log, '  [${id}] palette-mask atlas detected — lane colors baked into PNG');
      }
      var patchedXml:String = ReceptorFramePatcher.patch(xmlText, atlas, prefixes);

      // 3c. Compatibility analysis + conversion report.
      var analysis:SkinCompatibility.SkinAnalysis = SkinCompatibility.analyze(atlas, prefixes, baked != null);
      report.kind = analysis.kind;
      report.frameCount = analysis.frameCount;
      report.paletteMask = analysis.paletteMask;
      report.issues = analysis.issues;
      if (atlasSource != 'sparrow-xml') report.note('Converted ${atlasSource} atlas metadata to Sparrow XML');
      if (baked != null) report.note('Detected RGB channel-mask atlas — baked JS lane palettes into PNG');
      report.note('Normalized receptor frames to native V-Slice logical sizes');
      report.note('Wrote generated NoteStyleData JSON + copied atlas into assets/noteskins/');
      var reportPath:Null<String> = report.write(gameDir());

      // 4. Copy atlas PNG + XML into the runtime asset library's directory.
      var imageDir:String = Path.join([gameDir(), ATLAS_OUT_DIR]);
      ensureDirectory(imageDir);
      var pngPath:String = Path.join([imageDir, id + '.png']);
      var xmlPath:String = Path.join([imageDir, id + '.xml']);
      File.saveBytes(pngPath, pngBytes);
      File.saveContent(xmlPath, patchedXml);

      // 5. Write the generated notestyle JSON.
      var jsonDir:String = Path.join([gameDir(), DATA_OUT_DIR]);
      ensureDirectory(jsonDir);
      var jsonPath:String = Path.join([jsonDir, id + '.json']);
      File.saveContent(jsonPath, NoteSkinConverter.toJsonString(doc));

      // 6. Register the copied files as assets, then the entry into the registry.
      addRuntimeAsset('assets/noteskins/images/${id}.png', pngPath, 'IMAGE');
      addRuntimeAsset('assets/noteskins/images/${id}.xml', xmlPath, 'TEXT');
      addRuntimeAsset('assets/noteskins/data/notestyles/${id}.json', jsonPath, 'TEXT');
      refreshRuntimeAssetLibrary();

      logMessage(log, '  [${id}] asset check png=${Assets.exists('${LIBRARY_NAME}:assets/noteskins/images/${id}.png', AssetType.IMAGE)}'
        + ' xml=${Assets.exists('${LIBRARY_NAME}:assets/noteskins/images/${id}.xml', AssetType.TEXT)}'
        + ' json=${Assets.exists('${LIBRARY_NAME}:assets/noteskins/data/notestyles/${id}.json', AssetType.TEXT)}');

      var registry = NoteStyleRegistry.instance;
      var previousLevel:Null<String> = Paths.getCurrentLevel();
      Paths.setCurrentLevel(LIBRARY_NAME);
      var entry:Null<funkin.play.notes.notestyle.NoteStyle> = registry.registerEntry(id);
      Paths.setCurrentLevel(previousLevel);
      if (entry == null)
      {
        logMessage(log, '  [${id}] registered but entry creation returned null');
        return null;
      }

      if (analysis.issues.length > 0)
      {
        for (line in SkinCompatibility.formatIssues(analysis.issues)) logMessage(log, '  [${id}]${line}');
      }
      if (reportPath != null)
        logMessage(log, '  [${id}] conversion report: ${reportPath}');

      logMessage(log, '  [${id}] imported as "${displayName}" (${atlas.frames.length} frames, kind=${analysis.kind})');
      return id;
    }
    catch (e:Dynamic)
    {
      logMessage(log, '  [${id}] import failed: ${e}');
      return null;
    }
  }

  static function logMessage(?log:Null<String->Void>, message:String):Void
  {
    if (log != null) log(message);
  }

  /**
   * Resolve a package's atlas metadata to Sparrow XML text.
   * Sparrow `.xml` passes through unchanged; TexturePacker `.json` is
   * converted (hash and array formats supported).
   */
  static function loadAtlasXml(pkg:SkinPackage):Null<AtlasXmlResult>
  {
    if (pkg.xmlPath != null && FileSystem.exists(pkg.xmlPath))
    {
      return {text: File.getContent(pkg.xmlPath), source: 'sparrow-xml'};
    }
    if (pkg.atlasJsonPath != null && FileSystem.exists(pkg.atlasJsonPath))
    {
      var pngName:String = Path.withoutDirectory(pkg.pngPath);
      var xml:Null<String> = TexturePackerAtlas.toSparrowXml(File.getContent(pkg.atlasJsonPath), pngName);
      if (xml != null) return {text: xml, source: 'texturepacker-json'};
    }
    return null;
  }

  static function ensureDirectory(path:String):Void
  {
    if (FileSystem.exists(path)) return;
    var parent:String = Path.directory(path);
    if (parent != path && parent.length > 0) ensureDirectory(parent);
    FileSystem.createDirectory(path);
  }

  static function addRuntimeAsset(id:String, filePath:String, type:String):Void
  {
    skinAssetEntries.push({
      id: id,
      path: filePath,
      type: type,
      preload: false
    });
  }

  /**
   * Rebuild the runtime `noteskins` asset library with every skin imported so
   * far. Lime asset manifests are static after launch, so generated files are
   * exposed through a small library registered at runtime instead.
   */
  static function refreshRuntimeAssetLibrary():Void
  {
    if (skinAssetEntries.length == 0) return;

    var manifest = new AssetManifest();
    manifest.name = LIBRARY_NAME;
    manifest.assets = skinAssetEntries.copy();

    var library = AssetLibrary.fromManifest(manifest);
    if (library != null)
    {
      Assets.registerLibrary(LIBRARY_NAME, library);
    }
  }

  /**
   * Load + parse the base `funkin.json` once, used as the template for all
   * non-skin-specific slots (countdowns, judgements, combo numbers, covers).
   */
  static function loadBaseTemplate(?log:Null<String->Void>):Dynamic
  {
    if (baseTemplate != null) return baseTemplate;
    var path:String = Path.join([gameDir(), 'assets', 'data', 'notestyles', 'funkin.json']);
    if (FileSystem.exists(path))
    {
      try
      {
        baseTemplate = Json.parse(File.getContent(path));
        return baseTemplate;
      }
      catch (e:Dynamic) {}
    }
    logMessage(log, '  [importer] could not load funkin.json template — non-skin slots may be incomplete');
    return null;
  }

  static function findSiblingXml(dir:String, pngPath:String):Null<String>
  {
    var base:String = stripExt(Path.withoutDirectory(pngPath)).toLowerCase();
    for (name in FileSystem.readDirectory(dir))
    {
      if (name.toLowerCase().endsWith('.xml') && stripExt(name).toLowerCase() == base)
      {
        return Path.join([dir, name]);
      }
    }
    return null;
  }

  /** `<basename>.json` next to the atlas — possible TexturePacker metadata. */
  static function findAtlasJson(dir:String, pngPath:String):Null<String>
  {
    var base:String = stripExt(Path.withoutDirectory(pngPath)).toLowerCase();
    for (name in FileSystem.readDirectory(dir))
    {
      var lname = name.toLowerCase();
      if (lname.endsWith('.json') && stripExt(name).toLowerCase() == base)
        return Path.join([dir, name]);
    }
    return null;
  }

  /** `skin.json` override file, falling back to a same-named `.json`. */
  static function findOverrideJson(dir:String, pngPath:String):Null<String>
  {
    var base:String = stripExt(Path.withoutDirectory(pngPath)).toLowerCase();
    var dirEntries = FileSystem.readDirectory(dir);
    // Explicit skin.json wins.
    for (name in dirEntries)
    {
      if (name.toLowerCase() == 'skin.json') return Path.join([dir, name]);
    }
    for (name in dirEntries)
    {
      var lname = name.toLowerCase();
      if (lname.endsWith('.json') && stripExt(name).toLowerCase() == base)
        return Path.join([dir, name]);
    }
    return null;
  }

  /** Filename without its final extension (case-insensitive on the dot). */
  static function stripExt(name:String):String
  {
    var dot:Int = name.lastIndexOf('.');
    return (dot > 0) ? name.substr(0, dot) : name;
  }

  /** Turn an atlas filename into a safe registry id (lowercase, [a-z0-9_-]). */
  static function sanitizeId(raw:String):String
  {
    var s:String = StringTools.replace(raw.toLowerCase(), 'note_assets', '');
    s = StringTools.replace(s, 'noteassets', '');
    s = new EReg('-+', 'g').replace(s, '_');
    s = new EReg('[^a-z0-9_]', 'g').replace(s, '_');
    s = new EReg('_+', 'g').replace(s, '_');
    s = new EReg('^_+|_+$', 'g').replace(s, '');
    if (s.length == 0) s = 'imported';
    // Avoid clobbering built-in ids.
    if (s == 'funkin' || s == 'pixel' || s == 'default') s = 'imported_$s';
    return s;
  }

  static function prettyName(raw:String):String
  {
    var s:String = StringTools.replace(raw, 'NOTE_assets', '');
    s = StringTools.replace(s, 'note_assets', '');
    s = new EReg('[-_]+', 'g').replace(s, ' ');
    s = StringTools.trim(s);
    if (s.length == 0) return 'Imported Skin';
    return s;
  }
}
