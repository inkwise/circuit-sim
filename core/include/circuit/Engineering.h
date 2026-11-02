#pragma once

#include "circuit/Circuit.h"

#include <string>

namespace circuitlab {

// Human-readable number with an SI prefix, e.g. 2500 -> "2.5 kΩ",
// 1e-6 -> "1 µF", 0.5 -> "500 m".
std::string formatSI(double value, const char* unit, int significantDigits = 3);

// Label shown under a component on the schematic.
std::string componentValueLabel(const Component& component);

// A compact axis label without a trailing unit space.
std::string formatAxis(double value, const char* unit);

}  // namespace circuitlab
