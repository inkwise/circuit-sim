#include "circuit/Flow.h"

#include "circuit/ComponentCatalog.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace circuitlab {
namespace {

using PointKey = std::pair<int, int>;

PointKey keyOf(Point point) { return {point.x, point.y}; }

bool pointOnSegment(Point point, Point a, Point b)
{
    const long long cross =
        static_cast<long long>(point.x - a.x) * (b.y - a.y) -
        static_cast<long long>(point.y - a.y) * (b.x - a.x);
    if (cross != 0) {
        return false;
    }
    return point.x >= std::min(a.x, b.x) && point.x <= std::max(a.x, b.x) &&
           point.y >= std::min(a.y, b.y) && point.y <= std::max(a.y, b.y);
}

}  // namespace

FlowNetwork buildFlowNetwork(const Circuit& circuit, const CompiledCircuit& compiled)
{
    FlowNetwork network;
    if (!compiled.ok) {
        return network;
    }

    // Every wire vertex and every pin is a place where the copper can be cut
    // into a smaller piece or where a device attaches.
    std::set<PointKey> splitPoints;
    for (const auto& wire : circuit.wires) {
        for (const auto& point : wire.points) {
            splitPoints.insert(keyOf(point));
        }
    }
    for (const auto& component : circuit.components) {
        Point pins[kMaxComponentPins];
        const int count = componentPins(component, pins);
        for (int pin = 0; pin < count; ++pin) {
            splitPoints.insert(keyOf(pins[pin]));
        }
    }

    std::map<PointKey, int> vertexOf;
    network.vertices.reserve(splitPoints.size());
    for (const auto& key : splitPoints) {
        vertexOf[key] = static_cast<int>(network.vertices.size());
        network.vertices.push_back(Point{key.first, key.second});
    }

    // Wire pieces, split at junctions and at any pin touching the segment.
    for (const auto& wire : circuit.wires) {
        const auto nodeEntry = compiled.wireNodes.find(wire.id);
        if (nodeEntry == compiled.wireNodes.end() || wire.points.size() < 2) {
            continue;
        }
        const int node = nodeEntry->second;
        for (size_t segment = 0; segment + 1 < wire.points.size(); ++segment) {
            const Point a = wire.points[segment];
            const Point b = wire.points[segment + 1];
            std::vector<std::pair<long long, Point>> onSegment;
            for (const auto& key : splitPoints) {
                const Point point{key.first, key.second};
                if (!pointOnSegment(point, a, b)) {
                    continue;
                }
                const long long dx = static_cast<long long>(point.x - a.x);
                const long long dy = static_cast<long long>(point.y - a.y);
                onSegment.push_back({dx * dx + dy * dy, point});
            }
            std::sort(onSegment.begin(), onSegment.end(),
                      [](const std::pair<long long, Point>& lhs,
                         const std::pair<long long, Point>& rhs) {
                          if (lhs.first != rhs.first) {
                              return lhs.first < rhs.first;
                          }
                          if (lhs.second.x != rhs.second.x) {
                              return lhs.second.x < rhs.second.x;
                          }
                          return lhs.second.y < rhs.second.y;
                      });
            for (size_t i = 0; i + 1 < onSegment.size(); ++i) {
                const Point from = onSegment[i].second;
                const Point to = onSegment[i + 1].second;
                if (from == to) {
                    continue;
                }
                FlowEdge edge;
                edge.vertexA = vertexOf[keyOf(from)];
                edge.vertexB = vertexOf[keyOf(to)];
                edge.pointA = from;
                edge.pointB = to;
                edge.wireId = wire.id;
                edge.nodeA = node;
                edge.nodeB = node;
                network.edges.push_back(edge);
            }
        }
    }

    // Device bodies and ground pins.
    for (const auto& component : circuit.components) {
        Point pins[kMaxComponentPins];
        const int count = componentPins(component, pins);
        if (component.type == ComponentType::Ground) {
            if (count > 0) {
                const auto found = vertexOf.find(keyOf(pins[0]));
                if (found != vertexOf.end()) {
                    network.groundPins.push_back(found->second);
                }
            }
            continue;
        }
        if (count < 2) {
            continue;
        }
        const auto nodes = compiled.nodes.find(component.id);
        if (nodes == compiled.nodes.end()) {
            continue;
        }
        if (count <= 2 && !needsPinSensing(component.type)) {
            FlowEdge edge;
            edge.vertexA = vertexOf[keyOf(pins[0])];
            edge.vertexB = vertexOf[keyOf(pins[1])];
            edge.pointA = pins[0];
            edge.pointB = pins[1];
            edge.componentId = component.id;
            edge.nodeA = nodes->second.a();
            edge.nodeB = nodes->second.b();
            network.edges.push_back(edge);
            continue;
        }

        // Multi-pin devices (and two-pin subcircuits such as NOT) get a
        // synthetic center vertex and one edge per pin, so every pin current
        // can feed the wire-current solver independently.
        const int centerIndex = static_cast<int>(network.vertices.size());
        network.vertices.push_back(Point{component.gridX, component.gridY});
        for (int pin = 0; pin < count; ++pin) {
            FlowEdge edge;
            edge.vertexA = vertexOf[keyOf(pins[pin])];
            edge.vertexB = centerIndex;
            edge.pointA = pins[pin];
            edge.pointB = network.vertices[static_cast<size_t>(centerIndex)];
            edge.componentId = component.id;
            edge.componentPin = pin;
            edge.nodeA = nodes->second.pins[static_cast<size_t>(pin)];
            edge.nodeB = -1;
            network.edges.push_back(edge);
        }
    }

    network.incident.assign(network.vertices.size(), {});
    for (int index = 0; index < static_cast<int>(network.edges.size()); ++index) {
        const FlowEdge& edge = network.edges[static_cast<size_t>(index)];
        network.incident[static_cast<size_t>(edge.vertexA)].push_back(index);
        network.incident[static_cast<size_t>(edge.vertexB)].push_back(index);
    }
    network.ok = true;
    return network;
}

