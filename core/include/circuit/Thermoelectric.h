#pragma once

#include "circuit/Circuit.h"

#include <string>

namespace circuitlab {

// Datasheet parameters of a Peltier (thermoelectric) module. The module is
// described by the three classic coefficients plus its thermal masses; the
// electrical behaviour is V = I * R + alpha * (Th - Tc).
struct ThermoelectricSpec {
    const char* model = "";       // "TEC1-12706"
    int couples = 0;              // number of PN couples (127 for both parts)
    double seebeck = 0.0;         // V/K   module Seebeck coefficient (alpha)
    double resistance = 0.0;      // ohm   module resistance at DC
    double conductance = 0.0;     // W/K   heat conducted between the two faces
    // Heat capacity of the cold ceramic/copper, as modelled. A real plate is
    // 10..20 J/K, which would take a minute to settle; the modelled value is
    // small on purpose so the cooldown fits in the app's transient window. The
    // temperatures themselves are the real ones.
    double coldCapacity = 0.0;    // J/K
    double maxVoltage = 0.0;      // V     datasheet Vmax
    double maxCurrent = 0.0;      // A     datasheet Imax
    double maxPumping = 0.0;      // W     datasheet Qmax
    double maxDeltaT = 0.0;       // K     datasheet dTmax (hot face at 27 degC)
    double ratedVoltage = 0.0;    // V     nominal 12 V operating point
    double ratedCurrent = 0.0;    // A
    double ratedPower = 0.0;      // W     electrical input at the nominal point
};

// Datasheet parameters for a TEC component type, or nullptr.
const ThermoelectricSpec* thermoelectricSpec(ComponentType type);
bool isThermoelectricType(ComponentType type);
// Hot face temperature the model pins the heat sink to (degC).
double thermoelectricHotSide();

// One operating point of the module.
struct ThermoelectricPoint {
    double current = 0.0;          // A, positive = pumping heat out of the cold face
    double voltage = 0.0;          // V across the terminals
    double coldTemperature = 0.0;  // degC
    double deltaT = 0.0;           // K, hot minus cold
    double heatPumped = 0.0;       // W removed from the cold face (Qc)
    double heatRejected = 0.0;     // W dumped into the hot face (Qh)
    double inputPower = 0.0;       // W
    double cop = 0.0;              // heatPumped / inputPower
    double reachable = 0.0;        // achieved deltaT / datasheet deltaTmax
};

// Steady state of a module driven by `appliedVoltage` with the hot face held at
// `hotC` and `heatLoadW` leaking into the cold face. Returns false when the
// module cannot pump any heat at that voltage (nothing to solve).
bool solveThermoelectric(const ThermoelectricSpec& spec, double appliedVoltage,
                         double hotC, double heatLoadW, ThermoelectricPoint* out);

// Operating point derived from what a simulation actually measured: terminal
// voltage, branch current and the cold face temperature of that instant.
void thermoelectricFromMeasurement(const ThermoelectricSpec& spec, double voltage,
                                   double current, double coldC,
                                   ThermoelectricPoint* out);

// Behavioural subcircuit for one instance: the electrical branch (resistance
// plus Seebeck back-EMF) and the lumped thermal branch of the cold face. The
// thermal node's voltage is the cold face temperature in degC, so the app can
// read it straight out of the simulation.
std::string thermoelectricSubcircuitName(const Component& component);
std::string thermoelectricSubcircuitCard(const Component& component);

}  // namespace circuitlab
