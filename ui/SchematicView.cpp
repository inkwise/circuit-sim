#include "App.h"

#include "Symbols.h"
#include "Theme.h"

#include "circuit/Router.h"

#include "circuit/Engineering.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace circuitlab {
namespace {

ImVec2 rotateVec(float x, float y, int rotation)
{
    switch (((rotation % 360) + 360) % 360) {
    case 90:
        return ImVec2(-y, x);
    case 180:
        return ImVec2(-x, -y);
    case 270:
        return ImVec2(y, -x);
    case 0:
    default:
        return ImVec2(x, y);
    }
}

float pointSegmentDistance(const ImVec2& p, const ImVec2& a, const ImVec2& b)
{
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float lengthSquared = dx * dx + dy * dy;
    if (lengthSquared <= 0.0001f) {
        return std::hypot(p.x - a.x, p.y - a.y);
    }
    float t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / lengthSquared;
    t = std::clamp(t, 0.0f, 1.0f);
    return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}

const ImU32 kCurrentColor = IM_COL32(94, 196, 255, 255);

// The previous animation used one dot per 54 dp of wire and capped each wire
// at 24 dots. "加两倍" raises the count threefold, with no per-wire cap.
constexpr float kElectronDensityMultiplier = 3.0f;
constexpr float kElectronSpacingDp = 54.0f / kElectronDensityMultiplier;
constexpr float kElectronMinSpeedDp = 18.0f;
constexpr float kElectronSpeedRangeDp = 110.0f;
constexpr float kElectronRadiusDp = 2.3f;
constexpr float kElectronGlowDp = 3.0f;
constexpr size_t kMaxFlowParticles = 1200;
constexpr double kFlowCurrentEpsilon = 1e-9;

}  // namespace

float App::gridPixels() const
{
    return 20.0f * uiScale_ * zoom_;
}

ImVec2 App::worldToScreen(Point point) const
{
    return ImVec2(canvasOrigin_.x + viewOffset_.x + point.x * gridPixels(),
                  canvasOrigin_.y + viewOffset_.y + point.y * gridPixels());
}

ImVec2 App::screenToWorld(ImVec2 screen) const
{
    const float gp = gridPixels();
    if (gp <= 0.0001f) {
        return ImVec2(0.0f, 0.0f);
    }
    return ImVec2((screen.x - canvasOrigin_.x - viewOffset_.x) / gp,
                  (screen.y - canvasOrigin_.y - viewOffset_.y) / gp);
}

ImVec2 App::componentPoint(const Component& component, float localX, float localY) const
{
    // Mirror first, then rotate: the same order the pins use, so a flipped
    // symbol's leads still end exactly on its pins.
    const ImVec2 rotated =
        rotateVec(component.mirrored ? -localX : localX, localY, component.rotation);
    return ImVec2(canvasOrigin_.x + viewOffset_.x +
                      (component.gridX + rotated.x) * gridPixels(),
                  canvasOrigin_.y + viewOffset_.y +
                      (component.gridY + rotated.y) * gridPixels());
}

Point App::snapScreen(ImVec2 screen) const
{
    const ImVec2 world = screenToWorld(screen);
    return snapPoint(world.x, world.y);
}

void App::fitView()
{
    fitPending_ = false;
    if (canvasSize_.x < 10.0f || canvasSize_.y < 10.0f) {
        return;
    }
    if (circuit_.components.empty() && circuit_.wires.empty()) {
        zoom_ = 1.0f;
        viewOffset_ = ImVec2(canvasSize_.x * 0.5f, canvasSize_.y * 0.5f);
        return;
    }

    int minX = 0;
    int minY = 0;
    int maxX = 0;
    int maxY = 0;
    bool first = true;
    const auto include = [&](int x, int y) {
        if (first) {
            minX = maxX = x;
            minY = maxY = y;
            first = false;
        } else {
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }
    };
    for (const auto& component : circuit_.components) {
        Point boundsMin;
        Point boundsMax;
        componentBounds(component, &boundsMin, &boundsMax);
        include(boundsMin.x, boundsMin.y);
        include(boundsMax.x, boundsMax.y);
    }
    for (const auto& wire : circuit_.wires) {
        for (const auto& point : wire.points) {
            include(point.x, point.y);
        }
    }
    minX -= 1;
    minY -= 1;
    maxX += 1;
    maxY += 1;

    const float base = 20.0f * uiScale_;
    const float worldWidth = static_cast<float>(maxX - minX);
    const float worldHeight = static_cast<float>(maxY - minY);
    const float marginX = 24.0f * uiScale_;
    const float marginTop = 16.0f * uiScale_;
    // Component labels and the current readout sit below the symbol bounds.
    const float marginBottom = 40.0f * uiScale_;
    const float zoomX =
        (canvasSize_.x - marginX * 2.0f) / std::max(1.0f, worldWidth * base);
    const float zoomY =
        (canvasSize_.y - marginTop - marginBottom) /
        std::max(1.0f, worldHeight * base);
    zoom_ = std::clamp(std::min(zoomX, zoomY), 0.28f, 2.8f);

    const float gp = gridPixels();
    const float centerX = (minX + maxX) * 0.5f;
    const float centerY = (minY + maxY) * 0.5f;
    const float contentCenterY =
        (marginTop + (canvasSize_.y - marginBottom)) * 0.5f;
    viewOffset_ = ImVec2(canvasSize_.x * 0.5f - centerX * gp,
                         contentCenterY - centerY * gp);
}

namespace {

// Screen-space rectangle of the symbol that is actually drawn: the body (at the
// scale the drawing uses) plus the pin ends. Nothing else - no grid margins, no
// labels - so a tap on the symbol is a hit and a tap next to it is not.
float distanceToRect(const SymbolRect& rect, ImVec2 point)
{
    const float dx = std::max({rect.min.x - point.x, 0.0f, point.x - rect.max.x});
    const float dy = std::max({rect.min.y - point.y, 0.0f, point.y - rect.max.y});
    return std::hypot(dx, dy);
}

bool rectContains(const SymbolRect& rect, ImVec2 point)
{
    return point.x >= rect.min.x && point.x <= rect.max.x && point.y >= rect.min.y &&
           point.y <= rect.max.y;
}

}  // namespace

SymbolRect App::symbolRect(const Component& component) const
{
    SymbolRect rect;
    float halfWidth = 0.0f;
    float halfHeight = 0.0f;
    componentSymbolHalfExtents(component, &halfWidth, &halfHeight);
    rect.min = worldToScreen(Point{component.gridX, component.gridY});
    rect.max = rect.min;
    // worldToScreen only takes grid points, so build the corners by hand: the
    // symbol centre plus the drawn half extents in pixels.
    const ImVec2 centre = worldToScreen(Point{component.gridX, component.gridY});
    const float grid = gridPixels();
    rect.min = ImVec2(centre.x - halfWidth * grid, centre.y - halfHeight * grid);
    rect.max = ImVec2(centre.x + halfWidth * grid, centre.y + halfHeight * grid);
    Point pins[kMaxComponentPins];
    const int count = componentPins(component, pins);
    for (int index = 0; index < count; ++index) {
        const ImVec2 pin = worldToScreen(pins[index]);
        rect.min.x = std::min(rect.min.x, pin.x);
        rect.min.y = std::min(rect.min.y, pin.y);
        rect.max.x = std::max(rect.max.x, pin.x);
        rect.max.y = std::max(rect.max.y, pin.y);
    }
    rect.valid = true;
    return rect;
}

void App::hitSelection(ImVec2 screen, int* componentOut, int* wireOut) const
{
    if (componentOut) {
        *componentOut = -1;
    }
    if (wireOut) {
        *wireOut = -1;
    }

    // 1. A tap inside a symbol selects the smallest such symbol.
    int insideComponent = -1;
    float insideArea = 0.0f;
    // 2. Otherwise the closest symbol and wire compete; the closer one wins.
    int nearComponent = -1;
    float nearComponentDistance = 1e9f;
    int nearWire = -1;
    float nearWireDistance = 1e9f;

    const float componentGrace = dp(5.0f);
    const float wireGrace = dp(9.0f);

    for (const auto& component : circuit_.components) {
        const SymbolRect rect = symbolRect(component);
        if (!rect.valid) {
            continue;
        }
        if (rectContains(rect, screen)) {
            const float area = (rect.max.x - rect.min.x) * (rect.max.y - rect.min.y);
            if (insideComponent < 0 || area < insideArea) {
                insideComponent = component.id;
                insideArea = area;
            }
            continue;
        }
        const float distance = distanceToRect(rect, screen);
        if (distance <= componentGrace && distance < nearComponentDistance) {
            nearComponentDistance = distance;
            nearComponent = component.id;
        }
    }
    for (const auto& wire : circuit_.wires) {
        for (size_t index = 0; index + 1 < wire.points.size(); ++index) {
            const float distance =
                pointSegmentDistance(screen, worldToScreen(wire.points[index]),
                                     worldToScreen(wire.points[index + 1]));
            if (distance <= wireGrace && distance < nearWireDistance) {
                nearWireDistance = distance;
                nearWire = wire.id;
            }
        }
    }

    if (insideComponent >= 0) {
        if (componentOut) {
            *componentOut = insideComponent;
        }
        return;
    }
    // Nothing was hit directly: a wire under the finger beats a symbol that is
    // merely nearby, otherwise thin parts would keep stealing wire taps.
    if (nearWire >= 0 && nearWireDistance <= nearComponentDistance) {
        if (wireOut) {
            *wireOut = nearWire;
        }
        return;
    }
    if (nearComponent >= 0) {
        if (componentOut) {
            *componentOut = nearComponent;
        }
    }
}

bool App::hoveredConnectionTarget(ImVec2 screen, Point* point) const
{
    if (!point) {
        return false;
    }
    const float grid = gridPixels();
    if (!(grid > 0.0f)) {
        return false;
    }
    // The snap radius is a couple of grid cells, never less than one: pins sit
    // one cell from a compact symbol's centre.
    const int radius =
        std::clamp(static_cast<int>(std::lround(26.0f * uiScale_ / grid)), 1, 3);
    return nearestConnectionPoint(circuit_, snapScreen(screen), radius, point);
}

bool App::hoveredPinTarget(ImVec2 screen, Point* point) const
{
    if (!point) {
        return false;
    }
    const float grid = gridPixels();
    if (!(grid > 0.0f)) {
        return false;
    }
    const int radius =
        std::clamp(static_cast<int>(std::lround(26.0f * uiScale_ / grid)), 1, 3);
    return nearestPinPoint(circuit_, snapScreen(screen), radius, point);
}

Point App::wireEndPoint(ImVec2 screen) const
{
    // A wire may end on a pin, on an existing joint, or in blank space (so a
    // stub or a rail can be drawn where it is needed).
    Point target;
    if (hoveredConnectionTarget(screen, &target)) {
        return target;
    }
    return snapScreen(screen);
}