void solveFlowCurrents(FlowNetwork& network,
                       const std::map<int, std::vector<double>>& pinCurrent)
{
    for (auto& edge : network.edges) {
        edge.current = 0.0;
    }

    std::map<int, std::vector<int>> nodeWires;
    std::map<int, std::map<int, double>> injection;  // node -> vertex -> amperes

    for (int index = 0; index < static_cast<int>(network.edges.size()); ++index) {
        FlowEdge& edge = network.edges[static_cast<size_t>(index)];
        if (!edge.isDevice()) {
            if (edge.nodeA >= 0) {
                nodeWires[edge.nodeA].push_back(index);
            }
            continue;
        }
        const auto found = pinCurrent.find(edge.componentId);
        if (edge.componentPin >= 0) {
            // Pin -> synthetic center edge. Positive current flows into the
            // device, so the wire at that pin loses the same amount.
            if (found != pinCurrent.end() &&
                edge.componentPin < static_cast<int>(found->second.size())) {
                const double value = found->second[static_cast<size_t>(edge.componentPin)];
                if (std::isfinite(value)) {
                    edge.current = value;
                }
            }
            if (edge.nodeA >= 0) {
                injection[edge.nodeA][edge.vertexA] -= edge.current;
            }
            continue;
        }
        // Two-pin device: pin 0 current follows A -> B.
        if (found != pinCurrent.end() && !found->second.empty() &&
            std::isfinite(found->second[0])) {
            edge.current = found->second[0];
        }
        if (edge.nodeA >= 0) {
            injection[edge.nodeA][edge.vertexA] -= edge.current;
        }
        if (edge.nodeB >= 0) {
            injection[edge.nodeB][edge.vertexB] += edge.current;
        }
    }

    std::set<int> groundPins(network.groundPins.begin(), network.groundPins.end());

    for (const auto& entry : nodeWires) {
        const int node = entry.first;
        const std::vector<int>& wireEdges = entry.second;

        std::map<int, std::vector<int>> adjacency;  // vertex -> edge indices
        for (int index : wireEdges) {
            const FlowEdge& edge = network.edges[static_cast<size_t>(index)];
            adjacency[edge.vertexA].push_back(index);
            adjacency[edge.vertexB].push_back(index);
        }
        if (adjacency.empty()) {
            continue;
        }

        std::map<int, double> atVertex;
        const auto nodeInjection = injection.find(node);
        if (nodeInjection != injection.end()) {
            for (const auto& item : nodeInjection->second) {
                if (adjacency.find(item.first) != adjacency.end()) {
                    atVertex[item.first] += item.second;
                }
            }
        }
        for (const auto& item : adjacency) {
            atVertex.emplace(item.first, 0.0);
        }

        std::set<int> visited;
        for (const auto& start : adjacency) {
            if (visited.find(start.first) != visited.end()) {
                continue;
            }

            // Walk one connected copper island.
            std::vector<int> order;
            std::map<int, int> parent;
            std::map<int, int> parentEdge;
            std::vector<int> stack{start.first};
            visited.insert(start.first);
            parent[start.first] = -1;
            parentEdge[start.first] = -1;
            while (!stack.empty()) {
                const int vertex = stack.back();
                stack.pop_back();
                order.push_back(vertex);
                for (int index : adjacency[vertex]) {
                    const FlowEdge& edge = network.edges[static_cast<size_t>(index)];
                    const int other =
                        edge.vertexA == vertex ? edge.vertexB : edge.vertexA;
                    if (visited.find(other) != visited.end()) {
                        continue;
                    }
                    visited.insert(other);
                    parent[other] = vertex;
                    parentEdge[other] = index;
                    stack.push_back(other);
                }
            }

            double total = 0.0;
            for (int vertex : order) {
                total += atVertex[vertex];
            }
            if (std::fabs(total) > 1e-15) {
                // Unbalanced copper is either current entering/leaving through
                // a ground symbol or a component without a saved current. Put
                // the correction on the ground pins when there are any, so the
                // flow stays continuous inside this island.
                std::vector<int> grounds;
                if (node == 0) {
                    for (int vertex : order) {
                        if (groundPins.find(vertex) != groundPins.end()) {
                            grounds.push_back(vertex);
                        }
                    }
                }
                if (!grounds.empty()) {
                    const double share = -total / static_cast<double>(grounds.size());
                    for (int vertex : grounds) {
                        atVertex[vertex] += share;
                    }
                } else {
                    atVertex[start.first] -= total;
                }
            }

            std::map<int, double> subtree;
            for (int vertex : order) {
                subtree[vertex] = atVertex[vertex];
            }
            for (auto it = order.rbegin(); it != order.rend(); ++it) {
                const int vertex = *it;
                const int up = parent[vertex];
                if (up >= 0) {
                    subtree[up] += subtree[vertex];
                }
            }
            for (int vertex : order) {
                const auto edgeIt = parentEdge.find(vertex);
                if (edgeIt == parentEdge.end() || edgeIt->second < 0) {
                    continue;
                }
                FlowEdge& edge = network.edges[static_cast<size_t>(edgeIt->second)];
                const int up = parent[vertex];
                const double value = subtree[vertex];
                if (edge.vertexA == up && edge.vertexB == vertex) {
                    edge.current = -value;
                } else if (edge.vertexA == vertex && edge.vertexB == up) {
                    edge.current = value;
                }
            }
        }
    }
}

