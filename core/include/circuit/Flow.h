#pragma once

#include "circuit/Circuit.h"
#include "circuit/Netlist.h"

#include <cstdint>
#include <map>
#include <vector>

namespace circuitlab {

// One conductive piece of the schematic. Wire pieces follow the wire's point
// order; device pieces run from pin A to pin B through the symbol.
struct FlowEdge {
    int vertexA = 0;
    int vertexB = 0;
    Point pointA;
    Point pointB;
    int wireId = 0;       // > 0 for a piece of copper wire
    int componentId = 0;  // > 0 for a device body
    int componentPin = -1; // >= 0 for a pin -> device-center edge
    int nodeA = -1;       // electrical node at vertexA
    int nodeB = -1;       // electrical node at vertexB
    double current = 0.0; // amperes, positive from vertexA to vertexB

    bool isDevice() const { return componentId != 0; }
};

struct FlowNetwork {
    bool ok = false;
    std::vector<Point> vertices;
    std::vector<FlowEdge> edges;
    std::vector<std::vector<int>> incident;  // vertex -> edge indices
    std::vector<int> groundPins;             // vertices holding a ground pin
};

// Splits every wire at pins and at other wires' vertices, and adds one edge
// per two-pin component. Unlike the old per-wire animation this makes wire
// pieces between junctions part of the same conductor graph.
FlowNetwork buildFlowNetwork(const Circuit& circuit, const CompiledCircuit& compiled);

// Fills FlowEdge::current for every edge. `pinCurrent` is keyed by component
// id and holds one current per pin, positive into the device. Wire piece
// currents are solved from Kirchhoff's law on each node's copper graph, so a
// wire that does not touch a component directly still carries the current of
// its route.
void solveFlowCurrents(FlowNetwork& network,
                       const std::map<int, std::vector<double>>& pinCurrent);

// Convenience overload for the old two-pin device-current map. Positive follows
// pin A -> pin B.
void solveFlowCurrents(FlowNetwork& network,
                       const std::map<int, double>& deviceCurrent);

// Picks where a charge carrier continues after reaching `vertex` along
// `fromEdge`. Candidates are outgoing edges weighted by their current, so a
// branch splits the stream in proportion to the circuit current. Returns -1
// when the current has nowhere to continue. `rng` is advanced by the choice.
int flowChooseOutgoing(const FlowNetwork& network, int vertex, int fromEdge,
                       uint32_t* rng);

}  // namespace circuitlab
