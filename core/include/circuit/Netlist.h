#pragma once

#include "circuit/Circuit.h"

#include <array>
#include <map>
#include <string>
#include <vector>

namespace circuitlab {

struct ComponentNodes {
    std::array<int, kMaxComponentPins> pins{};
    int count = 0;
    // Peltier modules only: the node whose voltage is the cold face
    // temperature in degC, or -1. It is a real SPICE node but not an
    // electrical net, so it stays out of `nodeNumbers`.
    int thermalNode = -1;

    int a() const { return count > 0 ? pins[0] : -1; }
    int b() const { return count > 1 ? pins[1] : -1; }
};

struct CompiledCircuit {
    bool ok = false;
    std::string error;
    std::string netlist;
    std::map<int, ComponentNodes> nodes;  // component id -> assigned SPICE nodes
    std::map<int, int> wireNodes;        // wire id -> assigned node
    std::vector<int> nodeNumbers;        // non-ground node numbers, sorted
    std::map<int, int> thermalNodes;     // component id -> cold face node
    std::vector<Point> connectionPoints; // pins and wire vertices for rendering
    int danglingPins = 0;
    int groundCount = 0;
};

// Builds the node map and a complete SPICE netlist (without .op/.tran).
CompiledCircuit compileCircuit(const Circuit& circuit);

// Naming shared by the netlist generator, the ngspice save command and the UI.
// Two-pin devices use their SPICE branch current; multi-pin devices get a 0 V
// sense source in series with every pin: V<NAME>P<pin> <node> <internal> 0.
std::string pinSenseSourceName(const Component& component, int pin);
// Element name actually written into the deck. It usually equals the component
// name, but instruments and connectors are displayed as SC1 / J1 while SPICE
// requires a resistor card, so they are emitted as RSC1 / RJ1.
std::string spiceElementName(const Component& component);
// Result key (lowercase) for the pin current: "vq1p0" or "r1".
std::string pinCurrentKey(const Component& component, int pin);
// Save expression: "i(vq1p0)" or "@r1[i]".
std::string pinCurrentSaveExpression(const Component& component, int pin);

// Convenience wrapper: returns an empty string and sets *error on failure.
std::string generateNetlist(const Circuit& circuit, std::string* error = nullptr);

}  // namespace circuitlab
