package funkin.noteskins;

import haxe.Json;

/**
 * Converts a Psych-Engine/JS-Engine-style noteskin atlas into a V-Slice
 * `NoteStyleData` JSON document.
 *
 * Psych skins are convention-driven: a single Sparrow atlas whose frames are
 * named `arrowUP/arrowDOWN/arrowLEFT/arrowRIGHT`, `{dir} press`, `{dir} confirm`,
 * `{color} hold piece`, `{color} hold end`, `note splash`, etc. V-Slice instead
 * wants an explicit per-slot prefix map. This class bridges the two by probing
 * the atlas for the conventional names and emitting the equivalent JSON.
 *
 * Non-skin-specific slots (countdowns, judgements, combo numbers, hold covers)
 * are inherited from the base game's `funkin` style so imported skins always
 * render a complete, valid `NoteStyleData`.
 */
@:nullSafety
class NoteSkinConverter
{
  /**
   * Direction -> Psych color name used in hold/splash frame prefixes.
   * Matches JS Engine: purple=LEFT, blue=DOWN, green=UP, red=RIGHT.
   */
  static final COLOR_FOR_DIR:Map<Int, String> = [
    0 => 'purple', // LEFT
    1 => 'blue',   // DOWN
    2 => 'green',  // UP
    3 => 'red'     // RIGHT
  ];

  /** Direction index -> V-Slice note data key. */
  static final DIR_KEYS:Array<String> = ['left', 'down', 'up', 'right'];

  /** Direction index -> V-Slice strumline suffix base. */
  static final DIR_NAME:Array<String> = ['left', 'down', 'up', 'right'];

  var atlas:SkinAtlasParser;
  var baseJson:Dynamic; // parsed funkin.json (or supplied base)

  public function new(atlas:SkinAtlasParser, ?baseJson:Dynamic)
  {
    this.atlas = atlas;
    this.baseJson = baseJson;
  }

  /**
   * Candidate frame prefixes for a slot, in priority order. The first prefix
   * that matches a frame in the atlas wins. `null` entries are skipped.
   */
  function pick(candidates:Array<String>):Null<String>
  {
    for (c in candidates)
    {
      if (c == null || c.trim().length == 0) continue;
      if (atlas.hasPrefix(c)) return c;
    }
    return null;
  }

  /** Build the per-direction note-arrow prefix (scrolling notes). */
  function notePrefix(dir:Int):Null<String>
  {
    var color:String = COLOR_FOR_DIR.get(dir) ?? '  ';// sentinel: never matches a real frame
    return pick([
      color + '0',                            // purple0 / blue0 / ... JS colored note frames
      'note${DIR_NAME[dir].toUpperCase()}',    // noteLeft (funkIN-style)
      'arrow${DIR_NAME[dir].toUpperCase()}',   // arrowLEFT fallback for arrow-only atlases
      '${DIR_NAME[dir]}0'                     // left0 scroll variant
    ]);
  }

  /** Static receptor prefix for a direction. */
  function staticPrefix(dir:Int):Null<String>
  {
    var d:String = DIR_NAME[dir];
    var color:String = COLOR_FOR_DIR.get(dir) ?? '  ';// sentinel: never matches a real frame
    return pick([
      'arrow${d.toUpperCase()}',              // JS reuses the raw arrows for static receptors
      '${d} static',
      'static${d.toUpperCase()}',
      color + '0'                             // last resort: colored note frames
    ]);
  }

  /** Pressed receptor prefix for a direction. */
  function pressPrefix(dir:Int):Null<String>
  {
    var d:String = DIR_NAME[dir];
    var color:String = COLOR_FOR_DIR.get(dir) ?? '  ';// sentinel: never matches a real frame
    return pick([
      '${d} press',                           // "left press"
      '${d}press',
      'press${d.toUpperCase()}',
      '${d} pressed',
      color + ' press'
    ]);
  }

  /** Confirmed receptor prefix for a direction. */
  function confirmPrefix(dir:Int):Null<String>
  {
    var d:String = DIR_NAME[dir];
    var color:String = COLOR_FOR_DIR.get(dir) ?? '  ';// sentinel: never matches a real frame
    return pick([
      '${d} confirm',                         // "left confirm"
      '${d}confirm',
      'confirm${d.toUpperCase()}',
      '${d} confirmed',
      color + ' confirm'
    ]);
  }

  /**
   * Resolve every slot prefix once and return them grouped by role, indexed by
   * direction (0=left, 1=down, 2=up, 3=right). Used by the atlas post-processors
   * (palette baker, receptor frame patcher) so they classify frames exactly the
   * way the generated JSON will.
   */
  public function resolvePrefixes():Dynamic
  {
    var note:Array<Null<String>> = [];
    var stat:Array<Null<String>> = [];
    var prs:Array<Null<String>> = [];
    var cfm:Array<Null<String>> = [];
    for (dir in 0...4)
    {
      note[dir] = notePrefix(dir);
      stat[dir] = staticPrefix(dir);
      prs[dir] = pressPrefix(dir);
      cfm[dir] = confirmPrefix(dir);
    }
    return {note: note, strum: stat, press: prs, confirm: cfm};
  }

