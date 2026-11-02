#pragma once

#include "circuit/Circuit.h"

#include <vector>

namespace circuitlab {

// Where a wire is allowed to start or end: every component pin and every
// existing wire vertex (the junction dots).
std::vector<Point> connectionPoints(const Circuit& circuit);

// True when the point is a pin of some component or a vertex of some wire.
bool isConnectionPoint(const Circuit& circuit, Point point);
// The connection point nearest to `point` inside `radius` grid cells.
bool nearestConnectionPoint(const Circuit& circuit, Point point, int radius, Point* out);

// Routes an orthogonal wire from `start` to `end`.
//
// The route only touches the circuit where it is supposed to:
//   - it never runs over a pin or over a wire vertex or endpoint, because that
//     would silently join two nets;
//   - it never runs on top of an existing wire for the same reason;
//   - it crosses other wires as rarely as possible (a crossing is drawn as a
//     jumper bridge and stays electrically separate), and it prefers routes
//     that stay out of component bodies.
// Returns false when no clean route exists; the caller can then fall back to a
// plain L shape.
bool routeWire(const Circuit& circuit, Point start, Point end,
               std::vector<Point>* route);

// The plain two-corner route used as a fallback (and by the plain wire tool).
std::vector<Point> lShapedRoute(Point start, Point end);

}  // namespace circuitlab
