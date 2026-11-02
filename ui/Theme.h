#pragma once

#include "imgui.h"

namespace Theme {

void apply(float scale);
void loadFont(float scale);

ImU32 background();
ImU32 panel();
ImU32 panelAlt();
ImU32 surface();
ImU32 surfaceHigh();
ImU32 stroke();
ImU32 accentSoft();
ImU32 canvas();
ImU32 text();
ImU32 textDim();
ImU32 muted();
ImU32 accent();
// Schematic copper. Deliberately a different hue from the white component
// symbols and the blue accent so wires never read as part of a part.
ImU32 wire();
ImU32 grid();
ImU32 axis();
ImU32 danger();
ImU32 warning();
ImU32 good();

// Categorical series palette (validated dark-mode ordering).
ImU32 seriesColor(int index);

ImVec4 toVec4(ImU32 color);

}  // namespace Theme