const std::vector<Point>& App::previewRoute(Point start, Point end)
{
    if (wirePreviewStart_ == start && wirePreviewEnd_ == end &&
        wirePreviewRevision_ == circuit_.revision) {
        return wirePreview_;
    }
    wirePreviewStart_ = start;
    wirePreviewEnd_ = end;
    wirePreviewRevision_ = circuit_.revision;
    wirePreview_.clear();
    if (start != end && !routeWire(circuit_, start, end, &wirePreview_)) {
        wirePreview_ = lShapedRoute(start, end);
    }
    return wirePreview_;
}

void App::drawGrid(ImDrawList* drawList, const ImVec2& min, const ImVec2& max)
{
    drawList->AddRectFilled(min, max, Theme::canvas());
    const ImVec2 worldMin = screenToWorld(min);
    const ImVec2 worldMax = screenToWorld(max);
    const float gp = gridPixels();

    int step = 1;
    while (gp * static_cast<float>(step) < 13.0f * uiScale_) {
        step *= 2;
    }
    const int x0 = static_cast<int>(std::floor(worldMin.x)) - 1;
    const int x1 = static_cast<int>(std::ceil(worldMax.x)) + 1;
    const int y0 = static_cast<int>(std::floor(worldMin.y)) - 1;
    const int y1 = static_cast<int>(std::ceil(worldMax.y)) + 1;

    for (int x = x0 - (x0 % step); x <= x1; x += step) {
        const float sx = worldToScreen(Point{x, 0}).x;
        const bool major = (x / step) % 5 == 0;
        drawList->AddLine(ImVec2(sx, min.y), ImVec2(sx, max.y),
                          major ? IM_COL32(44, 56, 70, 255) : Theme::grid(),
                          major ? 1.5f * uiScale_ : 1.0f * uiScale_);
    }
    for (int y = y0 - (y0 % step); y <= y1; y += step) {
        const float sy = worldToScreen(Point{0, y}).y;
        const bool major = (y / step) % 5 == 0;
        drawList->AddLine(ImVec2(min.x, sy), ImVec2(max.x, sy),
                          major ? IM_COL32(44, 56, 70, 255) : Theme::grid(),
                          major ? 1.5f * uiScale_ : 1.0f * uiScale_);
    }
}

void App::drawWire(ImDrawList* drawList, const Wire& wire, bool selected)
{
    const ImU32 color = selected ? Theme::accent() : Theme::wire();
    const float thickness = selected ? 3.4f * uiScale_ : 2.0f * uiScale_;
    const float bridgeRadius = bridgeRadiusPixels();

    // Crossings where this wire is the one added later: draw a bridge there so
    // the two lines look separated instead of joined.
    std::vector<Point> hops;
    for (const WireCrossing& crossing : wireCrossings_) {
        if (crossing.hoppingWireId == wire.id) {
            hops.push_back(crossing.point);
        }
    }

    for (size_t i = 0; i + 1 < wire.points.size(); ++i) {
        const ImVec2 a = worldToScreen(wire.points[i]);
        const ImVec2 b = worldToScreen(wire.points[i + 1]);
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length < 0.5f) {
            continue;
        }
        const ImVec2 direction(dx / length, dy / length);
        const auto pointAt = [&](float t) { return ImVec2(a.x + dx * t, a.y + dy * t); };

        // Bridge positions on this segment, ordered along it.
        std::vector<float> stops;
        for (const Point& hop : hops) {
            const ImVec2 point = worldToScreen(hop);
            const float t = ((point.x - a.x) * direction.x + (point.y - a.y) * direction.y) /
                            length;
            if (t <= 0.0f || t >= 1.0f) {
                continue;
            }
            const ImVec2 projected = pointAt(t);
            if (std::fabs(projected.x - point.x) > dp(1.5f) ||
                std::fabs(projected.y - point.y) > dp(1.5f)) {
                continue;
            }
            stops.push_back(t);
        }
        std::sort(stops.begin(), stops.end());

        float cursor = 0.0f;
        for (float t : stops) {
            const float gap = std::min(bridgeRadius / length, std::max(0.0f, t - cursor));
            const float from = cursor;
            const float to = t - gap;
            if (to > from) {
                drawList->AddLine(pointAt(from), pointAt(to), color, thickness);
            }
            const ImVec2 center = pointAt(t);
            // Horizontal runs bulge upwards, vertical runs bulge to the right.
            const ImVec2 outward = std::fabs(direction.x) >= std::fabs(direction.y)
                                       ? ImVec2(0.0f, -1.0f)
                                       : ImVec2(1.0f, 0.0f);
            const float start = std::atan2(-direction.y, -direction.x);
            float apex = std::atan2(outward.y, outward.x) - start;
            while (apex <= -3.14159265f) {
                apex += 6.2831853f;
            }
            while (apex > 3.14159265f) {
                apex -= 6.2831853f;
            }
            const float end = start + (apex >= 0.0f ? 3.14159265f : -3.14159265f);
            drawList->PathArcTo(center, bridgeRadius, start, end, 0);
            drawList->PathStroke(color, 0, thickness);
            cursor = std::min(1.0f, t + bridgeRadius / length);
        }
        if (cursor < 1.0f) {
            drawList->AddLine(pointAt(cursor), pointAt(1.0f), color, thickness);
        }
    }

    // Junction studs: small enough to read as copper, not as a component pin.
    for (const auto& point : wire.points) {
        drawList->AddCircleFilled(worldToScreen(point), 2.6f * uiScale_, color);
    }
}

float App::bridgeRadiusPixels() const
{
    return std::max(dp(4.5f), gridPixels() * 0.38f);
}

void App::rebuildFlowNetwork()
{
    flowNetwork_ = buildFlowNetwork(circuit_, compiled_);
    flowParticles_.clear();
    flowReady_ = flowNetwork_.ok;
    flowRevision_ = circuit_.revision;
    flowEdgeHops_.clear();

    // Attach every jumper bridge to the flow edge that carries it, so the
    // electrons climb over the arc instead of running straight through.
    for (const WireCrossing& crossing : wireCrossings_) {
        for (int index = 0; index < static_cast<int>(flowNetwork_.edges.size()); ++index) {
            const FlowEdge& edge = flowNetwork_.edges[static_cast<size_t>(index)];
            if (edge.isDevice() || edge.wireId != crossing.hoppingWireId) {
                continue;
            }
            const Point a = edge.pointA;
            const Point b = edge.pointB;
            const long long cross =
                static_cast<long long>(crossing.point.x - a.x) * (b.y - a.y) -
                static_cast<long long>(crossing.point.y - a.y) * (b.x - a.x);
            if (cross != 0) {
                continue;
            }
            const long long lengthSquared = static_cast<long long>(b.x - a.x) * (b.x - a.x) +
                                            static_cast<long long>(b.y - a.y) * (b.y - a.y);
            if (lengthSquared <= 0) {
                continue;
            }
            const long long dot =
                static_cast<long long>(crossing.point.x - a.x) * (b.x - a.x) +
                static_cast<long long>(crossing.point.y - a.y) * (b.y - a.y);
            const float t = static_cast<float>(static_cast<double>(dot) /
                                              static_cast<double>(lengthSquared));
            if (t <= 0.0f || t >= 1.0f) {
                continue;   // a junction, not a bridge
            }
            FlowHop hop;
            hop.t = t;
            hop.outward = a.y == b.y ? ImVec2(0.0f, -1.0f) : ImVec2(1.0f, 0.0f);
            flowEdgeHops_[index].push_back(hop);
        }
    }
}

void App::updateFlowCurrents(size_t sample)
{
    std::map<int, std::vector<double>> pinCurrents;
    for (const Component& component : circuit_.components) {
        if (component.type == ComponentType::Ground) {
            continue;
        }
        const int pins = pinCount(component.type);
        std::vector<double> values(static_cast<size_t>(pins), 0.0);
        bool any = false;
        for (int pin = 0; pin < pins; ++pin) {
            double current = 0.0;
            if (currentForPin(component, pin, sample, &current)) {
                values[static_cast<size_t>(pin)] = current;
                any = true;
            }
        }
        if (any) {
            pinCurrents[component.id] = std::move(values);
        }
    }
    solveFlowCurrents(flowNetwork_, pinCurrents);
}

float App::flowEdgePixels(int edgeIndex) const
{
    if (edgeIndex < 0 ||
        edgeIndex >= static_cast<int>(flowNetwork_.edges.size())) {
        return 0.0f;
    }
    const FlowEdge& edge = flowNetwork_.edges[static_cast<size_t>(edgeIndex)];
    const ImVec2 a = worldToScreen(edge.pointA);
    const ImVec2 b = worldToScreen(edge.pointB);
    return std::hypot(b.x - a.x, b.y - a.y);
}

ImVec2 App::flowEdgePoint(int edgeIndex, float t) const
{
    const FlowEdge& edge = flowNetwork_.edges[static_cast<size_t>(edgeIndex)];
    const ImVec2 a = worldToScreen(edge.pointA);
    const ImVec2 b = worldToScreen(edge.pointB);
    ImVec2 point(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);

    const auto found = flowEdgeHops_.find(edgeIndex);
    if (found == flowEdgeHops_.end()) {
        return point;
    }
    const float length = std::hypot(b.x - a.x, b.y - a.y);
    if (length <= 0.0f) {
        return point;
    }
    const float radius = bridgeRadiusPixels();
    for (const FlowHop& hop : found->second) {
        // Distance of this electron from the middle of the bridge, in pixels.
        const float along = (t - hop.t) * length;
        if (std::fabs(along) >= radius) {
            continue;
        }
        // Semicircle: the stream arcs over the crossing exactly like the wire.
        const float offset = std::sqrt(std::max(0.0f, radius * radius - along * along));
        point.x += hop.outward.x * offset;
        point.y += hop.outward.y * offset;
    }
    return point;
}

