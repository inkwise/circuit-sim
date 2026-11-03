#include "circuit/Router.h"

#include "circuit/ComponentCatalog.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace circuitlab {
namespace {

// Direction of travel on the grid; -1 is the start (no direction yet).
constexpr int kNone = 0;
constexpr int kHorizontal = 1;
constexpr int kVertical = 2;

// One grid point, packed for the hash sets.
uint64_t pack(Point point)
{
    return (static_cast<uint64_t>(static_cast<uint32_t>(point.x)) << 32) |
           static_cast<uint64_t>(static_cast<uint32_t>(point.y));
}

// A grid edge: `horizontal` edges run from (x, y) to (x + 1, y), vertical edges
// from (x, y) to (x, y + 1). Packing keeps both variants separate.
uint64_t packEdge(int x, int y, bool horizontal)
{
    return pack(Point{x, y}) | (horizontal ? 0x0ull : 0x100000000ull);
}

struct Costs {
    static constexpr int kStep = 16;      // one grid cell
    // A corner costs about two cells of detour: straight wires read better and
    // keep the saved project small (a staircase would be dozens of points).
    static constexpr int kTurn = 34;
    static constexpr int kCrossWire = 90; // bridging an unrelated wire
    static constexpr int kBody = 34;      // running through a component body
};

struct Obstacles {
    std::unordered_set<uint64_t> pins;
    std::unordered_set<uint64_t> wireVertices;
    std::unordered_set<uint64_t> wirePoints;         // every grid point on a wire
    std::unordered_set<uint64_t> horizontalEdges;    // occupied wire segments
    std::unordered_set<uint64_t> verticalEdges;
    std::unordered_set<uint64_t> bodyPoints;
};

void collectObstacles(const Circuit& circuit, Point start, Point end, Obstacles* obstacles)
{
    const uint64_t startKey = pack(start);
    const uint64_t endKey = pack(end);
    for (const auto& component : circuit.components) {
        Point ownPins[kMaxComponentPins];
        const int ownCount = componentPins(component, ownPins);
        bool ownsEndpoint = false;
        for (int index = 0; index < ownCount; ++index) {
            const uint64_t key = pack(ownPins[index]);
            if (key == startKey || key == endKey) {
                ownsEndpoint = true;
            }
        }
        Point pins[kMaxComponentPins];
        const int count = componentPins(component, pins);
        for (int index = 0; index < count; ++index) {
            obstacles->pins.insert(pack(pins[index]));
        }
        int halfWidth = 0;
        int halfHeight = 0;
        componentBodyHalfExtents(component, &halfWidth, &halfHeight);
        if (ownsEndpoint) {
            continue;  // the wire may leave through its own symbol
        }
        for (int x = component.gridX - halfWidth; x <= component.gridX + halfWidth; ++x) {
            for (int y = component.gridY - halfHeight; y <= component.gridY + halfHeight;
                 ++y) {
                obstacles->bodyPoints.insert(pack(Point{x, y}));
            }
        }
    }
    for (const auto& wire : circuit.wires) {
        for (size_t index = 0; index < wire.points.size(); ++index) {
            obstacles->wireVertices.insert(pack(wire.points[index]));
            if (index + 1 >= wire.points.size()) {
                continue;
            }
            Point a = wire.points[index];
            Point b = wire.points[index + 1];
            const int stepX = (b.x > a.x) - (b.x < a.x);
            const int stepY = (b.y > a.y) - (b.y < a.y);
            const bool horizontalSegment = stepX != 0;
            Point cursor = a;
            while (true) {
                obstacles->wirePoints.insert(pack(cursor));
                if (cursor == b) {
                    break;
                }
                const bool horizontal = horizontalSegment;
                if (horizontal) {
                    obstacles->horizontalEdges.insert(packEdge(cursor.x, cursor.y, true));
                } else {
                    obstacles->verticalEdges.insert(packEdge(cursor.x, cursor.y, false));
                }
                cursor.x += stepX;
                cursor.y += stepY;
                if (std::abs(cursor.x - a.x) > 512 || std::abs(cursor.y - a.y) > 512) {
                    break;  // malformed segment guard
                }
            }
        }
    }
}

// Search window: the endpoints plus a margin, clipped to something a phone can
// solve in a frame.
struct Bounds {
    int minX = 0;
    int minY = 0;
    int width = 1;
    int height = 1;

