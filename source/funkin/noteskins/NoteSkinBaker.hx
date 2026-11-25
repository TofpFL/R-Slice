package funkin.noteskins;

import haxe.io.Bytes;
import openfl.display.BitmapData;
import openfl.display.PNGEncoderOptions;
import openfl.utils.ByteArray;

/**
 * Bakes JS Engine / Psych Engine RGB-palette-mask atlases into plain colored
 * pixels at import time.
 *
 * Many Psych noteskins ship a single "mask" texture: the falling-note and
 * receptor-glow frames store lane colors as pure red / green / blue channels,
 * and JS Engine recolors them at runtime with `RGBPaletteShader`
 * (`out = c.r*main + c.g*white + c.b*dark` per channel). V-Slice has no such
 * shader path, so without baking the notes render as raw red/green/blue masks.
 *
 * This class detects the mask signature (lots of near-primary pixels in the
 * note frames) and, when found, rewrites the PNG with the JS default lane
 * palettes (`ClientPrefs.arrowRGB`) already applied — matching what V-Slice's
 * own built-in atlases look like (fully colored arrows).
 *
 * Static receptor frames (`arrow*`) are deliberately NOT baked: JS Engine
 * leaves them unshaded and V-Slice's native receptors are raw grayscale art.
 */
@:nullSafety
class NoteSkinBaker
{
  /**
   * JS Engine `ClientPrefs.arrowRGB` per direction (0=left,1=down,2=up,3=right):
   * [main color, white highlight, dark shade] — texel R/G/B channels map to
   * palette.r / palette.g / palette.b respectively.
   */
  static final PALETTE:Array<Array<Array<Float>>> = [
    [[0xC2 / 255, 0x4B / 255, 0x99 / 255], [1, 1, 1], [0x3C / 255, 0x1F / 255, 0x56 / 255]], // left  (purple)
    [[0, 1, 1], [1, 1, 1], [0x15 / 255, 0x42 / 255, 0xB7 / 255]],                               // down  (cyan)
    [[0x12 / 255, 0xFA / 255, 0x05 / 255], [1, 1, 1], [0x0A / 255, 0x44 / 255, 0x47 / 255]],   // up    (green)
    [[0xF9 / 255, 0x39 / 255, 0x3F / 255], [1, 1, 1], [0x65 / 255, 0x10 / 255, 0x38 / 255]]    // right (red)
  ];

  /** Color-name -> direction index (JS Engine colArray order). */
  static final DIR_FOR_COLOR:Map<String, Int> = [
    'purple' => 0,
    'blue' => 1,
    'green' => 2,
    'red' => 3
  ];

  /** Minimum near-primary pixel count across note frames to treat atlas as a mask. */
  static final MASK_THRESHOLD:Int = 60;

  /**
   * Bake the atlas PNG if it uses the palette-mask convention.
   *
   * @param pngBytes  Raw bytes of the source atlas PNG.
   * @param atlas     Parsed Sparrow frames.
   * @param prefixes  Result of `NoteSkinConverter.resolvePrefixes()`.
   * @param force     `true` = always bake, `false` = never, `null` = auto-detect.
   * @return          PNG bytes with palettes applied, or `null` when the atlas
   *                  isn't a mask atlas (caller should use the original bytes).
   */
  public static function bake(pngBytes:Bytes, atlas:SkinAtlasParser, prefixes:Dynamic, ?force:Null<Bool>):Null<Bytes>
  {
    var bitmap = BitmapData.fromBytes(ByteArray.fromBytes(pngBytes));
    if (bitmap == null) return null;

    var assignments = assignFrames(atlas, prefixes);
    if (assignments.length == 0) return null;

    var isMask = force == true || (force == null && detectMask(bitmap, atlas, prefixes));
    if (!isMask) return null;

    var changed:Bool = false;
    for (a in assignments)
    {
      changed = bakeFrame(bitmap, a.frame, PALETTE[a.dir]) || changed;
    }
    if (!changed) return null;

    var out:ByteArray = bitmap.encode(bitmap.rect, new PNGEncoderOptions());
    var result:haxe.io.Bytes = out;
    return result;
  }

  /**
   * Collect every frame that should receive a lane palette: resolved note,
   * press and confirm prefixes plus conventional `{color} hold piece/end`
   * frames (kept in sync for the future hold-strip work).
   */
  static function assignFrames(atlas:SkinAtlasParser, prefixes:Dynamic):Array<NoteSkinBakerFrameAssignment>
  {
    var result:Array<NoteSkinBakerFrameAssignment> = [];
    var seen = new Map<String, Bool>();

    function add(frame:SkinAtlasParser.AtlasFrame, dir:Int):Void
    {
      var key = frame.name.toLowerCase();
      if (seen.exists(key)) return;
      seen.set(key, true);
      result.push({frame: frame, dir: dir});
    }

    function matchPrefix(list:Null<Array<Null<String>>>, dir:Int):Null<String>
    {
      if (list == null || dir >= list.length) return null;
      return list[dir];
    }

    for (frame in atlas.frames)
    {
      var lname = frame.name.toLowerCase();

      // {color} hold piece / {color} hold end -> lane palette
      for (color => dir in DIR_FOR_COLOR)
      {
        if (lname.startsWith(color + ' hold'))
        {
          add(frame, dir);
          break;
        }
      }
      if (seen.exists(lname)) continue;

      if (prefixes != null)
      {
        var matched = false;
        var roles = [
          {list: prefixes.note, role: 0},
          {list: prefixes.press, role: 1},
          {list: prefixes.confirm, role: 2}
        ];
        for (r in roles)
        {
          var list:Null<Array<Null<String>>> = r.list;
          if (list == null) continue;
          for (dir in 0...4)
          {
            var p = matchPrefix(list, dir);
            if (p != null && p.length > 0 && lname.startsWith(p.toLowerCase()))
            {
              add(frame, dir);
              matched = true;
              break;
            }
          }
          if (matched) break;
        }
      }
    }
    return result;
  }