void App::drawWireFlow(ImDrawList* drawList)
{
    if (!live_ || !hasResult_ || !result_.ok || simRunning_ ||
        resultRevision_ != circuit_.revision) {
        return;
    }
    if (!compiled_.ok) {
        return;
    }
    if (!flowReady_ || flowRevision_ != circuit_.revision) {
        rebuildFlowNetwork();
    }
    if (!flowReady_ || flowNetwork_.edges.empty()) {
        return;
    }

    updateFlowCurrents(liveSampleIndex());
    std::vector<FlowEdge>& edges = flowNetwork_.edges;
    const float deltaTime = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.05f);
    std::vector<size_t> dead;

    // Move every electron along its edge. At a junction it continues onto
    // whichever outgoing edge carries current, weighted by the current split,
    // so a stream runs through the whole route instead of restarting per wire.
    for (size_t index = 0; index < flowParticles_.size(); ++index) {
        FlowParticle& particle = flowParticles_[index];
        if (particle.edge < 0 ||
            particle.edge >= static_cast<int>(edges.size())) {
            dead.push_back(index);
            continue;
        }
        const double current = edges[static_cast<size_t>(particle.edge)].current;
        if (std::fabs(current) < kFlowCurrentEpsilon) {
            // An AC zero crossing briefly stops the electrons in place.
            continue;
        }
        const float pixels = flowEdgePixels(particle.edge);
        if (pixels <= 1.0f) {
            dead.push_back(index);
            continue;
        }
        const double magnitude = std::fabs(current);
        const double normalized =
            std::clamp(std::log10(1.0 + magnitude / 1e-6) / 6.0, 0.0, 1.0);
        const float speed = dp(kElectronMinSpeedDp) +
                            dp(kElectronSpeedRangeDp) *
                                static_cast<float>(normalized);
        particle.t +=
            (current >= 0.0 ? 1.0f : -1.0f) * speed * deltaTime / pixels;

        int guard = 0;
        while ((particle.t > 1.0f || particle.t < 0.0f) && guard++ < 8) {
            const bool atEnd = particle.t > 1.0f;
            const FlowEdge& edge = edges[static_cast<size_t>(particle.edge)];
            const float length = flowEdgePixels(particle.edge);
            const float overshoot =
                (atEnd ? particle.t - 1.0f : -particle.t) * length;
            const int vertex = atEnd ? edge.vertexB : edge.vertexA;
            const int next =
                flowChooseOutgoing(flowNetwork_, vertex, particle.edge,
                                   &flowRandom_);
            if (next < 0) {
                particle.edge = -1;
                break;
            }
            const float nextLength = flowEdgePixels(next);
            if (!(nextLength > 1.0f)) {
                particle.edge = -1;
                break;
            }
            const FlowEdge& nextEdge = edges[static_cast<size_t>(next)];
            particle.edge = next;
            if (nextEdge.vertexB == vertex && nextEdge.vertexA != vertex) {
                particle.t = 1.0f - overshoot / nextLength;
            } else {
                particle.t = overshoot / nextLength;
            }
        }
        if (particle.edge < 0) {
            dead.push_back(index);
            continue;
        }
        particle.t = std::clamp(particle.t, 0.0f, 1.0f);
    }

    if (!dead.empty()) {
        size_t write = 0;
        size_t nextDead = 0;
        for (size_t read = 0; read < flowParticles_.size(); ++read) {
            if (nextDead < dead.size() && dead[nextDead] == read) {
                ++nextDead;
                continue;
            }
            if (write != read) {
                flowParticles_[write] = flowParticles_[read];
            }
            ++write;
        }
        flowParticles_.resize(write);
    }

    std::vector<int> perEdge(edges.size(), 0);
    for (const FlowParticle& particle : flowParticles_) {
        if (particle.edge >= 0 &&
            particle.edge < static_cast<int>(edges.size())) {
            ++perEdge[static_cast<size_t>(particle.edge)];
        }
    }

    // Keep a steady density on every current-carrying wire piece. Electrons
    // that vanish into a device or an unbalanced end are replenished here.
    const float spacing = std::max(1.0f, dp(kElectronSpacingDp));
    for (int index = 0; index < static_cast<int>(edges.size()); ++index) {
        const FlowEdge& edge = edges[static_cast<size_t>(index)];
        if (edge.isDevice() ||
            std::fabs(edge.current) < kFlowCurrentEpsilon) {
            continue;
        }
        const float pixels = flowEdgePixels(index);
        if (pixels < 2.0f) {
            continue;
        }
        const int desired = std::clamp(
            static_cast<int>(std::lround(pixels / spacing)), 1, 240);
        int count = perEdge[static_cast<size_t>(index)];

        // Merging branches can double up the dots on one wire; thin the
        // excess so the stream keeps an even density.
        const int maximum = desired + std::max(2, desired / 3);
        if (count > maximum) {
            int excess = count - desired;
            for (size_t i = flowParticles_.size(); i-- > 0 && excess > 0;) {
                if (flowParticles_[i].edge == index) {
                    flowParticles_.erase(flowParticles_.begin() +
                                         static_cast<std::ptrdiff_t>(i));
                    --excess;
                    --count;
                }
            }
            perEdge[static_cast<size_t>(index)] = count;
        }

        if (count >= desired || flowParticles_.size() >= kMaxFlowParticles) {
            continue;
        }
        std::vector<float> positions;
        for (const FlowParticle& particle : flowParticles_) {
            if (particle.edge == index) {
                positions.push_back(particle.t);
            }
        }
        std::sort(positions.begin(), positions.end());
        while (count < desired && flowParticles_.size() < kMaxFlowParticles) {
            float bestGap = -1.0f;
            float gapStart = 0.0f;
            float gapEnd = 1.0f;
            float previous = 0.0f;
            for (float position : positions) {
                if (position - previous > bestGap) {
                    bestGap = position - previous;
                    gapStart = previous;
                    gapEnd = position;
                }
                previous = position;
            }
            if (1.0f - previous > bestGap) {
                gapStart = previous;
                gapEnd = 1.0f;
            }
            const float t = 0.5f * (gapStart + gapEnd);
            flowParticles_.push_back(FlowParticle{index, t});
            positions.push_back(t);
            std::sort(positions.begin(), positions.end());
            ++count;
        }
    }

    for (const FlowParticle& particle : flowParticles_) {
        if (particle.edge < 0 ||
            particle.edge >= static_cast<int>(edges.size())) {
            continue;
        }
        const FlowEdge& edge = edges[static_cast<size_t>(particle.edge)];
        if (edge.isDevice() ||
            std::fabs(edge.current) < kFlowCurrentEpsilon) {
            continue;
        }
        const ImVec2 point = flowEdgePoint(particle.edge, particle.t);
        const float radius = dp(kElectronRadiusDp);
        drawList->AddCircleFilled(point, radius + dp(kElectronGlowDp),
                                  IM_COL32(94, 196, 255, 32));
        drawList->AddCircleFilled(point, radius, IM_COL32(174, 230, 255, 230));
    }
}