    bool contains(int x, int y) const
    {
        return x >= minX && y >= minY && x < minX + width && y < minY + height;
    }
    int index(int x, int y) const
    {
        return (y - minY) * width + (x - minX);
    }
};

Bounds makeBounds(Point start, Point end, int margin)
{
    Bounds bounds;
    bounds.minX = std::min(start.x, end.x) - margin;
    bounds.minY = std::min(start.y, end.y) - margin;
    bounds.width = std::abs(start.x - end.x) + margin * 2 + 1;
    bounds.height = std::abs(start.y - end.y) + margin * 2 + 1;
    // Keep the search space bounded even for far-apart endpoints.
    constexpr int kMaxSide = 160;
    if (bounds.width > kMaxSide) {
        const int shrink = (bounds.width - kMaxSide) / 2;
        bounds.minX += shrink;
        bounds.width = kMaxSide;
    }
    if (bounds.height > kMaxSide) {
        const int shrink = (bounds.height - kMaxSide) / 2;
        bounds.minY += shrink;
        bounds.height = kMaxSide;
    }
    return bounds;
}

}  // namespace

std::vector<Point> connectionPoints(const Circuit& circuit)
{
    std::vector<Point> points;
    std::unordered_set<uint64_t> seen;
    for (const auto& component : circuit.components) {
        Point pins[kMaxComponentPins];
        const int count = componentPins(component, pins);
        for (int index = 0; index < count; ++index) {
            if (seen.insert(pack(pins[index])).second) {
                points.push_back(pins[index]);
            }
        }
    }
    for (const auto& wire : circuit.wires) {
        for (const auto& vertex : wire.points) {
            if (seen.insert(pack(vertex)).second) {
                points.push_back(vertex);
            }
        }
    }
    return points;
}

bool isConnectionPoint(const Circuit& circuit, Point point)
{
    const uint64_t key = pack(point);
    for (const auto& component : circuit.components) {
        Point pins[kMaxComponentPins];
        const int count = componentPins(component, pins);
        for (int index = 0; index < count; ++index) {
            if (pack(pins[index]) == key) {
                return true;
            }
        }
    }
    for (const auto& wire : circuit.wires) {
        for (const auto& vertex : wire.points) {
            if (pack(vertex) == key) {
                return true;
            }
        }
    }
    return false;
}

bool nearestConnectionPoint(const Circuit& circuit, Point point, int radius, Point* out)
{
    bool found = false;
    int bestDistance = radius + 1;
    const auto consider = [&](Point candidate) {
        const int dx = candidate.x - point.x;
        const int dy = candidate.y - point.y;
        const int distance = std::max(std::abs(dx), std::abs(dy));
        if (distance <= radius && distance < bestDistance) {
            bestDistance = distance;
            *out = candidate;
            found = true;
        }
    };
    for (const auto& component : circuit.components) {
        Point pins[kMaxComponentPins];
        const int count = componentPins(component, pins);
        for (int index = 0; index < count; ++index) {
            consider(pins[index]);
        }
    }
    for (const auto& wire : circuit.wires) {
        for (const auto& vertex : wire.points) {
            consider(vertex);
        }
    }
    return found;
}

