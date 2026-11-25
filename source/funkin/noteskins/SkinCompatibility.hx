package funkin.noteskins;

/**
 * Classifies a discovered noteskin package and collects the issues a
 * conversion produced — the data behind the conversion report and the
 * future "make adjustments?" compatibility prompt.
 *
 * Kind values:
 *   `psych`   — Psych/JS-Engine conventions detected (arrow{DIR} receptors,
 *               {color}0 note frames, {dir} press/confirm, {color} holds)
 *   `generic` — conventional FNF frame names found but not the full Psych set
 *   `unknown` — atlas parsed but no recognizable noteskin conventions
 */
typedef SkinIssue =
{
  /** `error` | `warn` | `info` */
  var level:String;
  /** Pipeline area: `atlas`, `frames`, `palette`, `receptors`, `notes`, `holds`, `splash`, `metadata` */
  var area:String;
  var message:String;
}

typedef SkinAnalysis =
{
  var kind:String;
  var issues:Array<SkinIssue>;
  var frameCount:Int;
  /** True when the atlas uses the RGB channel-mask convention. */
  var paletteMask:Bool;
  /** True when the source atlas carried trim/offset metadata. */
  var hadTrimData:Bool;
}

@:nullSafety
class SkinCompatibility
{
  /** Direction index -> Psych color name. Mirrors NoteSkinConverter. */
  static final COLOR_FOR_DIR:Map<Int, String> = [
    0 => 'purple', 1 => 'blue', 2 => 'green', 3 => 'red'
  ];

  static final DIR_NAME:Array<String> = ['left', 'down', 'up', 'right'];

  /**
   * Analyze an atlas + resolved prefixes. `baked` reports whether the palette
   * baker rewrote the PNG (mask atlas detected and applied).
   */
  public static function analyze(atlas:SkinAtlasParser, prefixes:Dynamic, baked:Bool):SkinAnalysis
  {
    var issues:Array<SkinIssue> = [];
    var frameCount:Int = atlas.frames.length;

    if (frameCount == 0)
    {
      issues.push({level: 'error', area: 'atlas', message: 'Atlas contains no frames'});
      return {kind: 'unknown', issues: issues, frameCount: 0, paletteMask: false, hadTrimData: false};
    }

    // --- Frame-role coverage -------------------------------------------------
    var missingNotes:Array<String> = [];
    var missingStatic:Array<String> = [];
    var missingPress:Array<String> = [];
    var missingConfirm:Array<String> = [];

    var noteP:Array<Null<String>> = prefixes != null && prefixes.note != null ? prefixes.note : [];
    var statP:Array<Null<String>> = prefixes != null && prefixes.strum != null ? prefixes.strum : [];
    var prsP:Array<Null<String>> = prefixes != null && prefixes.press != null ? prefixes.press : [];
    var cfmP:Array<Null<String>> = prefixes != null && prefixes.confirm != null ? prefixes.confirm : [];

    for (dir in 0...4)
    {
      var d = DIR_NAME[dir];
      if (dir >= noteP.length || noteP[dir] == null) missingNotes.push(d);
      if (dir >= statP.length || statP[dir] == null) missingStatic.push(d);
      if (dir >= prsP.length || prsP[dir] == null) missingPress.push(d);
      if (dir >= cfmP.length || cfmP[dir] == null) missingConfirm.push(d);
    }

    if (missingNotes.length == 4)
      issues.push({level: 'error', area: 'notes', message: 'No falling-note frames found for any direction — skin cannot render notes'});
    else if (missingNotes.length > 0)
      issues.push({level: 'warn', area: 'notes', message: 'Missing falling-note frames for: ${missingNotes.join(", ")} (base style used)'});

    if (missingStatic.length > 0)
      issues.push({level: 'warn', area: 'receptors', message: 'Missing static receptor frames for: ${missingStatic.join(", ")} (fell back to note frames)'});
    if (missingPress.length > 0)
      issues.push({level: 'info', area: 'receptors', message: 'No press animation for: ${missingPress.join(", ")} (static frame reused)'});
    if (missingConfirm.length > 0)
      issues.push({level: 'info', area: 'receptors', message: 'No confirm animation for: ${missingConfirm.join(", ")} (static frame reused)'});

    // --- Conventional Psych/JS features -------------------------------------
    var hasArrowReceptors = atlas.hasPrefix('arrowLEFT') && atlas.hasPrefix('arrowDOWN')
      && atlas.hasPrefix('arrowUP') && atlas.hasPrefix('arrowRIGHT');
    var hasColorNotes = COLOR_FOR_DIR.exists(0) && atlas.hasPrefix('purple0')
      && atlas.hasPrefix('blue0') && atlas.hasPrefix('green0') && atlas.hasPrefix('red0');
    var hasDirPress = atlas.hasPrefix('left press') || atlas.hasPrefix('leftpress');
    var hasColorHolds = atlas.hasPrefix('purple hold') || atlas.hasPrefix('blue hold')
      || atlas.hasPrefix('green hold') || atlas.hasPrefix('red hold');

    var hasSplash = atlas.hasPrefix('note splash') || atlas.hasPrefix('notesplash') || atlas.hasPrefix('splash');
    if (!hasSplash) issues.push({level: 'info', area: 'splash', message: 'No note-splash frames — base style splash used'});
    if (!hasColorHolds) issues.push({level: 'warn', area: 'holds', message: 'No {color} hold piece/end frames — hold notes fall back to the base strip'});

    // --- Palette / trim -------------------------------------------------------
    var kind = 'unknown';
    if (hasArrowReceptors && hasDirPress) kind = 'psych';
    else if (hasArrowReceptors || hasColorNotes || hasDirPress) kind = 'generic';

    if (baked) issues.push({level: 'info', area: 'palette', message: 'RGB channel-mask atlas detected — lane palettes baked into PNG'});

    // Trim metadata detection requires raw XML attributes the parser drops;
    // the receptor patcher handles normalization regardless, so this stays
    // conservatively false until the parser exposes trim fields.
    return {kind: kind, issues: issues, frameCount: frameCount, paletteMask: baked, hadTrimData: false};
  }

  /** One-line-per-issue rendering for logs. */
  public static function formatIssues(issues:Array<SkinIssue>):Array<String>
  {
    var out:Array<String> = [];
    for (i in issues) out.push('    [${i.level}] ${i.area}: ${i.message}');
    return out;
  }
}