  /**
   * Build the full `assets` object for the imported skin.
   * Starts from the base (funkIN) assets and overrides the skin-specific slots.
   */
  public function buildAssets(skinAssetPath:String, ?scale:Float = 0.7):Dynamic
  {
    // Deep-copy the base assets so we never mutate the shared funkin.json parse.
    var assets:Dynamic = if (baseJson != null && baseJson.assets != null)
      Json.parse(Json.stringify(baseJson.assets))
    else
      {};

    if (assets == null) assets = {};

    // --- NOTE (scrolling arrows) ---
    var noteData:Dynamic = {};
    var foundNotes:Int = 0;
    for (dir in 0...4)
    {
      var p:Null<String> = notePrefix(dir);
      if (p != null)
      {
        Reflect.setField(noteData, DIR_KEYS[dir], {prefix: p});
        foundNotes++;
      }
    }
    // NoteStyleData requires all four directions; fall back to the base style
    // for partial atlases rather than emitting an invalid document.
    if (foundNotes == 4)
    {
      assets.note = {
        assetPath: skinAssetPath,
        scale: scale,
        data: noteData
      };
    }

    // --- STRUMLINE (receptors) ---
    var strumData:Dynamic = {};
    var foundStrum:Int = 0;
    for (dir in 0...4)
    {
      var d:String = DIR_NAME[dir];
      var stat:Null<String> = staticPrefix(dir);
      var prs:Null<String> = pressPrefix(dir);
      var cfm:Null<String> = confirmPrefix(dir);
      // Fall back to the arrow for any missing receptor state.
      var arrowP:Null<String> = notePrefix(dir);
      var useStat:Null<String> = stat ?? arrowP;
      var usePrs:Null<String> = prs ?? useStat;
      var useCfm:Null<String> = cfm ?? useStat;
      if (useStat != null)
      {
        Reflect.setField(strumData, d + 'Static', {prefix: useStat});
        Reflect.setField(strumData, d + 'Press', {prefix: usePrs ?? useStat});
        Reflect.setField(strumData, d + 'Confirm', {prefix: useCfm ?? useStat});
        Reflect.setField(strumData, d + 'ConfirmHold', {prefix: useCfm ?? useStat});
        foundStrum++;
      }
    }
    // All 16 strumline fields are required by the schema; use the base style
    // unless every direction resolved to a receptor prefix.
    if (foundStrum == 4)
    {
      assets.noteStrumline = {
        assetPath: skinAssetPath,
        scale: scale,
        offsets: [0, 0],
        data: strumData
      };
    }

    // --- NOTE SPLASH ---
    var splashPrefix:Null<String> = pick(['note splash', 'notesplash', 'splash']);
    if (splashPrefix != null && assets.noteSplash != null)
    {
      // Keep the base splash assetPath but point its frame prefixes at this skin
      // only if the skin actually ships splash frames on the same atlas.
      // Psych skins usually have a single `note splash`; reuse it for all dirs.
      var splashData:Dynamic = {enabled: true};
      for (key in ['leftSplashes', 'rightSplashes', 'upSplashes', 'downSplashes'])
      {
        Reflect.setField(splashData, key, [{prefix: splashPrefix}]);
      }
      assets.noteSplash = {
        assetPath: skinAssetPath,
        alpha: assets.noteSplash.alpha ?? 0.8,
        offsets: assets.noteSplash.offsets ?? [25, -5],
        data: splashData
      };
    }

    // --- HOLD NOTE ---
    // V-Slice expects a single 8-segment striped image, which a Psych atlas does
    // not provide. We leave the base (funkIN) holdNote in place for now; the
    // importer may later synthesize a strip (see HoldNoteStripBuilder).
    // If the atlas DOES ship a conventional hold strip we could detect it here.

    return assets;
  }

  /**
   * Build the complete `NoteStyleData` JSON document for the skin.
   * @param id The skin id (becomes the filename + registry key).
   * @param displayName Human-readable name shown in the selector.
   * @param skinAssetPath The asset path (e.g. `shared:MySkin`) for the imported atlas.
   * @param author Optional author string.
   * @param scale Render scale for note/receptor sprites.
   */
  public function build(id:String, displayName:String, skinAssetPath:String, ?author:String = 'Imported', ?scale:Float = 0.7):Dynamic
  {
    var doc:Dynamic = {
      version: '1.1.0',
      name: displayName,
      author: author,
      fallback: 'funkin',
      assets: buildAssets(skinAssetPath, scale)
    };
    return doc;
  }

  /** Serialize a built document to pretty JSON text. */
  public static function toJsonString(doc:Dynamic):String
  {
    return Json.stringify(doc, null, '  ');
  }
}
