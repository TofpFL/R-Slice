// Small sprite-atlas loader for the V-Slice title screen assets.
// Supports the two formats shipped in assets/title/:
//   - Adobe Animate XML atlases (<SubTexture name x y width height
//     frameX frameY frameWidth frameHeight/>) - gfDanceTitle, titleEnter
//   - Adobe "ATLAS/SPRITES" JSON atlases (spritemap*.json) - stage, speakers
#pragma once

#include <map>
#include <string>
#include <string_view>

#include "raylib.h"

namespace gh {

struct AtlasFrame {
    Rectangle src{};  // crop rectangle in the texture, pixels
    float offsetX = 0, offsetY = 0;  // trimmed-frame offset (frameX/frameY)
    float w = 0, h = 0;              // logical frame size (frameWidth/frameHeight)
    bool rotated = false;            // stored rotated 90 degrees clockwise
};

struct TitleAtlas {
    Texture2D tex{};
    std::map<std::string, AtlasFrame> frames;
    const AtlasFrame* find(std::string_view name) const;
};

bool loadAtlasXml(const std::string& pngPath, const std::string& xmlPath, TitleAtlas& out,
                  std::string* err = nullptr);
bool loadAtlasJson(const std::string& pngPath, const std::string& jsonPath, TitleAtlas& out,
                   std::string* err = nullptr);

// "gfDance" + 7 -> "gfDance0007"
std::string frameName(const std::string& prefix, int index, int pad = 4);

}  // namespace gh