void App::drawComponent(ImDrawList* drawList, const Component& component, bool selected)
{
    // Simple two- and three-pin parts are drawn at half scale and only occupy
    // two grid cells, so a circuit needs far less canvas. Their symbols are
    // written in the old, larger coordinates and scaled here; multi-pin chips
    // keep full size because their pins need the room.
    const float symbolScale =
        component.legacySymbol ? 1.0f : componentSymbolScale(component.type);
    const float t = 2.6f * uiScale_ * std::max(0.78f, symbolScale);
    const bool damagedLoad = component.damaged && isLoadType(component.type);
    const ImU32 body =
        selected ? Theme::accent() : (damagedLoad ? Theme::danger() : Theme::text());
    const ImU32 detail = selected ? Theme::accent()
                                  : (damagedLoad ? Theme::danger() : Theme::textDim());
    const float gp = gridPixels();
    const float gps = gp * symbolScale;
    const auto P = [&](float x, float y) {
        return componentPoint(component, x * symbolScale, y * symbolScale);
    };
    const ImVec2 center = P(0.0f, 0.0f);

    const size_t sampleIndex = liveSampleIndex();
    double current = 0.0;
    const bool haveCurrent = currentFor(component, sampleIndex, &current);
    const bool flowing = haveCurrent && std::fabs(current) >= 1e-9;
    const double intensity =
        flowing && !damagedLoad ? loadIntensity(component, sampleIndex) : 0.0;

    std::vector<ImVec2> polyline;
    switch (component.type) {
    case ComponentType::Resistor:
    case ComponentType::PowerResistor:
        if (component.type == ComponentType::PowerResistor) {
            // Cement / wirewound block: a boxed resistor instead of a zigzag.
            drawList->AddLine(P(0, -2), P(0, -1.2f), body, t);
            drawList->AddLine(P(0, 1.2f), P(0, 2), body, t);
            drawList->AddRect(P(-0.6f, -1.2f), P(0.6f, 1.2f), body, dp(1.5f), 0, t);
            drawList->AddLine(P(-0.35f, -0.6f), P(0.35f, -0.6f), detail,
                              1.4f * uiScale_);
            drawList->AddLine(P(-0.35f, 0.0f), P(0.35f, 0.0f), detail,
                              1.4f * uiScale_);
            break;
        }
        polyline = {
            P(0, -2),     P(0, -1.2f), P(0.45f, -1.0f), P(-0.45f, -0.6f),
            P(0.45f, -0.2f), P(-0.45f, 0.2f), P(0.45f, 0.6f),
            P(-0.45f, 1.0f), P(0, 1.2f), P(0, 2),
        };
        drawList->AddPolyline(polyline.data(), static_cast<int>(polyline.size()), body, 0, t);
        break;
    case ComponentType::Capacitor:
    case ComponentType::CeramicCap:
        drawList->AddLine(P(0, -2), P(0, -0.3f), body, t);
        drawList->AddLine(P(0, 0.3f), P(0, 2), body, t);
        drawList->AddLine(P(-0.95f, -0.3f), P(0.95f, -0.3f), body, t);
        drawList->AddLine(P(-0.95f, 0.3f), P(0.95f, 0.3f), body, t);
        if (component.type == ComponentType::CeramicCap) {
            // Ceramic chip: both plates straight, no polarity mark.
            drawList->AddLine(P(-0.55f, 0.55f), P(0.55f, 0.55f), detail,
                              1.4f * uiScale_);
        }
        break;
    case ComponentType::Inductor: {
        drawList->AddLine(P(0, -2), P(0, -1.2f), body, t);
        drawList->AddLine(P(0, 1.2f), P(0, 2), body, t);
        polyline.clear();
        const int segments = 36;
        for (int i = 0; i <= segments; ++i) {
            const float u = static_cast<float>(i) / segments;
            const float y = -1.2f + 2.4f * u;
            const float x = 0.38f * std::sin(u * 6.0f * 3.14159265f);
            polyline.push_back(P(x, y));
        }
        drawList->AddPolyline(polyline.data(), static_cast<int>(polyline.size()), body, 0, t);
        break;
    }
    case ComponentType::VoltageSource:
    case ComponentType::AcSource:
    case ComponentType::SignalSource: {
        drawList->AddLine(P(0, -2), P(0, -0.85f), body, t);
        drawList->AddLine(P(0, 0.85f), P(0, 2), body, t);
        drawList->AddCircle(center, 0.85f * gps, body, 0, t);

        if (component.source.mode == Waveform::DC) {
            drawList->AddLine(P(-0.16f, -0.35f), P(0.16f, -0.35f), body, t);
            drawList->AddLine(P(0, -0.51f), P(0, -0.19f), body, t);
            drawList->AddLine(P(-0.16f, 0.42f), P(0.16f, 0.42f), body, t);
        } else if (component.source.mode == Waveform::Pulse) {
            polyline = {
                P(-0.52f, 0.25f), P(-0.3f, 0.25f), P(-0.3f, -0.25f),
                P(0.0f, -0.25f),  P(0.0f, 0.25f),  P(0.3f, 0.25f),
                P(0.3f, -0.25f),  P(0.52f, -0.25f),
            };
            drawList->AddPolyline(polyline.data(), static_cast<int>(polyline.size()),
                                  detail, 0, 1.6f * uiScale_);
        } else {
            polyline.clear();
            for (int i = 0; i <= 20; ++i) {
                const float u = static_cast<float>(i) / 20.0f;
                polyline.push_back(P(-0.52f + 1.04f * u,
                                     0.28f * std::sin(u * 6.2831853f)));
            }
            drawList->AddPolyline(polyline.data(), static_cast<int>(polyline.size()),
                                  detail, 0, 1.6f * uiScale_);
        }
        break;
    }
    case ComponentType::Ground:
        drawList->AddLine(P(0, -1), P(0, 0.05f), body, t);
        drawList->AddLine(P(-0.95f, 0.05f), P(0.95f, 0.05f), body, t);
        drawList->AddLine(P(-0.62f, 0.3f), P(0.62f, 0.3f), body, t);
        drawList->AddLine(P(-0.3f, 0.55f), P(0.3f, 0.55f), body, t);
        break;
    case ComponentType::Switch:
        drawList->AddLine(P(0, -2), P(0, -0.62f), body, t);
        drawList->AddLine(P(0, 0.62f), P(0, 2), body, t);
        drawList->AddCircleFilled(P(0, -0.62f), 0.11f * gps, body);
        drawList->AddCircleFilled(P(0, 0.62f), 0.11f * gps, body);
        if (component.switchClosed) {
            drawList->AddLine(P(0, -0.62f), P(0, 0.62f), body, t);
        } else {
            drawList->AddLine(P(0, -0.62f), P(0.78f, -0.12f), body, t);
        }
        break;
    case ComponentType::Bulb: {
        // A lamp drawn as a lamp: glass envelope with a filament, screw base.
        const ImVec2 glass = P(0.0f, -0.42f);
        if (intensity > 0.01) {
            const float glow = static_cast<float>(std::min(1.0, intensity));
            // Inner light under the symbol so the glass reads as "lit inside".
            drawList->AddCircleFilled(glass, 0.95f * gps,
                                      IM_COL32(255, 226, 150,
                                               static_cast<int>(30 + glow * 110)));
            // Wide three-layer halo, drawn in the deferred pass so the light
            // lands on top of the wires and of neighbouring parts. It has to be
            // obvious from a zoomed-out view which lamp is lit.
            const struct {
                float radius;   // grid units
                int red, green, blue;
                float base, span;
            } kHalo[] = {
                {3.10f, 255, 186, 64, 24.0f, 44.0f},
                {2.05f, 255, 202, 96, 24.0f, 52.0f},
                {1.30f, 255, 224, 146, 20.0f, 58.0f},
            };
            for (const auto& layer : kHalo) {
                Glow halo;
                halo.center = glass;
                halo.radius = layer.radius * gps;
                halo.core = false;
                halo.color = IM_COL32(layer.red, layer.green, layer.blue,
                                      static_cast<int>(layer.base + glow * layer.span));
                pendingGlows_.push_back(halo);
            }
        }
        drawList->AddLine(P(0, -2), P(0, -1.22f), body, t);
        drawList->AddLine(P(0, 0.4f), P(0, 0.72f), body, t);
        drawList->AddCircle(glass, 0.82f * gps, body, 0, t);
        polyline = {
            P(-0.42f, -0.42f), P(-0.18f, -1.04f), P(0.02f, -0.56f),
            P(0.22f, -1.02f),  P(0.42f, -0.42f),
        };
        drawList->AddPolyline(polyline.data(), static_cast<int>(polyline.size()), detail, 0,
                              1.8f * uiScale_);
        drawList->AddLine(P(-0.44f, 0.72f), P(0.44f, 0.72f), body, t);
        drawList->AddLine(P(-0.34f, 0.96f), P(0.34f, 0.96f), body, t);
        drawList->AddLine(P(-0.24f, 1.2f), P(0.24f, 1.2f), body, t);
        break;
    }
    case ComponentType::Fan: {
        drawList->AddLine(P(0, -2), P(0, -0.95f), body, t);
        drawList->AddLine(P(0, 0.95f), P(0, 2), body, t);
        drawList->AddCircle(center, 0.95f * gps, body, 0, t);
        float spin = 0.0f;
        if (flowing && !damagedLoad && intensity > 0.001) {
            spin = static_cast<float>(liveClock_ * 6.0 *
                                      (current >= 0.0 ? 1.0 : -1.0)) *
                   static_cast<float>(intensity);
        }
        const float base = static_cast<float>(component.rotation) * 0.0174532925f;
        for (int blade = 0; blade < 3; ++blade) {
            const float angle = base + spin + static_cast<float>(blade) * 2.0943951f;
            const ImVec2 tip(center.x + std::cos(angle) * 0.78f * gps,
                             center.y + std::sin(angle) * 0.78f * gps);
            const ImVec2 side(center.x + std::cos(angle + 0.55f) * 0.45f * gps,
                              center.y + std::sin(angle + 0.55f) * 0.45f * gps);
            drawList->AddTriangleFilled(center, tip, side, detail);
            drawList->AddLine(center, tip, body, 1.2f * uiScale_);
        }
        drawList->AddCircleFilled(center, 0.14f * gps, body);
        break;
    }
    case ComponentType::Tec12706:
    case ComponentType::Tec12710: {
        // A Peltier plate: two ceramic faces with the couples between them.
        // The face being cooled is tinted ice blue and the one dumping heat
        // warm orange, so reversing the supply is obvious at a glance.
        const bool pumpingHeat = flowing && !damagedLoad && std::fabs(current) > 1e-4;
        const bool cooling = pumpingHeat && current > 0.0;
        const ImU32 coldFace = cooling ? IM_COL32(130, 205, 255, 255) : body;
        const ImU32 hotFace = cooling ? IM_COL32(255, 150, 110, 255) : body;
        drawList->AddLine(P(0, -2), P(0, -1.15f), body, t);
        drawList->AddLine(P(0, 1.15f), P(0, 2), body, t);
        drawList->AddLine(P(-0.95f, -1.15f), P(0.95f, -1.15f), coldFace, 1.25f * t);
        drawList->AddLine(P(-0.95f, 1.15f), P(0.95f, 1.15f), hotFace, 1.25f * t);
        for (int couple = 0; couple < 3; ++couple) {
            const float x = -0.52f + 0.52f * static_cast<float>(couple);
            drawList->AddLine(P(x, -1.15f), P(x, 1.15f), detail, 1.5f * uiScale_);
        }
        // Heat is pumped from the cold face towards the hot one: one arrow per
        // pellet, flipping when the current is reversed.
        if (pumpingHeat) {
            const float direction = current > 0.0 ? 1.0f : -1.0f;
            const ImU32 arrowColor =
                current > 0.0 ? IM_COL32(130, 205, 255, 255) : IM_COL32(255, 150, 110, 255);
            for (int couple = 0; couple < 3; ++couple) {
                const float x = -0.52f + 0.52f * static_cast<float>(couple);
                const float y0 = -0.5f * direction;
                const float y1 = 0.5f * direction;
                drawList->AddLine(P(x, y0), P(x, y1), arrowColor, 1.8f * uiScale_);
                drawList->AddTriangleFilled(P(x - 0.13f, y1 - 0.18f * direction),
                                            P(x + 0.13f, y1 - 0.18f * direction),
                                            P(x, y1), arrowColor);
            }
        }
        break;
    }
    case ComponentType::Diode:
    case ComponentType::Led:
    case ComponentType::Zener: {
        if (component.type == ComponentType::Led && flowing &&
            std::fabs(current) > 1e-5) {
            const float level =
                static_cast<float>(std::min(1.0, std::fabs(current) / 0.02));
            drawList->AddCircleFilled(center, 0.9f * gps,
                                      IM_COL32(255, 160, 100,
                                               static_cast<int>(25 + level * 100)));
            const struct {
                float radius;
                int red, green, blue;
                float base, span;
            } kLedHalo[] = {
                {2.4f, 255, 92, 58, 20.0f, 38.0f},
                {1.6f, 255, 118, 74, 20.0f, 44.0f},
                {1.05f, 255, 150, 96, 18.0f, 50.0f},
            };
            for (const auto& layer : kLedHalo) {
                Glow halo;
                halo.center = center;
                halo.radius = layer.radius * gps;
                halo.core = false;
                halo.color = IM_COL32(layer.red, layer.green, layer.blue,
                                      static_cast<int>(layer.base + level * layer.span));
                pendingGlows_.push_back(halo);
            }
        }
        drawList->AddLine(P(0, -2), P(0, -0.8f), body, t);
        drawList->AddLine(P(0, 0.8f), P(0, 2), body, t);
        const ImVec2 triangle[] = {P(-0.8f, -0.8f), P(0.8f, -0.8f), P(0.0f, 0.8f)};
        drawList->AddPolyline(triangle, 3, body, ImDrawFlags_Closed, t);
        drawList->AddLine(P(-0.8f, 0.8f), P(0.8f, 0.8f), body, t);
        if (component.type == ComponentType::Zener) {
            drawList->AddLine(P(-0.8f, 0.8f), P(-0.55f, 0.8f), body, t);
            drawList->AddLine(P(0.8f, 0.8f), P(0.55f, 0.8f), body, t);
            drawList->AddLine(P(-0.8f, 0.8f), P(-0.95f, 0.55f), body, t);
            drawList->AddLine(P(0.8f, 0.8f), P(0.95f, 1.05f), body, t);
        }
        if (component.type == ComponentType::Led) {
            drawList->AddLine(P(0.55f, -0.55f), P(1.15f, -1.15f), detail, t);
            drawList->AddLine(P(1.15f, -1.15f), P(0.9f, -1.12f), detail, t);
            drawList->AddLine(P(1.15f, -1.15f), P(1.12f, -0.9f), detail, t);
            drawList->AddLine(P(0.2f, -0.25f), P(0.8f, -0.85f), detail, t);
            drawList->AddLine(P(0.8f, -0.85f), P(0.55f, -0.82f), detail, t);
            drawList->AddLine(P(0.8f, -0.85f), P(0.77f, -0.6f), detail, t);
        }
        break;
    }
    case ComponentType::Npn:
    case ComponentType::Pnp: {
        drawList->AddCircle(center, 1.15f * gps, body, 0, t);
        drawList->AddLine(P(-2.0f, 0.0f), P(-0.55f, 0.0f), body, t);
        drawList->AddLine(P(-0.55f, -0.8f), P(-0.55f, 0.8f), body, t);
        drawList->AddLine(P(2.0f, -2.0f), P(0.35f, -0.55f), body, t);
        drawList->AddLine(P(-0.55f, -0.4f), P(0.35f, -0.55f), body, t);
        drawList->AddLine(P(2.0f, 2.0f), P(0.35f, 0.55f), body, t);
        drawList->AddLine(P(-0.55f, 0.4f), P(0.35f, 0.55f), body, t);
        if (component.type == ComponentType::Npn) {
            drawList->AddTriangleFilled(P(0.35f, 0.55f), P(0.1f, 0.32f),
                                        P(0.02f, 0.62f), body);
        } else {
            drawList->AddTriangleFilled(P(-0.55f, 0.4f), P(-0.22f, 0.58f),
                                        P(-0.38f, 0.25f), body);
        }
        break;
    }
    case ComponentType::Nmos: {
        drawList->AddLine(P(-2.0f, 0.0f), P(-0.85f, 0.0f), body, t);
        drawList->AddLine(P(-0.85f, -0.85f), P(-0.85f, 0.85f), body, t);
        drawList->AddLine(P(-0.45f, -0.9f), P(-0.45f, 0.9f), body, t);
        drawList->AddLine(P(-0.45f, -0.6f), P(2.0f, -2.0f), body, t);
        drawList->AddLine(P(-0.45f, 0.6f), P(2.0f, 2.0f), body, t);
        drawList->AddLine(P(-0.45f, 0.0f), P(0.5f, 0.0f), body, t);
        drawList->AddTriangleFilled(P(0.5f, 0.0f), P(0.1f, -0.25f),
                                    P(0.1f, 0.25f), body);
        break;
    }
    case ComponentType::OpAmp: {
        // Leads reach the triangle edge; the supply pins enter top and bottom.
        drawList->AddLine(P(-2.0f, -1.0f), P(-1.45f, -1.0f), body, t);
        drawList->AddLine(P(-2.0f, 1.0f), P(-1.45f, 1.0f), body, t);
        drawList->AddLine(P(1.45f, 0.0f), P(2.0f, 0.0f), body, t);
        drawList->AddLine(P(0.0f, -2.0f), P(0.0f, -0.65f), body, t);
        drawList->AddLine(P(0.0f, 2.0f), P(0.0f, 0.65f), body, t);
        const ImVec2 triangle[] = {P(-1.45f, -1.5f), P(-1.45f, 1.5f), P(1.45f, 0.0f)};
        drawList->AddPolyline(triangle, 3, body, ImDrawFlags_Closed, t);
        drawList->AddLine(P(-1.1f, -0.85f), P(-0.6f, -0.85f), detail, t);
        drawList->AddLine(P(-0.85f, -1.1f), P(-0.85f, -0.6f), detail, t);
        drawList->AddLine(P(-1.1f, 0.85f), P(-0.6f, 0.85f), detail, t);
        break;
    }
    case ComponentType::Timer555: {
        const ImVec2 bodyMin = P(-1.4f, -2.6f);
        const ImVec2 bodyMax = P(1.4f, 2.6f);
        drawList->AddRect(bodyMin, bodyMax, body, dp(2.0f), 0, t);
        for (int pin = 0; pin < 4; ++pin) {
            const float y = -3.0f + 2.0f * static_cast<float>(pin);
            drawList->AddLine(P(-2.0f, y), P(-1.4f, y), body, t);
            drawList->AddLine(P(1.4f, y), P(2.0f, y), body, t);
        }
        const char* title = "555";
        const ImVec2 titleSize = ImGui::CalcTextSize(title);
        drawList->AddText(ImVec2(center.x - titleSize.x * 0.5f,
                                 center.y - titleSize.y * 0.5f),
                          body, title);
        break;
    }
    case ComponentType::GateAnd:
    case ComponentType::GateOr:
    case ComponentType::GateNot: {
        const ImVec2 bodyMin = P(-1.2f, -1.5f);
        const ImVec2 bodyMax = P(1.2f, 1.5f);
        drawList->AddRect(bodyMin, bodyMax, body, dp(2.0f), 0, t);
        if (component.type == ComponentType::GateNot) {
            drawList->AddLine(P(-2.0f, 0.0f), P(-1.2f, 0.0f), body, t);
            drawList->AddLine(P(1.2f, 0.0f), P(2.0f, 0.0f), body, t);
        } else {
            drawList->AddLine(P(-2.0f, -1.0f), P(-1.2f, -1.0f), body, t);
            drawList->AddLine(P(-2.0f, 1.0f), P(-1.2f, 1.0f), body, t);
            drawList->AddLine(P(1.2f, 0.0f), P(2.0f, 0.0f), body, t);
        }
        const char* title = component.type == ComponentType::GateAnd
                                ? "&"
                                : (component.type == ComponentType::GateOr ? "≥1"
                                                                            : "1");
        const ImVec2 titleSize = ImGui::CalcTextSize(title);
        drawList->AddText(ImVec2(center.x - titleSize.x * 0.5f,
                                 center.y - titleSize.y * 0.5f),
                          body, title);
        break;
    }
    case ComponentType::Regulator7805: {
        const ImVec2 bodyMin = P(-1.2f, -1.2f);
        const ImVec2 bodyMax = P(1.2f, 1.2f);
        drawList->AddRect(bodyMin, bodyMax, body, dp(2.0f), 0, t);
        drawList->AddLine(P(-2.0f, 0.0f), P(-1.2f, 0.0f), body, t);
        drawList->AddLine(P(1.2f, 0.0f), P(2.0f, 0.0f), body, t);
        drawList->AddLine(P(0.0f, 1.2f), P(0.0f, 2.0f), body, t);
        const char* title = "7805";
        const ImVec2 titleSize = ImGui::CalcTextSize(title);
        drawList->AddText(ImVec2(center.x - titleSize.x * 0.5f,
                                 center.y - titleSize.y * 0.5f),
                          body, title);
        break;
    }
    case ComponentType::PowerInductor: {
        drawList->AddLine(P(0, -2), P(0, -1.2f), body, t);
        drawList->AddLine(P(0, 1.2f), P(0, 2), body, t);
        polyline.clear();
        const int segments = 36;
        for (int i = 0; i <= segments; ++i) {
            const float u = static_cast<float>(i) / segments;
            const float y = -1.2f + 2.4f * u;
            polyline.push_back(P(0.38f * std::sin(u * 6.0f * 3.14159265f), y));
        }
        drawList->AddPolyline(polyline.data(), static_cast<int>(polyline.size()), body, 0, t);
        // Iron core bars mark a power inductor with a real winding resistance.
        drawList->AddLine(P(0.62f, -1.2f), P(0.62f, 1.2f), detail, 1.8f * uiScale_);
        drawList->AddLine(P(0.88f, -1.2f), P(0.88f, 1.2f), detail, 1.8f * uiScale_);
        break;
    }
    case ComponentType::ElectrolyticCap: {
        drawList->AddLine(P(0, -2), P(0, -0.3f), body, t);
        drawList->AddLine(P(0, 0.42f), P(0, 2), body, t);
        drawList->AddLine(P(-0.95f, -0.3f), P(0.95f, -0.3f), body, t);
        polyline.clear();
        for (int i = 0; i <= 12; ++i) {
            const float u = static_cast<float>(i) / 12.0f;
            polyline.push_back(P(-0.95f + 1.9f * u, 0.42f + 0.24f * std::sin(u * 3.14159265f)));
        }
        drawList->AddPolyline(polyline.data(), static_cast<int>(polyline.size()), body, 0, t);
        // Polarity mark next to the positive plate.
        drawList->AddLine(P(-0.72f, -0.85f), P(-0.38f, -0.85f), detail, 1.6f * uiScale_);
        drawList->AddLine(P(-0.55f, -1.02f), P(-0.55f, -0.68f), detail, 1.6f * uiScale_);
        break;
    }
    case ComponentType::PwmSource: {
        drawList->AddLine(P(0, -2), P(0, -0.85f), body, t);
        drawList->AddLine(P(0, 0.85f), P(0, 2), body, t);
        drawList->AddCircle(center, 0.85f * gps, body, 0, t);
        const ImVec2 pulse[] = {P(-0.55f, 0.22f), P(-0.36f, 0.22f), P(-0.36f, -0.22f),
                                P(-0.12f, -0.22f), P(-0.12f, 0.22f), P(0.12f, 0.22f),
                                P(0.12f, -0.22f), P(0.36f, -0.22f), P(0.36f, 0.22f),
                                P(0.55f, 0.22f)};
        drawList->AddPolyline(pulse, 10, detail, 0, 1.7f * uiScale_);
        break;
    }
    case ComponentType::Irl540N:
    case ComponentType::PowerNmos:
    case ComponentType::PowerPmos: {
        drawList->AddLine(P(-2.0f, 0.0f), P(-0.85f, 0.0f), body, t);
        drawList->AddLine(P(-0.85f, -0.85f), P(-0.85f, 0.85f), body, t);
        // A thicker channel bar marks a power device.
        drawList->AddLine(P(-0.45f, -0.95f), P(-0.45f, 0.95f), body, 3.0f * uiScale_);
        drawList->AddLine(P(-0.45f, -0.6f), P(2.0f, -2.0f), body, t);
        drawList->AddLine(P(-0.45f, 0.6f), P(2.0f, 2.0f), body, t);
        drawList->AddLine(P(-0.45f, 0.0f), P(0.55f, 0.0f), body, t);
        if (component.type == ComponentType::PowerNmos) {
            drawList->AddTriangleFilled(P(0.55f, 0.0f), P(0.15f, -0.25f), P(0.15f, 0.25f), body);
        } else {
            // P-channel: the arrow points into the channel.
            drawList->AddTriangleFilled(P(0.12f, 0.0f), P(0.52f, -0.25f), P(0.52f, 0.25f), body);
        }
        break;
    }
    case ComponentType::SchottkyDiode: {
        drawList->AddLine(P(0, -2), P(0, -0.8f), body, t);
        drawList->AddLine(P(0, 0.8f), P(0, 2), body, t);
        const ImVec2 triangle[] = {P(-0.8f, -0.8f), P(0.8f, -0.8f), P(0.0f, 0.8f)};
        drawList->AddPolyline(triangle, 3, body, ImDrawFlags_Closed, t);
        drawList->AddLine(P(-0.8f, 0.8f), P(0.8f, 0.8f), body, t);
        // "S" hooks on the cathode bar: the Schottky signature.
        drawList->AddLine(P(-0.8f, 0.8f), P(-0.8f, 0.45f), body, t);
        drawList->AddLine(P(-0.8f, 0.45f), P(-1.02f, 0.45f), body, t);
        drawList->AddLine(P(0.8f, 0.8f), P(0.8f, 1.15f), body, t);
        drawList->AddLine(P(0.8f, 1.15f), P(1.02f, 1.15f), body, t);
        break;
    }
    case ComponentType::Oscilloscope: {
        drawList->AddLine(P(0, -2), P(0, -1.1f), body, t);
        drawList->AddLine(P(0, 1.1f), P(0, 2), body, t);
        drawList->AddRect(P(-0.95f, -1.1f), P(0.95f, 1.1f), body, dp(2.5f), 0, t);
        const ImVec2 trace[] = {P(-0.68f, 0.32f), P(-0.34f, 0.32f), P(-0.34f, -0.32f),
                                P(0.04f, -0.32f), P(0.04f, 0.32f), P(0.42f, 0.32f),
                                P(0.42f, -0.32f), P(0.68f, -0.32f)};
        drawList->AddPolyline(trace, 8, detail, 0, 1.6f * uiScale_);
        break;
    }
    case ComponentType::TerminalBlock: {
        drawList->AddLine(P(0, -2), P(0, -1.15f), body, t);
        drawList->AddLine(P(0, 1.15f), P(0, 2), body, t);
        drawList->AddRect(P(-0.78f, -1.15f), P(0.78f, 1.15f), body, dp(2.5f), 0, t);
        drawList->AddCircle(P(0, -0.55f), 0.32f * gps, body, 0, t);
        drawList->AddCircle(P(0, 0.55f), 0.32f * gps, body, 0, t);
        drawList->AddLine(P(-0.18f, -0.73f), P(0.18f, -0.37f), detail, 1.4f * uiScale_);
        drawList->AddLine(P(-0.18f, 0.37f), P(0.18f, 0.73f), detail, 1.4f * uiScale_);
        break;
    }
    }

    if (damagedLoad) {
        drawList->AddLine(P(-0.55f, -0.55f), P(0.55f, 0.55f), Theme::danger(),
                          3.0f * t);
        drawList->AddLine(P(0.55f, -0.55f), P(-0.55f, 0.55f), Theme::danger(),
                          3.0f * t);
    }

    if (selected && pinCount(component.type) > 2) {
        const PinLayout& layout = pinLayout(component.type);
        Point placedPins[kMaxComponentPins];
        const int placedCount = componentPins(component, placedPins);
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.78f);
        for (int pin = 0; pin < placedCount && pin < layout.count; ++pin) {
            const char* name = layout.pins[static_cast<size_t>(pin)].name;
            // The real pin position: a mirrored or legacy part has its pins
            // somewhere the layout table alone does not tell.
            const ImVec2 point = worldToScreen(placedPins[pin]);
            const ImVec2 textSize = ImGui::CalcTextSize(name);
            const float dx = point.x - center.x;
            const float dy = point.y - center.y;
            ImVec2 textPos;
            if (std::fabs(dx) >= std::fabs(dy)) {
                textPos = ImVec2(dx >= 0.0f ? point.x + dp(4.0f)
                                           : point.x - textSize.x - dp(4.0f),
                                 point.y - textSize.y * 0.5f);
            } else {
                textPos = ImVec2(point.x - textSize.x * 0.5f,
                                 dy >= 0.0f ? point.y + dp(2.0f)
                                            : point.y - textSize.y - dp(2.0f));
            }
            drawList->AddText(textPos, Theme::accent(), name);
        }
        ImGui::PopFont();
    }

    const char* name = component.name.c_str();
    const std::string value = componentValueLabel(component);
    const ImVec2 nameSize = ImGui::CalcTextSize(name);
    const ImVec2 valueSize = ImGui::CalcTextSize(value.c_str());
    const float lineHeight = ImGui::GetFontSize() + 1.0f;
    // The label sits just under the symbol: the bounding box minus the one-cell
    // touch margin, so a two-cell part no longer pushes its name two cells away.
    Point boundsMin;
    Point boundsMax;
    componentBounds(component, &boundsMin, &boundsMax);
    const float labelY =
        center.y + static_cast<float>(boundsMax.y - 1 - component.gridY) * gp + dp(3.0f);
    const ImU32 nameColor = selected ? Theme::accent()
                                     : (damagedLoad ? Theme::danger() : Theme::text());
    const ImU32 valueColor = damagedLoad ? Theme::danger() : Theme::textDim();
    drawList->AddText(ImVec2(center.x - nameSize.x * 0.5f, labelY), nameColor, name);

    const bool showCurrent =
        haveCurrent && (flowing || selected) && pinCount(component.type) == 2;
    if (showCurrent) {
        std::string currentText;
        if (!flowing) {
            currentText = "0 A";
        } else {
            currentText = std::string(currentArrow(component, current)) + " " +
                          formatSI(std::fabs(current), "A");
        }
        const ImVec2 currentSize = ImGui::CalcTextSize(currentText.c_str());
        const float gap = ImGui::GetFontSize() * 0.35f;
        const float totalWidth = valueSize.x + gap + currentSize.x;
        const float x = center.x - totalWidth * 0.5f;
        const float y = labelY + lineHeight;
        drawList->AddText(ImVec2(x, y), valueColor, value.c_str());
        drawList->AddText(ImVec2(x + valueSize.x + gap, y), kCurrentColor,
                          currentText.c_str());
    } else {
        drawList->AddText(ImVec2(center.x - valueSize.x * 0.5f, labelY + lineHeight),
                          valueColor, value.c_str());
    }
}