bool nearestPinPoint(const Circuit& circuit, Point point, int radius, Point* out)
{
    if (!out) {
        return false;
    }
    bool found = false;
    int bestDistance = radius + 1;
    for (const auto& component : circuit.components) {
        Point pins[kMaxComponentPins];
        const int count = componentPins(component, pins);
        for (int index = 0; index < count; ++index) {
            const int dx = pins[index].x - point.x;
            const int dy = pins[index].y - point.y;
            const int distance = std::max(std::abs(dx), std::abs(dy));
            if (distance <= radius && distance < bestDistance) {
                bestDistance = distance;
                *out = pins[index];
                found = true;
            }
        }
    }
    return found;
}

std::vector<Point> lShapedRoute(Point start, Point end)
{
    std::vector<Point> route;
    route.push_back(start);
    if (start.x != end.x && start.y != end.y) {
        route.push_back(Point{end.x, start.y});
    }
    if (route.back() != end) {
        route.push_back(end);
    }
    return route;
}

bool routeWire(const Circuit& circuit, Point start, Point end, std::vector<Point>* route)
{
    if (!route) {
        return false;
    }
    route->clear();
    if (start == end) {
        return false;
    }
    std::vector<Point> straight;
    if (start.x == end.x || start.y == end.y) {
        straight = lShapedRoute(start, end);
    }

    Obstacles obstacles;
    collectObstacles(circuit, start, end, &obstacles);
    const uint64_t startKey = pack(start);

    // A straight unobstructed run is always the best answer.
    const auto straightUsable = [&]() {
        if (straight.empty()) {
            return false;
        }
        for (size_t index = 0; index + 1 < straight.size(); ++index) {
            Point a = straight[index];
            Point b = straight[index + 1];
            const int stepX = (b.x > a.x) - (b.x < a.x);
            const int stepY = (b.y > a.y) - (b.y < a.y);
            const bool horizontal = stepX != 0;
            Point cursor = a;
            while (true) {
                const uint64_t cursorKey = pack(cursor);
                const bool isEnd = cursor == end;
                const bool isLast = cursor == b;
                if (!isEnd &&
                    (obstacles.pins.count(cursorKey) ||
                     obstacles.wireVertices.count(cursorKey))) {
                    return false;
                }
                if (cursor == b) {
                    break;
                }
                if ((horizontal ? obstacles.horizontalEdges.count(
                                      packEdge(cursor.x, cursor.y, true))
                                : obstacles.verticalEdges.count(
                                      packEdge(cursor.x, cursor.y, false))) != 0) {
                    return false;
                }
                if (!isLast && cursor != start &&
                    obstacles.bodyPoints.count(cursorKey) != 0) {
                    return false;
                }
                cursor.x += stepX;
                cursor.y += stepY;
            }
        }
        return true;
    };
    if (straightUsable()) {
        // Still avoid running over unrelated wires when a clean detour exists.
        int crossings = 0;
        for (size_t index = 0; index + 1 < straight.size(); ++index) {
            Point a = straight[index];
            Point b = straight[index + 1];
            const int steps = std::abs(b.x - a.x) + std::abs(b.y - a.y);
            const int stepX = (b.x > a.x) - (b.x < a.x);
            const int stepY = (b.y > a.y) - (b.y < a.y);
            Point cursor = a;
            for (int step = 0; step <= steps; ++step) {
                if (cursor != start && cursor != end &&
                    obstacles.wirePoints.count(pack(cursor)) != 0) {
                    ++crossings;
                }
                cursor.x += stepX;
                cursor.y += stepY;
            }
        }
        if (crossings == 0) {
            *route = straight;
            return true;
        }
    }

    // Dijkstra over (grid point, direction) so bends can be penalised.
    const Bounds bounds = makeBounds(start, end, 6);
    if (!bounds.contains(start.x, start.y) || !bounds.contains(end.x, end.y)) {
        return false;
    }
    const int cellCount = bounds.width * bounds.height;
    const int stateCount = cellCount * 3;
    std::vector<int> best(static_cast<size_t>(stateCount),
                          std::numeric_limits<int>::max());
    std::vector<int> parent(static_cast<size_t>(stateCount), -1);
    struct Node {
        int cost;
        int state;
    };
    struct Compare {
        bool operator()(const Node& a, const Node& b) const { return a.cost > b.cost; }
    };
    std::priority_queue<Node, std::vector<Node>, Compare> queue;

    const auto stateIndex = [&](int x, int y, int direction) {
        return bounds.index(x, y) * 3 + direction;
    };
    const int startState = stateIndex(start.x, start.y, kNone);
    best[static_cast<size_t>(startState)] = 0;
    queue.push(Node{0, startState});

    while (!queue.empty()) {
        const Node node = queue.top();
        queue.pop();
        if (node.cost > best[static_cast<size_t>(node.state)]) {
            continue;
        }
        const int direction = node.state % 3;
        const int cell = node.state / 3;
        const int x = bounds.minX + cell % bounds.width;
        const int y = bounds.minY + cell / bounds.width;
        if (x == end.x && y == end.y) {
            // Walk back to the start.
            std::vector<Point> reversed;
            int state = node.state;
            while (state >= 0) {
                const int stateCell = state / 3;
                reversed.push_back(Point{bounds.minX + stateCell % bounds.width,
                                         bounds.minY + stateCell / bounds.width});
                state = parent[static_cast<size_t>(state)];
            }
            std::reverse(reversed.begin(), reversed.end());
            // Collapse the collinear runs into corners.
            std::vector<Point> path;
            for (const Point& point : reversed) {
                if (!path.empty() && path.back() == point) {
                    continue;
                }
                if (path.size() >= 2) {
                    const Point& previous = path[path.size() - 2];
                    const Point& last = path.back();
                    const bool sameRow = previous.y == last.y && last.y == point.y;
                    const bool sameColumn = previous.x == last.x && last.x == point.x;
                    if (sameRow || sameColumn) {
                        path.back() = point;
                        continue;
                    }
                }
                path.push_back(point);
            }
            if (path.size() >= 2) {
                *route = std::move(path);
                return true;
            }
            return false;
        }

        static const int kSteps[4][3] = {
            {1, 0, kHorizontal}, {-1, 0, kHorizontal}, {0, 1, kVertical}, {0, -1, kVertical},
        };
        for (const auto& step : kSteps) {
            const int nx = x + step[0];
            const int ny = y + step[1];
            if (!bounds.contains(nx, ny)) {
                continue;
            }
            const int stepDirection = step[2];
            const uint64_t edge = stepDirection == kHorizontal
                                      ? packEdge(x, y, true)
                                      : packEdge(x, y, false);
            // Running along an existing wire would merge the two nets.
            if ((stepDirection == kHorizontal ? obstacles.horizontalEdges
                                              : obstacles.verticalEdges)
                    .count(edge) != 0) {
                continue;
            }
            const uint64_t nextKey = pack(Point{nx, ny});
            const bool isEnd = (nx == end.x && ny == end.y);
            if (!isEnd) {
                // Landing on a pin or a wire endpoint would connect to it.
                if (obstacles.pins.count(nextKey) != 0 ||
                    obstacles.wireVertices.count(nextKey) != 0) {
                    continue;
                }
            }
            int cost = node.cost + Costs::kStep;
            if (direction != kNone && direction != stepDirection) {
                cost += Costs::kTurn;
            }
            if (!isEnd && obstacles.bodyPoints.count(nextKey) != 0) {
                cost += Costs::kBody;
            }
            if (!isEnd && nextKey != startKey &&
                obstacles.wirePoints.count(nextKey) != 0) {
                // Bridging another wire: legal, drawn as a jumper, but avoided.
                cost += Costs::kCrossWire;
            }
            const int nextState = stateIndex(nx, ny, stepDirection);
            if (cost < best[static_cast<size_t>(nextState)]) {
                best[static_cast<size_t>(nextState)] = cost;
                parent[static_cast<size_t>(nextState)] = node.state;
                queue.push(Node{cost, nextState});
            }
        }
    }
    return false;
}

}  // namespace circuitlab
