#pragma once

#include "circuit/Circuit.h"

#include "imgui.h"

namespace circuitlab {

// Compact schematic glyph used by the component picker and other UI cards.
// `size` is the full icon extent in pixels.
void drawComponentGlyph(ImDrawList* drawList, ImVec2 center, float size,
                        ComponentType type, ImU32 color);

}  // namespace circuitlab
