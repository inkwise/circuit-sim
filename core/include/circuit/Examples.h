#pragma once

#include "circuit/Circuit.h"

namespace circuitlab {

Circuit makeDividerDemo();
Circuit makeRcDemo();
Circuit makeTimer555Demo();
// 12 V PWM switch board: high-side P-MOSFET chop plus an LC output filter and
// an oscilloscope probe.
Circuit makeMosSwitchBoardDemo();

}  // namespace circuitlab