void App::drawGhost(ImDrawList* drawList, ImVec2 mouse)
{
    const Point snapped = snapScreen(mouse);
    const ImVec2 p = worldToScreen(snapped);
    const float radius = 12.0f * uiScale_;
    drawList->AddCircle(p, radius, Theme::accent(), 0, 2.0f * uiScale_);
    drawList->AddLine(ImVec2(p.x - radius, p.y), ImVec2(p.x + radius, p.y),
                      Theme::accent(), 1.5f * uiScale_);
    drawList->AddLine(ImVec2(p.x, p.y - radius), ImVec2(p.x, p.y + radius),
                      Theme::accent(), 1.5f * uiScale_);
}

void App::handleCanvasTap(const ImVec2& mouse)
{
    if (placement_.active) {
        pushUndo();
        const Point snapped = snapScreen(mouse);
        const int id = circuit_.addComponent(placement_.type, snapped.x, snapped.y);
        Component* component = circuit_.findComponent(id);
        if (component) {
            if (component->type == ComponentType::SignalSource) {
                component->source.mode = placement_.waveform;
                component->source.dc = 0.0;
                component->source.amplitude = 5.0;
                component->source.frequency = 1000.0;
                component->source.period = 1e-3;
                component->source.pulseWidth = 0.5e-3;
                component->source.rise = 1e-7;
                component->value = 5.0;
            } else if (component->type == ComponentType::AcSource) {
                component->source.mode = Waveform::Sine;
                component->source.dc = 0.0;
                component->source.amplitude = 12.0;
                component->source.frequency = 50.0;
                component->value = 12.0;
            }
        }
        selectedComponent_ = id;
        selectedWire_ = -1;
        const std::string placed = componentInfo(placement_.type).label;
        // One pick places one part: no more "keep tapping" mode.
        placement_ = Placement{};
        markChanged();
        setStatus("已放置 " + placed + "，点“+”可以再放一个");
        return;
    }

    if (tool_ == Tool::Wire) {
        if (!wireStarted_) {
            // A wire always starts on a component pin; the end may be a pin, an
            // existing joint, or blank canvas (a stub / rail).
            Point pin;
            if (!hoveredPinTarget(mouse, &pin)) {
                setStatus("导线要从元件引脚开始，请点高亮的引脚");
                return;
            }
            wireStart_ = pin;
            wireStarted_ = true;
            setStatus("点第二个引脚、接头或空白处完成导线（自动布线）");
            return;
        }
        const Point target = wireEndPoint(mouse);
        if (target == wireStart_) {
            wireStarted_ = false;
            setStatus("已取消导线");
            return;
        }
        pushUndo();
        std::vector<Point> route;
        const bool routed = routeWire(circuit_, wireStart_, target, &route);
        if (!routed) {
            route = lShapedRoute(wireStart_, target);
        }
        circuit_.addWirePoints(std::move(route));
        markChanged();
        setStatus(routed ? "导线已添加（已自动绕开其他导线与引脚）"
                         : "导线已添加（没有干净路径，使用直角连接）");
        wireStarted_ = false;
        return;
    }

    // Selection is decided on touch-down (see drawCanvas) so that a slightly
    // drifting finger cannot select a different part on release.
}

