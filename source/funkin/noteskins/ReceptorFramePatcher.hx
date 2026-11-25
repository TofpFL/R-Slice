package funkin.noteskins;

import haxe.ds.StringMap;

/**
 * Rewrites a Sparrow atlas XML so receptor-role frames (static / press /
 * confirm) carry trimmed-source metadata (`frameX/frameY/frameWidth/frameHeight`)
 * matching the logical sizes V-Slice's native `noteStrumline` atlas uses.
 *
 * Why: `FlxAtlasFrames.fromSparrow` treats any frame with `frameX` as trimmed —
 * `frameWidth/frameHeight` become the frame's `sourceSize` and `-frameX/-frameY`
 * become its offset inside that logical rect. `StrumlineNote` centers sprites
 * from `sourceSize`, and V-Slice's native receptors use ~232-238px logical
 * frames while Psych atlases ship untrimmed ~157px frames. Without this patch
 * the imported receptors render shifted relative to the note columns.
 *
 * Only frames that the generated JSON will actually use as receptors are
 * patched; falling-note frames are left untouched (native note frames are
 * untrimmed too).
 */
@:nullSafety
class ReceptorFramePatcher
{
  /** Role ids. */
  static final ROLE_STATIC:Int = 0;
  static final ROLE_PRESS:Int = 1;
  static final ROLE_CONFIRM:Int = 2;

  /**
   * Logical [width, height] per role per direction (0=left,1=down,2=up,3=right),
   * mirroring the frameWidth/frameHeight values in the built-in
   * `noteStrumline.xml` so imported receptors get identical sprite metrics.
   */
  static final LOGICAL_SIZE:Map<Int, Array<Array<Int>>> = [
    ROLE_STATIC => [[232, 236], [235, 230], [238, 235], [233, 236]],
    ROLE_PRESS => [[146, 148], [149, 146], [153, 150], [148, 151]],
    ROLE_CONFIRM => [[232, 236], [235, 230], [238, 235], [233, 236]]
  ];

  /**
   * Patch `xmlText`, returning the rewritten XML. `prefixes` is the structure
   * returned by `NoteSkinConverter.resolvePrefixes()` ({note, strum, press,
   * confirm} — each an Array<Null<String>> indexed by direction).
   */
  public static function patch(xmlText:String, atlas:SkinAtlasParser, prefixes:Dynamic):String
  {
    if (xmlText == null || atlas == null || atlas.frames.length == 0) return xmlText;

    var roles = classifyFrames(atlas, prefixes);
    var hasRoles:Bool = false;
    for (_ in roles.keys()) { hasRoles = true; break; }
    if (!hasRoles) return xmlText;

    var tagRe = ~/<SubTexture\b[^>]*>/g;
    var out = new StringBuf();
    var pos:Int = 0;
    while (tagRe.matchSub(xmlText, pos))
    {
      var span = tagRe.matchedPos();
      out.addSub(xmlText, pos, span.pos - pos);
      var tag:String = tagRe.matched(0);
      var name:Null<String> = SkinAtlasParser.extractAttr(tag, 'name');
      var info:Null<{role:Int, dir:Int, frame:SkinAtlasParser.AtlasFrame}> = (name != null) ? roles.get(name.toLowerCase()) : null;
      if (info != null)
      {
        out.add(patchTag(tag, info.frame, info.role, info.dir));
      }
      else
      {
        out.add(tag);
      }
      pos = span.pos + span.len;
    }
    out.addSub(xmlText, pos, xmlText.length - pos);
    return out.toString();
  }

  /**
   * Map lowercased frame name -> patch info for every frame that is used as a
   * receptor by the generated style. Frames also used as falling notes are
   * excluded so note metrics stay untrimmed like the native skin.
   */
  static function classifyFrames(atlas:SkinAtlasParser, prefixes:Dynamic):Map<String, {role:Int, dir:Int, frame:SkinAtlasParser.AtlasFrame}>
  {
    var result = new Map<String, {role:Int, dir:Int, frame:SkinAtlasParser.AtlasFrame}>();

    // Frames referenced by note prefixes must never be treated as receptors.
    var notePrefixes = new Array<Null<String>>();
    if (prefixes != null && prefixes.note != null) notePrefixes = prefixes.note;

    var roleLists = [
      {role: ROLE_STATIC, list: prefixes != null ? prefixes.strum : null},
      {role: ROLE_PRESS, list: prefixes != null ? prefixes.press : null},
      {role: ROLE_CONFIRM, list: prefixes != null ? prefixes.confirm : null}
    ];

    for (frame in atlas.frames)
    {
      var lname = frame.name.toLowerCase();

      var isNote = false;
      for (p in notePrefixes)
      {
        if (p != null && lname.startsWith(p.toLowerCase())) { isNote = true; break; }
      }
      if (isNote) continue;

      for (entry in roleLists)
      {
        var list:Null<Array<Null<String>>> = entry.list;
        if (list == null) continue;
        for (dir in 0...4)
        {
          var p:Null<String> = (dir < list.length) ? list[dir] : null;
          if (p == null || p.length == 0) continue;
          if (lname.startsWith(p.toLowerCase()))
          {
            result.set(lname, {role: entry.role, dir: dir, frame: frame});
            break;
          }
        }
        if (result.exists(lname)) break;
      }
    }
    return result;
  }

  /**
   * Rewrite a single `<SubTexture ...>` tag, replacing/adding the trimmed
   * metadata attributes for the frame's receptor role.
   */
  static function patchTag(tag:String, frame:SkinAtlasParser.AtlasFrame, role:Int, dir:Int):String
  {
    var roleSizes:Null<Array<Array<Int>>> = LOGICAL_SIZE.get(role);
    if (roleSizes == null || dir >= roleSizes.length) return tag;
    var size:Array<Int> = roleSizes[dir];
    var logicalW:Int = size[0];
    var logicalH:Int = size[1];

    // Center the art inside the logical rect: offset = (logical - art) / 2,
    // and fromSparrow stores offset as -frameX/-frameY.
    var frameX:Int = Std.int((frame.width - logicalW) / 2);
    var frameY:Int = Std.int((frame.height - logicalH) / 2);

    // Strip a self-closing or open tag suffix.
    var suffix = ">";
    var inner = tag;
    if (tag.length >= 2 && tag.charAt(tag.length - 2) == '/')
    {
      suffix = "/>";
      inner = tag.substr(0, tag.length - 2);
    }
    else if (tag.charAt(tag.length - 1) == '>')
    {
      inner = tag.substr(0, tag.length - 1);
    }

    // Remove any pre-existing trimmed metadata attributes.
    inner = new EReg('\\s+(frameX|frameY|frameWidth|frameHeight)\\s*=\\s*("[^"]*"|\'[^\']*\')', 'g').replace(inner, '');

    return inner
      + ' frameX="' + frameX + '"'
      + ' frameY="' + frameY + '"'
      + ' frameWidth="' + logicalW + '"'
      + ' frameHeight="' + logicalH + '"'
      + suffix;
  }
}
