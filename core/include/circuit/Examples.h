#pragma once

#include "circuit/Circuit.h"

namespace circuitlab {

Circuit makeDividerDemo();
Circuit makeRcDemo();
Circuit makeTimer555Demo();
// 12 V PWM switch board: high-side P-MOSFET chop plus an LC output filter and
// an oscilloscope probe.
Circuit makeMosSwitchBoardDemo();
// A Peltier cooler on a 12 V supply, cooling a small heat load from room
// temperature: the plate starts at 20 °C and the cold face drops from there.
Circuit makeThermoelectricDemo();

}  // namespace circuitlab
