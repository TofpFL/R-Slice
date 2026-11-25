// .rvfx — timed shader-effect script. A short line-based file that schedules
// shader clips at song times; each line compiles to one VisualClip, so the
// existing visuals.json runtime (play_visuals.cpp) drives it unchanged.
//
// Format (one clip per line, '#' comments, blank lines ignored):
//
//   <startMs> [endMs|-] [@target] <fx> [k=v ...]
//
//   startMs   clip start, song milliseconds (required, >= 0)
//   endMs     clip end; "-" or omitted = until the song ends
//   @target   game | hud | both (default: game)
//   fx        built-in effect name or shaders/<name>.frag resolved through
//             the song's asset roots (same rules as visuals.json)
//   k=v       float uniforms applied to the shader instance
//
// Example:
//   1200 glitch
//   3500 - @both shaders/pulse.frag uSpeed=2.5 uStrength=0.8
//
// Validation: malformed lines are errors naming the file and line number.
// Missing shaders are reported by the visuals runtime at resolve time.
#pragma once

#include <string>
#include <vector>

#include "visuals.hpp"

namespace gh {

// Parses .rvfx text into VisualClips (appended to `out`). `sourceName` is
// used in error messages (file path or "<memory>").
bool parseRvfx(const std::string& text, const std::string& sourceName,
               std::vector<VisualClip>& out, std::string& err);

}  // namespace gh
