package funkin.noteskins;

/**
 * Minimal Sparrow-format atlas (.xml) reader — extracts sub-texture frame
 * names + rects. Used by the noteskin importer to detect which Psych-convention
 * frames a skin provides (arrowUP, left press, purple hold piece, ...) without
 * needing the full TexturePacker JSON format.
 *
 * Frame name matching is prefix-based because Psych atlases suffix frame names
 * with a 4-digit index (e.g. "arrowUP0000", "left press0000").
 */
class SkinAtlasParser
{
  /** name -> rect info */
  public var frames(default, null):Array<AtlasFrame> = [];

  /** normalized-lowercase name -> AtlasFrame (first wins on dupes) */
  var frameNames:Map<String, AtlasFrame> = new Map();

  var xmlText:String;

  public function new(xmlText:String)
  {
    this.xmlText = xmlText;
  }

  /** Parse the XML text into frame records. */
  public function parse():Array<AtlasFrame>
  {
    frames = [];
    frameNames.clear();
    try
    {
      var root:Xml = Xml.parse(xmlText);
      collectFrames(root);
    }
    catch (e:Dynamic)
    {
      // Malformed XML — fall back to a tolerant regex scan so a slightly-off
      // atlas still yields its frame names.
      parseFallback();
    }
    return frames;
  }

  function collectFrames(node:Xml):Void
  {
    for (el in node.elements())
    {
      if (el.nodeName == 'SubTexture')
      {
        var name:String = el.get('name');
        if (name != null && name.length > 0)
        {
          var frame:AtlasFrame = {
            name: name,
            x: parseIntAttr(el, 'x'),
            y: parseIntAttr(el, 'y'),
            width: parseIntAttr(el, 'width') ?? parseIntAttr(el, 'frameWidth') ?? 0,
            height: parseIntAttr(el, 'height') ?? parseIntAttr(el, 'frameHeight') ?? 0
          };
          frames.push(frame);
          var key = normalize(name);
          if (!frameNames.exists(key)) frameNames.set(key, frame);
        }
      }
      else
      {
        collectFrames(el);
      }
    }
  }

  function parseIntAttr(el:Xml, key:String):Null<Int>
  {
    var v:String = el.get(key);
    if (v == null) return null;
    var n:Int = Std.parseInt(v);
    return n;
  }

  /**
   * Tolerant fallback for atlases that `Xml.parse` rejects — scans for
   * `<SubTexture ... name="X" .../>` tags with a regex.
   */
  function parseFallback():Void
  {
    var re = ~/<SubTexture\s+([^>]+)\/>/g;
    var pos:Int = 0;
    while (re.matchSub(xmlText, pos))
    {
      var span = re.matchedPos();
      pos = span.pos + span.len;
      var attrs:String = re.matched(1);
      var name = extractAttr(attrs, 'name');
      if (name == null) continue;

      var frame:AtlasFrame = {
        name: name,
        x: Std.parseInt(extractAttr(attrs, 'x') ?? '0'),
        y: Std.parseInt(extractAttr(attrs, 'y') ?? '0'),
        width: Std.parseInt(extractAttr(attrs, 'width') ?? extractAttr(attrs, 'frameWidth') ?? '0'),
        height: Std.parseInt(extractAttr(attrs, 'height') ?? extractAttr(attrs, 'frameHeight') ?? '0')
      };
      frames.push(frame);
      var key = normalize(name);
      if (!frameNames.exists(key)) frameNames.set(key, frame);
    }
  }

  public static function extractAttr(attrs:String, key:String):Null<String>
  {
    var re = new EReg(key + '\\s*=\\s*"([^"]*)"', '');
    if (re.match(attrs)) return re.matched(1);
    var re2 = new EReg(key + "\\s*=\\s*'([^']*)'", '');
    if (re2.match(attrs)) return re2.matched(1);
    return null;
  }

  static function normalize(s:String):String
  {
    return s.toLowerCase();
  }

  /**
   * True if any frame name starts with `prefix` (case-insensitive). Handles the
   * common Psych suffix convention "name0000".
   */
  public function hasPrefix(prefix:String):Bool
  {
    if (prefix == null || prefix.length == 0) return false;
    var lp = normalize(prefix);
    for (f in frames)
    {
      var n = normalize(f.name);
      if (n == lp || n.startsWith(lp)) return true;
    }
    return false;
  }

  /**
   * Resolve a frame name for a given logical prefix. Returns the first frame
   * whose name matches `prefix` exactly or as a prefix (e.g. "arrowUP" ->
   * "arrowUP0000"). Returns null when nothing matches.
   */
  public function resolveFrame(prefix:String):Null<AtlasFrame>
  {
    if (prefix == null) return null;
    var lp = normalize(prefix);
    // exact match first
    var exact = frameNames.get(lp);
    if (exact != null) return exact;
    // prefix match
    for (f in frames)
    {
      var n = normalize(f.name);
      if (n.startsWith(lp)) return f;
    }
    return null;
  }

  public function getFrame(name:String):Null<AtlasFrame>
  {
    return frameNames.get(normalize(name));
  }

  public function toStrings():Array<String>
  {
    return frames.map(f -> f.name);
  }
}

typedef AtlasFrame = {
  name:String,
  x:Int,
  y:Int,
  width:Int,
  height:Int
}