void solveFlowCurrents(FlowNetwork& network,
                       const std::map<int, double>& deviceCurrent)
{
    std::map<int, std::vector<double>> pinCurrent;
    for (const auto& entry : deviceCurrent) {
        pinCurrent[entry.first] = {entry.second, -entry.second};
    }
    solveFlowCurrents(network, pinCurrent);
}

namespace {

bool leavesVertex(const FlowEdge& edge, int vertex, double epsilon)
{
    if (!std::isfinite(edge.current) || std::fabs(edge.current) < epsilon) {
        return false;
    }
    if (edge.vertexA == vertex && edge.vertexB != vertex) {
        return edge.current > 0.0;
    }
    if (edge.vertexB == vertex && edge.vertexA != vertex) {
        return edge.current < 0.0;
    }
    return false;
}

}  // namespace

int flowChooseOutgoing(const FlowNetwork& network, int vertex, int fromEdge,
                       uint32_t* rng)
{
    if (vertex < 0 ||
        vertex >= static_cast<int>(network.incident.size())) {
        return -1;
    }
    constexpr double kEpsilon = 1e-9;
    const std::vector<int>& incident =
        network.incident[static_cast<size_t>(vertex)];

    double total = 0.0;
    int fallback = -1;
    for (int candidate : incident) {
        if (candidate == fromEdge) {
            continue;
        }
        const FlowEdge& edge = network.edges[static_cast<size_t>(candidate)];
        if (!leavesVertex(edge, vertex, kEpsilon)) {
            continue;
        }
        total += std::fabs(edge.current);
        fallback = candidate;
    }
    if (!(total > 0.0)) {
        return -1;
    }
    if (!rng) {
        return fallback;
    }

    uint32_t state = *rng;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    *rng = state;
    double pick = (static_cast<double>(state & 0xffffffu) / 16777216.0) * total;
    for (int candidate : incident) {
        if (candidate == fromEdge) {
            continue;
        }
        const FlowEdge& edge = network.edges[static_cast<size_t>(candidate)];
        if (!leavesVertex(edge, vertex, kEpsilon)) {
            continue;
        }
        pick -= std::fabs(edge.current);
        if (pick <= 0.0) {
            return candidate;
        }
    }
    return fallback;
}

}  // namespace circuitlab