void App::drawCanvas(float height)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::toVec4(Theme::canvas()));
    ImGui::BeginChild("##canvas", ImVec2(0.0f, height), 0,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();

    canvasOrigin_ = ImGui::GetCursorScreenPos();
    canvasSize_ = ImGui::GetContentRegionAvail();
    if (canvasSize_.x < 10.0f || canvasSize_.y < 10.0f) {
        ImGui::EndChild();
        return;
    }

    ImGui::InvisibleButton("##canvas_input", canvasSize_, ImGuiButtonFlags_MouseButtonLeft);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;

    if (fitPending_) {
        fitView();
    }

    if (hovered && io.MouseWheel != 0.0f) {
        const ImVec2 before = screenToWorld(mouse);
        zoom_ = std::clamp(zoom_ * (1.0f + io.MouseWheel * 0.12f), 0.3f, 3.5f);
        viewOffset_ = ImVec2(mouse.x - canvasOrigin_.x - before.x * gridPixels(),
                             mouse.y - canvasOrigin_.y - before.y * gridPixels());
    }

    const bool activated = ImGui::IsItemActivated();
    if (activated) {
        canvasPressed_ = true;
        canvasPressPos_ = mouse;
        suppressTap_ = pinching_;
        dragging_ = false;
        dragUndoPushed_ = false;
        dragComponent_ = -1;
        panCandidate_ = false;
        panningCanvas_ = false;
        panStartOffset_ = viewOffset_;
        if (!pinching_ && tool_ == Tool::Select) {
            if (placement_.active) {
                // Tapping places another component; dragging pans the sheet.
                panCandidate_ = true;
            } else {
                int component = -1;
                int wire = -1;
                hitSelection(mouse, &component, &wire);
                selectedComponent_ = component;
                selectedWire_ = wire;
                if (component >= 0) {
                    dragComponent_ = component;
                    if (const Component* hit = circuit_.findComponent(component)) {
                        const ImVec2 world = screenToWorld(mouse);
                        dragOffsetWorld_ =
                            ImVec2(world.x - hit->gridX, world.y - hit->gridY);
                    }
                } else {
                    selectedComponent_ = -1;
                    if (wire < 0) {
                        panCandidate_ = true;
                    }
                }
            }
        }
    }

    if (active && panCandidate_ && !pinching_) {
        if (!panningCanvas_ && ImGui::IsMouseDragging(0, 8.0f * uiScale_)) {
            panningCanvas_ = true;
            suppressTap_ = true;
        }
        if (panningCanvas_) {
            viewOffset_ = ImVec2(panStartOffset_.x + mouse.x - canvasPressPos_.x,
                                 panStartOffset_.y + mouse.y - canvasPressPos_.y);
        }
    }

    if (!pinching_ && tool_ == Tool::Select && dragComponent_ >= 0 &&
        ImGui::IsMouseDragging(0, dp(14.0f))) {
        if (!dragUndoPushed_) {
            pushUndo();
            dragUndoPushed_ = true;
        }
        dragging_ = true;
        const ImVec2 world = screenToWorld(mouse);
        const int nx = static_cast<int>(std::lround(world.x - dragOffsetWorld_.x));
        const int ny = static_cast<int>(std::lround(world.y - dragOffsetWorld_.y));
        if (circuit_.moveComponent(dragComponent_, nx, ny)) {
            markChanged();
        }
    }

    if (ImGui::IsItemDeactivated()) {
        const bool pressed = canvasPressed_;
        canvasPressed_ = false;
        const float distance =
            std::hypot(mouse.x - canvasPressPos_.x, mouse.y - canvasPressPos_.y);
        if (pressed && !pinching_ && !suppressTap_ && !panningCanvas_ &&
            distance < 18.0f * uiScale_) {
            handleCanvasTap(mouse);
        }
        dragging_ = false;
        dragComponent_ = -1;
        dragUndoPushed_ = false;
        panCandidate_ = false;
        panningCanvas_ = false;
        suppressTap_ = false;
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 canvasMax(canvasOrigin_.x + canvasSize_.x, canvasOrigin_.y + canvasSize_.y);
    drawList->PushClipRect(canvasOrigin_, canvasMax, true);
    drawGrid(drawList, canvasOrigin_, canvasMax);

    for (const auto& wire : circuit_.wires) {
        drawWire(drawList, wire, wire.id == selectedWire_);
    }
    drawWireFlow(drawList);
    pendingGlows_.clear();
    for (const auto& component : circuit_.components) {
        drawComponent(drawList, component, component.id == selectedComponent_);
    }
    // Lamp / LED halos last: light sits on top of copper and neighbouring parts.
    for (const Glow& glow : pendingGlows_) {
        drawList->AddCircleFilled(glow.center, glow.radius, glow.color);
    }
    pendingGlows_.clear();
    for (const auto& point : compiled_.connectionPoints) {
        drawList->AddCircleFilled(worldToScreen(point), 2.0f * uiScale_,
                                  IM_COL32(120, 132, 146, 150));
    }

    if (placement_.active && hovered) {
        drawGhost(drawList, mouse);
    }
    if (tool_ == Tool::Wire) {
        // Every joint gets a ring, so a wire end can snap to a pin or to an
        // existing joint; blank canvas is a legal end too, just not snapped.
        Point hoveredTarget;
        const bool hasHover = hovered && hoveredConnectionTarget(mouse, &hoveredTarget);
        const std::vector<Point> targets = connectionPoints(circuit_);
        for (const Point& target : targets) {
            const ImVec2 screen = worldToScreen(target);
            const bool isStart = wireStarted_ && target == wireStart_;
            const bool isHover = hasHover && target == hoveredTarget;
            if (isStart || isHover) {
                drawList->AddCircleFilled(screen, 5.5f * uiScale_,
                                          isStart ? Theme::warning() : Theme::accent());
                drawList->AddCircle(screen, 9.0f * uiScale_,
                                    isStart ? Theme::warning() : Theme::accent(), 0,
                                    2.0f * uiScale_);
            } else {
                drawList->AddCircle(screen, 4.0f * uiScale_,
                                    IM_COL32(76, 141, 255, 130), 0, 1.6f * uiScale_);
            }
        }
        if (wireStarted_) {
            // Preview the route the tap would create; the end follows the finger
            // even over blank canvas.
            const Point end = hovered ? wireEndPoint(mouse) : wireStart_;
            if (hovered && end != wireStart_) {
                const std::vector<Point>& path = previewRoute(wireStart_, end);
                for (size_t i = 0; i + 1 < path.size(); ++i) {
                    drawList->AddLine(worldToScreen(path[i]), worldToScreen(path[i + 1]),
                                      Theme::accent(), 2.6f * uiScale_);
                }
                drawList->AddCircle(worldToScreen(end), 7.0f * uiScale_,
                                    Theme::accent(), 0, 2.0f * uiScale_);
            }
            drawList->AddCircleFilled(worldToScreen(wireStart_), 5.0f * uiScale_,
                                      Theme::warning());
        }
    }

    const char* hint = nullptr;
    if (placement_.active) {
        hint = "点击放置 · 拖动平移";
    } else if (tool_ == Tool::Wire) {
        hint = wireStarted_ ? "点引脚 / 接头 / 空白处结束 · 自动布线"
                            : "点元件引脚开始导线";
    } else if (canvasSize_.x > dp(260.0f)) {
        hint = "拖动空白平移 · 双指缩放";
    }
    if (hint) {
        const ImVec2 textSize = ImGui::CalcTextSize(hint);
        const ImVec2 pos(canvasOrigin_.x + 14.0f * uiScale_,
                         canvasOrigin_.y + canvasSize_.y - textSize.y -
                             14.0f * uiScale_);
        drawList->AddRectFilled(
            pos, ImVec2(pos.x + textSize.x + 18.0f * uiScale_,
                        pos.y + textSize.y + 10.0f * uiScale_),
            IM_COL32(23, 29, 39, 225), 7.0f * uiScale_);
        drawList->AddText(ImVec2(pos.x + 9.0f * uiScale_,
                                 pos.y + 5.0f * uiScale_),
                          Theme::textDim(), hint);
    }
    if (!compiled_.ok && !circuit_.components.empty()) {
        const std::string warning = compiled_.error;
        const ImVec2 textSize = ImGui::CalcTextSize(warning.c_str());
        const ImVec2 pos(canvasOrigin_.x + 14.0f * uiScale_,
                         canvasMax.y - textSize.y - 14.0f * uiScale_);
        drawList->AddRectFilled(
            pos, ImVec2(pos.x + textSize.x + 18.0f * uiScale_,
                        pos.y + textSize.y + 10.0f * uiScale_),
            IM_COL32(60, 25, 25, 235), 7.0f * uiScale_);
        drawList->AddText(ImVec2(pos.x + 9.0f * uiScale_,
                                 pos.y + 5.0f * uiScale_),
                          Theme::danger(), warning.c_str());
    }

    drawList->PopClipRect();
    ImGui::EndChild();
}

void App::pinchBegin()
{
    if (screen_ != Screen::Editor) {
        return;
    }
    pinching_ = true;
    suppressTap_ = true;
    if (dragging_ && dragUndoPushed_ && !undoStack_.empty()) {
        // Cancel the in-progress drag while keeping revisions monotonic: a
        // restored snapshot must not reuse the revision of a newer result.
        const int nextRevision = circuit_.revision + 1;
        circuit_ = undoStack_.back();
        circuit_.revision = nextRevision;
        undoStack_.pop_back();
        updateCompiled();
        markDirty();
        if (live_) {
            scheduleLiveRerun(250);
        }
    }
    dragging_ = false;
    dragComponent_ = -1;
    dragUndoPushed_ = false;
    panCandidate_ = false;
    panningCanvas_ = false;
    wireStarted_ = false;
}

void App::pinchZoom(float scale, ImVec2 centerNormalized)
{
    if (screen_ != Screen::Editor || !pinching_ || !(scale > 0.0f)) {
        return;
    }
    if (canvasSize_.x < 10.0f || canvasSize_.y < 10.0f) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 point(centerNormalized.x * io.DisplaySize.x,
                 centerNormalized.y * io.DisplaySize.y);
    const ImVec2 min = canvasOrigin_;
    const ImVec2 max(canvasOrigin_.x + canvasSize_.x, canvasOrigin_.y + canvasSize_.y);
    if (point.x < min.x || point.y < min.y || point.x > max.x || point.y > max.y) {
        return;
    }
    const ImVec2 before = screenToWorld(point);
    zoom_ = std::clamp(zoom_ * scale, 0.3f, 3.5f);
    viewOffset_ = ImVec2(point.x - canvasOrigin_.x - before.x * gridPixels(),
                         point.y - canvasOrigin_.y - before.y * gridPixels());
}

void App::pinchEnd()
{
    pinching_ = false;
}

void drawComponentGlyph(ImDrawList* drawList, ImVec2 center, float size,
                        ComponentType type, ImU32 color)
{
    const float s = size * 0.5f;
    const float thickness = std::max(1.2f, size * 0.065f);
    const auto P = [&](float x, float y) {
        return ImVec2(center.x + x * s, center.y + y * s);
    };
    const auto line = [&](float x1, float y1, float x2, float y2) {
        drawList->AddLine(P(x1, y1), P(x2, y2), color, thickness);
    };
    const auto circle = [&](float radius) {
        drawList->AddCircle(center, radius * s, color, 0, thickness);
    };

    switch (type) {
    case ComponentType::PowerResistor:
        line(-0.95f, 0.0f, -0.55f, 0.0f);
        line(0.55f, 0.0f, 0.95f, 0.0f);
        drawList->AddRect(P(-0.55f, -0.4f), P(0.55f, 0.4f), color, 0.0f, 0, thickness);
        break;
    case ComponentType::Resistor: {
        const ImVec2 points[] = {
            P(-0.95f, 0.0f), P(-0.75f, 0.0f), P(-0.6f, -0.4f), P(-0.35f, 0.4f),
            P(-0.1f, -0.4f), P(0.15f, 0.4f), P(0.4f, -0.4f), P(0.65f, 0.0f),
            P(0.95f, 0.0f),
        };
        drawList->AddPolyline(points, static_cast<int>(sizeof(points) / sizeof(points[0])),
                              color, 0, thickness);
        break;
    }
    case ComponentType::Capacitor:
    case ComponentType::CeramicCap:
        line(-0.95f, 0.0f, -0.18f, 0.0f);
        line(0.18f, 0.0f, 0.95f, 0.0f);
        line(-0.18f, -0.55f, -0.18f, 0.55f);
        line(0.18f, -0.55f, 0.18f, 0.55f);
        if (type == ComponentType::CeramicCap) {
            drawList->AddRect(P(-0.42f, -0.72f), P(0.42f, 0.72f), color, 0.0f, 0,
                              thickness * 0.7f);
        }
        break;
    case ComponentType::Inductor: {
        std::vector<ImVec2> points;
        for (int i = 0; i <= 24; ++i) {
            const float u = static_cast<float>(i) / 24.0f;
            points.push_back(P(-0.9f + 1.8f * u, 0.32f * std::sin(u * 6.2831853f)));
        }
        drawList->AddPolyline(points.data(), static_cast<int>(points.size()), color, 0,
                              thickness);
        break;
    }
    case ComponentType::VoltageSource:
    case ComponentType::AcSource:
    case ComponentType::SignalSource: {
        circle(0.55f);
        line(-0.95f, 0.0f, -0.55f, 0.0f);
        line(0.55f, 0.0f, 0.95f, 0.0f);
        const bool alternating = type != ComponentType::VoltageSource;
        if (!alternating) {
            line(-0.24f, -0.16f, 0.0f, -0.16f);
            line(-0.12f, -0.28f, -0.12f, -0.04f);
            line(-0.24f, 0.2f, 0.0f, 0.2f);
        } else {
            std::vector<ImVec2> wave;
            for (int i = 0; i <= 20; ++i) {
                const float u = static_cast<float>(i) / 20.0f;
                wave.push_back(P(-0.32f + 0.64f * u,
                                 -0.2f * std::sin(u * 6.2831853f)));
            }
            drawList->AddPolyline(wave.data(), static_cast<int>(wave.size()), color, 0,
                                  thickness);
        }
        break;
    }
    case ComponentType::Ground:
        line(0.0f, -0.85f, 0.0f, 0.05f);
        line(-0.7f, 0.05f, 0.7f, 0.05f);
        line(-0.45f, 0.3f, 0.45f, 0.3f);
        line(-0.2f, 0.55f, 0.2f, 0.55f);
        break;
    case ComponentType::Switch:
        line(-0.95f, 0.0f, -0.55f, 0.0f);
        line(0.55f, 0.0f, 0.95f, 0.0f);
        drawList->AddCircleFilled(P(-0.38f, 0.0f), thickness * 1.3f, color);
        drawList->AddCircleFilled(P(0.38f, 0.0f), thickness * 1.3f, color);
        line(-0.38f, 0.0f, 0.3f, -0.55f);
        break;
    case ComponentType::Bulb: {
        line(-0.95f, 0.0f, -0.52f, 0.0f);
        line(0.34f, 0.0f, 0.95f, 0.0f);
        drawList->AddCircle(P(-0.06f, 0.0f), 0.46f * s, color, 0, thickness);
        const ImVec2 filament[] = {P(-0.34f, -0.2f), P(-0.16f, 0.22f), P(0.02f, -0.18f),
                                   P(0.18f, 0.2f), P(0.32f, -0.16f)};
        drawList->AddPolyline(filament, 5, color, 0, thickness * 0.85f);
        // Screw base: two short bars on the right.
        line(0.4f, -0.26f, 0.4f, 0.26f);
        line(0.58f, -0.2f, 0.58f, 0.2f);
        break;
    }
    case ComponentType::Fan:
        line(-0.95f, 0.0f, -0.55f, 0.0f);
        line(0.55f, 0.0f, 0.95f, 0.0f);
        circle(0.55f);
        for (int blade = 0; blade < 3; ++blade) {
            const float angle = static_cast<float>(blade) * 2.0943951f;
            line(0.0f, 0.0f, std::cos(angle) * 0.42f, std::sin(angle) * 0.42f);
        }
        break;
    case ComponentType::Tec12706:
    case ComponentType::Tec12710:
        line(-0.95f, 0.0f, -0.55f, 0.0f);
        line(0.55f, 0.0f, 0.95f, 0.0f);
        line(-0.55f, -0.55f, -0.55f, 0.55f);
        line(0.55f, -0.55f, 0.55f, 0.55f);
        for (int couple = 0; couple < 3; ++couple) {
            const float y = -0.32f + 0.32f * static_cast<float>(couple);
            line(-0.34f, y, 0.34f, y);
        }
        break;
    case ComponentType::Diode:
    case ComponentType::Led:
    case ComponentType::Zener: {
        line(-0.95f, 0.0f, -0.35f, 0.0f);
        line(0.4f, 0.0f, 0.95f, 0.0f);
        const ImVec2 triangle[] = {P(-0.35f, -0.55f), P(-0.35f, 0.55f), P(0.4f, 0.0f)};
        drawList->AddPolyline(triangle, 3, color, ImDrawFlags_Closed, thickness);
        if (type == ComponentType::Zener) {
            line(0.4f, -0.55f, 0.4f, 0.55f);
            line(0.4f, -0.55f, 0.25f, -0.55f);
            line(0.4f, 0.55f, 0.55f, 0.55f);
        } else {
            line(0.4f, -0.55f, 0.4f, 0.55f);
        }
        if (type == ComponentType::Led) {
            line(0.45f, -0.45f, 0.8f, -0.8f);
            line(0.45f, -0.2f, 0.8f, -0.55f);
            line(0.62f, -0.8f, 0.8f, -0.8f);
            line(0.8f, -0.8f, 0.8f, -0.62f);
        }
        break;
    }
    case ComponentType::Npn:
    case ComponentType::Pnp: {
        circle(0.62f);
        line(-0.95f, 0.0f, -0.25f, 0.0f);
        line(-0.25f, -0.5f, -0.25f, 0.5f);
        line(-0.25f, -0.22f, 0.55f, -0.62f);
        line(-0.25f, 0.22f, 0.55f, 0.62f);
        if (type == ComponentType::Npn) {
            drawList->AddTriangleFilled(P(0.55f, 0.62f), P(0.42f, 0.38f),
                                        P(0.24f, 0.55f), color);
        } else {
            drawList->AddTriangleFilled(P(-0.25f, 0.22f), P(-0.08f, 0.42f),
                                        P(-0.24f, 0.52f), color);
        }
        break;
    }
    case ComponentType::Irl540N:
    case ComponentType::Nmos:
        line(-0.95f, 0.0f, -0.45f, 0.0f);
        line(-0.45f, -0.45f, -0.45f, 0.45f);
        line(-0.2f, -0.55f, -0.2f, 0.55f);
        line(-0.2f, -0.4f, 0.55f, -0.4f);
        line(-0.2f, 0.4f, 0.55f, 0.4f);
        line(0.55f, -0.4f, 0.55f, -0.95f);
        line(0.55f, 0.4f, 0.55f, 0.95f);
        break;
    case ComponentType::OpAmp: {
        const ImVec2 triangle[] = {P(-0.7f, -0.75f), P(-0.7f, 0.75f), P(0.75f, 0.0f)};
        drawList->AddPolyline(triangle, 3, color, ImDrawFlags_Closed, thickness);
        line(-0.95f, -0.35f, -0.7f, -0.35f);
        line(-0.95f, 0.35f, -0.7f, 0.35f);
        line(0.75f, 0.0f, 0.95f, 0.0f);
        line(-0.55f, -0.35f, -0.35f, -0.35f);
        line(-0.45f, -0.45f, -0.45f, -0.25f);
        line(-0.55f, 0.35f, -0.35f, 0.35f);
        break;
    }
    case ComponentType::Timer555:
    case ComponentType::GateAnd:
    case ComponentType::GateOr:
    case ComponentType::GateNot:
    case ComponentType::Regulator7805: {
        const float halfW = type == ComponentType::Timer555 ? 0.6f : 0.55f;
        const float halfH = type == ComponentType::Timer555 ? 0.8f : 0.5f;
        drawList->AddRect(P(-halfW, -halfH), P(halfW, halfH), color, s * 0.14f, 0,
                          thickness);
        const char* text = "555";
        if (type == ComponentType::GateAnd) {
            text = "&";
        } else if (type == ComponentType::GateOr) {
            text = "≥1";
        } else if (type == ComponentType::GateNot) {
            text = "1";
        } else if (type == ComponentType::Regulator7805) {
            text = "7805";
        }
        ImGui::PushFont(nullptr, size * (type == ComponentType::Timer555 ? 0.2f : 0.24f));
        const ImVec2 textSize = ImGui::CalcTextSize(text);
        drawList->AddText(ImVec2(center.x - textSize.x * 0.5f,
                                 center.y - textSize.y * 0.5f),
                          color, text);
        ImGui::PopFont();
        if (type == ComponentType::Timer555) {
            for (int pin = 0; pin < 4; ++pin) {
                const float py = -0.6f + 0.4f * static_cast<float>(pin);
                line(-0.95f, py, -halfW, py);
                line(halfW, py, 0.95f, py);
            }
        } else if (type == ComponentType::Regulator7805) {
            line(-0.95f, 0.0f, -halfW, 0.0f);
            line(halfW, 0.0f, 0.95f, 0.0f);
            line(0.0f, halfH, 0.0f, 0.95f);
        } else if (type == ComponentType::GateNot) {
            line(-0.95f, 0.0f, -halfW, 0.0f);
            line(halfW, 0.0f, 0.95f, 0.0f);
        } else {
            line(-0.95f, -0.25f, -halfW, -0.25f);
            line(-0.95f, 0.25f, -halfW, 0.25f);
            line(halfW, 0.0f, 0.95f, 0.0f);
        }
        break;
    }
    case ComponentType::PowerInductor: {
        std::vector<ImVec2> points;
        for (int i = 0; i <= 24; ++i) {
            const float u = static_cast<float>(i) / 24.0f;
            points.push_back(P(-0.9f + 1.8f * u, 0.32f * std::sin(u * 6.2831853f)));
        }
        drawList->AddPolyline(points.data(), static_cast<int>(points.size()), color, 0,
                              thickness);
        line(-0.45f, -0.55f, -0.45f, 0.55f);
        line(-0.25f, -0.55f, -0.25f, 0.55f);
        break;
    }
    case ComponentType::ElectrolyticCap:
        line(-0.95f, 0.0f, -0.2f, 0.0f);
        line(0.28f, 0.0f, 0.95f, 0.0f);
        line(-0.2f, -0.6f, -0.2f, 0.6f);
        line(0.2f, -0.5f, 0.28f, 0.0f);
        line(0.2f, 0.5f, 0.28f, 0.0f);
        line(-0.55f, -0.45f, -0.35f, -0.45f);
        line(-0.45f, -0.55f, -0.45f, -0.35f);
        break;
    case ComponentType::PwmSource: {
        circle(0.55f);
        line(-0.95f, 0.0f, -0.55f, 0.0f);
        line(0.55f, 0.0f, 0.95f, 0.0f);
        const ImVec2 pulse[] = {P(-0.34f, 0.2f), P(-0.18f, 0.2f), P(-0.18f, -0.2f),
                                P(-0.02f, -0.2f), P(-0.02f, 0.2f), P(0.16f, 0.2f),
                                P(0.16f, -0.2f), P(0.34f, -0.2f)};
        drawList->AddPolyline(pulse, 8, color, 0, thickness);
        break;
    }
    case ComponentType::PowerNmos:
    case ComponentType::PowerPmos:
        line(-0.95f, 0.0f, -0.45f, 0.0f);
        line(-0.45f, -0.45f, -0.45f, 0.45f);
        line(-0.2f, -0.6f, -0.2f, 0.6f);
        line(-0.2f, -0.4f, 0.55f, -0.4f);
        line(-0.2f, 0.4f, 0.55f, 0.4f);
        line(0.55f, -0.4f, 0.55f, -0.95f);
        line(0.55f, 0.4f, 0.55f, 0.95f);
        line(-0.2f, 0.0f, 0.35f, 0.0f);
        if (type == ComponentType::PowerNmos) {
            drawList->AddTriangleFilled(P(0.42f, 0.0f), P(0.18f, -0.16f),
                                        P(0.18f, 0.16f), color);
        } else {
            drawList->AddTriangleFilled(P(0.06f, 0.0f), P(0.3f, -0.16f), P(0.3f, 0.16f),
                                        color);
        }
        break;
    case ComponentType::SchottkyDiode: {
        line(-0.95f, 0.0f, -0.35f, 0.0f);
        line(0.4f, 0.0f, 0.95f, 0.0f);
        const ImVec2 triangle[] = {P(-0.35f, -0.55f), P(-0.35f, 0.55f), P(0.4f, 0.0f)};
        drawList->AddPolyline(triangle, 3, color, ImDrawFlags_Closed, thickness);
        line(0.4f, -0.55f, 0.4f, 0.55f);
        line(0.4f, -0.55f, 0.28f, -0.55f);
        line(0.4f, 0.55f, 0.52f, 0.55f);
        break;
    }
    case ComponentType::Oscilloscope: {
        drawList->AddRect(P(-0.62f, -0.62f), P(0.62f, 0.62f), color, s * 0.16f, 0,
                          thickness);
        line(-0.95f, 0.0f, -0.62f, 0.0f);
        line(0.62f, 0.0f, 0.95f, 0.0f);
        const ImVec2 trace[] = {P(-0.44f, 0.2f), P(-0.2f, 0.2f), P(-0.2f, -0.2f),
                                P(0.04f, -0.2f), P(0.04f, 0.2f), P(0.28f, 0.2f),
                                P(0.28f, -0.2f), P(0.44f, -0.2f)};
        drawList->AddPolyline(trace, 8, color, 0, thickness);
        break;
    }
    case ComponentType::TerminalBlock: {
        drawList->AddRect(P(-0.45f, -0.62f), P(0.45f, 0.62f), color, s * 0.14f, 0,
                          thickness);
        line(-0.95f, -0.3f, -0.45f, -0.3f);
        line(-0.95f, 0.3f, -0.45f, 0.3f);
        line(0.45f, -0.3f, 0.95f, -0.3f);
        line(0.45f, 0.3f, 0.95f, 0.3f);
        drawList->AddCircleFilled(P(0.0f, -0.3f), thickness * 1.5f, color);
        drawList->AddCircleFilled(P(0.0f, 0.3f), thickness * 1.5f, color);
        break;
    }
    }
}

}  // namespace circuitlab
