#pragma once

#include <string>

namespace circuitlab {

// Returns the .model card for a built-in device model, or an empty string for
// an unknown name. Cards are emitted once per netlist.
std::string spiceModelCard(const std::string& modelName);

// Per-instance zener model so the user can edit the breakdown voltage.
std::string zenerModelCard(const std::string& instanceName, double breakdownVoltage);

// Built-in behavioral subcircuit card. Returns an empty string when the name
// is unknown.
std::string spiceSubcircuitCard(const std::string& subcircuitName);

}  // namespace circuitlab
