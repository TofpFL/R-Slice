package funkin.graphics.shaders;

import flixel.addons.display.FlxRuntimeShader;
import funkin.Assets;
import funkin.Paths;

/**
 * Recolors channel-mask sprites using a JS Engine-style RGB palette.
 * The sprite's red channel is scaled by `_r` (main color), green by `_g`
 * (highlight), and blue by `_b` (dark shade).
 */
@:nullSafety
class RGBPaletteShader extends FlxRuntimeShader
{
  public function new()
  {
    super(Assets.getText(Paths.frag('rgbpalette')));
  }

  /**
   * @param colors Three [r,g,b] colors (0-255) for the main, highlight, and dark channels.
   */
  public function setPalette(colors:Array<Array<Float>>):Void
  {
    if (colors.length < 3) return;
    setFloatArray('_r', normalize(colors[0]));
    setFloatArray('_g', normalize(colors[1]));
    setFloatArray('_b', normalize(colors[2]));
  }

  static function normalize(color:Array<Float>):Array<Float>
  {
    return [color[0] / 255, color[1] / 255, color[2] / 255];
  }
}
