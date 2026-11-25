package funkin.noteskins;

import haxe.Json;

/**
 * Reads TexturePacker JSON atlases (`{"frames": {...}, "meta": {...}}` hash
 * format and `{"frames": [...]}` array format) and converts them to the
 * Sparrow XML dialect that `FlxAtlasFrames.fromSparrow` — and therefore the
 * whole V-Slice noteskin pipeline — already understands.
 *
 * Conversion rules mirror what TexturePacker's own Sparrow exporter emits:
 *   frame{x,y,w,h}            -> SubTexture x/y/width/height
 *   spriteSourceSize{x,y}     -> frameX/frameY
 *   sourceSize{w,h}           -> frameWidth/frameHeight
 *   rotated                   -> rotated="true"
 *
 * Frames without trim data (spriteSourceSize == full sourceSize) are emitted
 * untrimmed, matching how untrimmed Sparrow frames behave in Flixel.
 */
@:nullSafety
class TexturePackerAtlas
{
  /**
   * Parse TexturePacker JSON text. Returns `null` when the document does not
   * look like a TexturePacker atlas (no `frames` collection).
   */
  public static function parseFrames(jsonText:String):Null<Array<SkinAtlasParser.AtlasFrame>>
  {
    var doc:Dynamic = try Json.parse(jsonText) catch (e:Dynamic) null;
    if (doc == null || doc.frames == null) return null;

    var frames:Array<SkinAtlasParser.AtlasFrame> = [];
    var rawFrames:Dynamic = doc.frames;

    if (Std.is(rawFrames, Array))
    {
      for (f in (rawFrames : Array<Dynamic>))
      {
        var frame = convertEntry(f);
        if (frame != null) frames.push(frame);
      }
    }
    else
    {
      // Hash format: {"frames": {"name": {...}, ...}}
      try
      {
        for (name in Reflect.fields(rawFrames))
        {
          var frame = convertEntry(Reflect.field(rawFrames, name), name);
          if (frame != null) frames.push(frame);
        }
      }
      catch (e:Dynamic) {}
    }

    return frames.length > 0 ? frames : null;
  }

  /** True when `jsonText` parses as a TexturePacker atlas document. */
  public static function isTexturePackerAtlas(jsonText:String):Bool
  {
    return parseFrames(jsonText) != null;
  }

  /**
   * Convert a TexturePacker JSON atlas into a complete Sparrow XML document
   * string, suitable for `SkinAtlasParser` / `ReceptorFramePatcher` / the
   * runtime `FlxAtlasFrames.fromSparrow` loader.
   *
   * @param imageName File name written into `imagePath` (cosmetic — V-Slice
   *                  resolves the sibling PNG by convention).
   */
  public static function toSparrowXml(jsonText:String, imageName:String = "atlas.png"):Null<String>
  {
    var doc:Dynamic = try Json.parse(jsonText) catch (e:Dynamic) null;
    if (doc == null || doc.frames == null) return null;

    var out = new StringBuf();
    out.add('<TextureAtlas imagePath="');
    out.add(xmlEscape(imageName));
    out.add('">\n');

    var rawFrames:Dynamic = doc.frames;
    if (Std.is(rawFrames, Array))
    {
      for (f in (rawFrames : Array<Dynamic>)) writeSubTexture(out, f, null);
    }
    else
    {
      // Hash format: {"frames": {"name": {...}, ...}}
      try
      {
        for (name in Reflect.fields(rawFrames))
        {
          writeSubTexture(out, Reflect.field(rawFrames, name), name);
        }
      }
      catch (e:Dynamic) {}
    }

    out.add('</TextureAtlas>');
    return out.toString();
  }

  static function convertEntry(f:Dynamic, ?hashName:Null<String>):Null<SkinAtlasParser.AtlasFrame>
  {
    var name:Null<String> = hashName ?? (f != null ? str(f.filename) : null);
    if (name == null || name.length == 0) return null;

    var rect = readRect(f.frame);
    if (rect == null) return null;

    var w:Int = rect.w;
    var h:Int = rect.h;
    var rotated:Bool = f.rotated == true || f.rotated == "true";
    // TexturePacker stores rotated frames with swapped on-atlas dimensions in
    // some exporters; Flixel's fromSparrow expects the stored rect as-is plus
    // rotated="true", so keep w/h untouched.
    var _ = rotated;

    return {
      name: name,
      x: rect.x,
      y: rect.y,
      width: w,
      height: h
    };
  }

  static function writeSubTexture(out:StringBuf, f:Dynamic, ?hashName:Null<String>):Void
  {
    var name:Null<String> = hashName ?? (f != null ? str(f.filename) : null);
    if (name == null || name.length == 0) return;

    var rect = readRect(f.frame);
    if (rect == null) return;

    var sss = readRect(f.spriteSourceSize);
    var src = readRect(f.sourceSize);
    var rotated:Bool = f.rotated == true || f.rotated == "true";

    out.add('  <SubTexture name="');
    out.add(xmlEscape(name));
    out.add('" x="');
    out.add(Std.string(rect.x));
    out.add('" y="');
    out.add(Std.string(rect.y));
    out.add('" width="');
    out.add(Std.string(rect.w));
    out.add('" height="');
    out.add(Std.string(rect.h));
    out.add('"');

    if (sss != null && src != null)
    {
      out.add(' frameX="');
      out.add(Std.string(sss.x));
      out.add('" frameY="');
      out.add(Std.string(sss.y));
      out.add('" frameWidth="');
      out.add(Std.string(src.w));
      out.add('" frameHeight="');
      out.add(Std.string(src.h));
      out.add('"');
    }
    if (rotated) out.add(' rotated="true"');
    out.add('/>\n');
  }

  /**
   * Read a rect given either as `{x,y,w,h}` / `{x,y,width,height}` object or
   * as `[x,y,w,h]` array (both appear in the wild).
   */
  static function readRect(v:Dynamic):Null<{x:Int, y:Int, w:Int, h:Int}>
  {
    if (v == null) return null;
    if (Std.is(v, Array))
    {
      var a:Array<Dynamic> = cast v;
      if (a.length < 4) return null;
      return {x: Std.parseInt(Std.string(a[0])), y: Std.parseInt(Std.string(a[1])),
              w: Std.parseInt(Std.string(a[2])), h: Std.parseInt(Std.string(a[3]))};
    }
    function field(n:String):Dynamic
    {
      return try Reflect.field(v, n) catch (e:Dynamic) null;
    }
    var x = field('x');
    var y = field('y');
    var w = field('w') ?? field('width');
    var h = field('h') ?? field('height');
    if (x == null || y == null || w == null || h == null) return null;
    return {x: Std.parseInt(Std.string(x)), y: Std.parseInt(Std.string(y)),
            w: Std.parseInt(Std.string(w)), h: Std.parseInt(Std.string(h))};
  }

  static function str(v:Dynamic):Null<String>
  {
    if (v == null) return null;
    return Std.string(v);
  }

  static function xmlEscape(s:String):String
  {
    return StringTools.htmlEscape(s);
  }
}