  /**
   * Detect the palette-mask signature: the frames the converter will use as
   * falling notes contain large numbers of pure primary-color pixels
   * (pure red/green/blue channel masks), which a normally-colored skin lacks.
   */
  static function detectMask(bitmap:BitmapData, atlas:SkinAtlasParser, prefixes:Dynamic):Bool
  {
    var primaryCount:Int = 0;
    var sampled:Int = 0;
    var notePrefixes:Array<Null<String>> = (prefixes != null && prefixes.note != null) ? prefixes.note : [];

    for (frame in atlas.frames)
    {
      var lname = frame.name.toLowerCase();
      var isNote = false;
      for (p in notePrefixes)
      {
        if (p != null && lname.startsWith(p.toLowerCase())) { isNote = true; break; }
      }
      if (!isNote) continue;

      var x0:Int = Std.int(Math.max(0, frame.x));
      var y0:Int = Std.int(Math.max(0, frame.y));
      var x1:Int = Std.int(Math.min(bitmap.width, frame.x + frame.width));
      var y1:Int = Std.int(Math.min(bitmap.height, frame.y + frame.height));
      if (x1 <= x0 || y1 <= y0) continue;

      var stride:Int = 5;
      var yy:Int = y0;
      while (yy < y1)
      {
        var xx:Int = x0;
        while (xx < x1)
        {
          var c:Int = bitmap.getPixel32(xx, yy);
          var a:Int = (c >>> 24) & 0xFF;
          if (a > 200)
          {
            sampled++;
            var r:Int = (c >>> 16) & 0xFF;
            var g:Int = (c >>> 8) & 0xFF;
            var b:Int = c & 0xFF;
            // near-pure channel: one channel dominant, others near zero
            if ((r > 240 && g < 20 && b < 20) || (g > 240 && r < 20 && b < 20) || (b > 240 && r < 20 && g < 20))
            {
              primaryCount++;
            }
          }
          xx += stride;
        }
        yy += stride;
      }
    }

    // Require a meaningful sample so tiny/degenerate frames can't false-positive.
    return sampled > 200 && primaryCount > MASK_THRESHOLD && primaryCount * 4 > sampled;
  }

  /**
   * Apply the JS palette transform to every pixel of `frame`'s atlas region.
   * Mirrors `RGBPaletteShader`: `out = min(c.r*R + c.g*G + c.b*B, 1)` where
   * R/G/B are the palette's main/white/dark colors and c.* are texel channels.
   */
  static function bakeFrame(bitmap:BitmapData, frame:SkinAtlasParser.AtlasFrame, palette:Array<Array<Float>>):Bool
  {
    var pr = palette[0];
    var pg = palette[1];
    var pb = palette[2];

    var x0:Int = Std.int(Math.max(0, frame.x));
    var y0:Int = Std.int(Math.max(0, frame.y));
    var x1:Int = Std.int(Math.min(bitmap.width, frame.x + frame.width));
    var y1:Int = Std.int(Math.min(bitmap.height, frame.y + frame.height));
    if (x1 <= x0 || y1 <= y0) return false;

    var changed:Bool = false;
    for (y in y0...y1)
    {
      for (x in x0...x1)
      {
        var c:Int = bitmap.getPixel32(x, y);
        var a:Int = (c >>> 24) & 0xFF;
        if (a < 8) continue;

        var r:Float = ((c >>> 16) & 0xFF) / 255;
        var g:Float = ((c >>> 8) & 0xFF) / 255;
        var b:Float = (c & 0xFF) / 255;

        var nr:Float = Math.min(r * pr[0] + g * pg[0] + b * pb[0], 1);
        var ng:Float = Math.min(r * pr[1] + g * pg[1] + b * pb[1], 1);
        var nb:Float = Math.min(r * pr[2] + g * pg[2] + b * pb[2], 1);

        var out:Int = (a << 24) | (Std.int(nr * 255) << 16) | (Std.int(ng * 255) << 8) | Std.int(nb * 255);
        if (out != c)
        {
          bitmap.setPixel32(x, y, out);
          changed = true;
        }
      }
    }
    return changed;
  }
}

typedef NoteSkinBakerFrameAssignment = {frame:SkinAtlasParser.AtlasFrame, dir:Int};
