#include "circuit/Circuit.h"

#include "circuit/ComponentCatalog.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

namespace circuitlab {
namespace {

Point rotateLocal(int x, int y, int rotation)
{
    switch (((rotation % 360) + 360) % 360) {
    case 90:
        return Point{-y, x};
    case 180:
        return Point{-x, -y};
    case 270:
        return Point{y, -x};
    case 0:
    default:
        return Point{x, y};
    }
}

}  // namespace

int pinCount(ComponentType type)
{
    return pinLayout(type).count;
}

int componentPins(const Component& component, Point out[kMaxComponentPins])
{
    const PinLayout& layout = pinLayout(component.type);
    const int count = std::min(layout.count, kMaxComponentPins);
    for (int i = 0; i < count; ++i) {
        const PinDef& pin = layout.pins[static_cast<size_t>(i)];
        const Point rotated = rotateLocal(pin.x, pin.y, component.rotation);
        out[i] = Point{component.gridX + rotated.x, component.gridY + rotated.y};
    }
    return count;
}

void componentBounds(const Component& component, Point* minOut, Point* maxOut)
{
    const PinLayout& layout = pinLayout(component.type);

    // AABB of the rotated symbol body. Rotating the body rectangle by 90°
    // swaps its extents.
    const bool horizontal =
        (((component.rotation % 360) + 360) % 360) == 90 ||
        (((component.rotation % 360) + 360) % 360) == 270;
    const int bodyHalfW = horizontal ? layout.bodyHalfHeight : layout.bodyHalfWidth;
    const int bodyHalfH = horizontal ? layout.bodyHalfWidth : layout.bodyHalfHeight;
    int minX = component.gridX - bodyHalfW;
    int maxX = component.gridX + bodyHalfW;
    int minY = component.gridY - bodyHalfH;
    int maxY = component.gridY + bodyHalfH;

    Point pins[kMaxComponentPins];
    const int count = componentPins(component, pins);
    for (int i = 0; i < count; ++i) {
        minX = std::min(minX, pins[i].x);
        maxX = std::max(maxX, pins[i].x);
        minY = std::min(minY, pins[i].y);
        maxY = std::max(maxY, pins[i].y);
    }

    // Extra room for labels, current readouts and touch hit testing.
    minOut[0] = Point{minX - 1, minY - 1};
    maxOut[0] = Point{maxX + 1, maxY + 1};
}

const char* componentPrefix(ComponentType type)
{
    return componentInfo(type).prefix;
}

const char* componentTypeKey(ComponentType type)
{
    return componentInfo(type).typeKey;
}

bool componentTypeFromKey(const std::string& key, ComponentType* out)
{
    return parseComponentTypeKey(key, out);
}

bool isSourceType(ComponentType type)
{
    return type == ComponentType::VoltageSource || type == ComponentType::AcSource ||
           type == ComponentType::SignalSource || type == ComponentType::PwmSource;
}

bool isLoadType(ComponentType type)
{
    return type == ComponentType::Bulb || type == ComponentType::Fan;
}

void applyLoadDefaults(Component& component)
{
    if (!isLoadType(component.type)) {
        return;
    }
    const double rated = component.ratedVoltage > 0.0
                             ? component.ratedVoltage
                             : (component.type == ComponentType::Fan ? 12.0 : 12.0);
    component.ratedVoltage = rated;
    const bool fan = component.type == ComponentType::Fan;
    // A fan barely turns at half voltage and tolerates about 10 % over its
    // rating; a lamp starts lower and dies a little later. Currents follow the
    // resistance, so the current limit is expressed at the maximum voltage.
    if (!(component.minVoltage > 0.0)) {
        component.minVoltage = rated * (fan ? 0.58 : 0.45);
    }
    if (!(component.maxVoltage > 0.0)) {
        component.maxVoltage = rated * (fan ? 1.10 : 1.15);
    }
    const double resistance = component.value > 0.0 ? component.value : 1.0;
    if (!(component.maxCurrent > 0.0)) {
        component.maxCurrent = component.maxVoltage / resistance;
    }
    component.minVoltage = std::min(component.minVoltage, rated);
    component.maxVoltage = std::max(component.maxVoltage, rated);
    component.maxCurrent = std::max(component.maxCurrent, 0.0);
}

LoadFault loadFault(const Component& component, double voltage, double current)
{
    if (!isLoadType(component.type)) {
        return LoadFault::None;
    }
    const double magnitude = std::fabs(voltage);
    if (component.maxVoltage > 0.0 && magnitude > component.maxVoltage) {
        return LoadFault::OverVoltage;
    }
    if (component.maxCurrent > 0.0 && std::fabs(current) > component.maxCurrent) {
        return LoadFault::OverCurrent;
    }
    return LoadFault::None;
}

double loadLevel(const Component& component, double voltage)
{
    if (!isLoadType(component.type) || component.damaged) {
        return 0.0;
    }
    const double magnitude = std::fabs(voltage);
    const double rated = component.ratedVoltage > 0.0 ? component.ratedVoltage : 0.0;
    const double start = component.minVoltage > 0.0 ? component.minVoltage : 0.0;
    if (!(rated > 0.0) || magnitude < start) {
        return 0.0;
    }
    // Electrical power rises as V^2; scale it so the nominal point is 1.0.
    const double ratio = std::clamp(magnitude / rated, 0.0, 1.0);
    const double level = ratio * ratio;
    // Lamps and fans are barely perceptible right at the start-up voltage.
    const double startFraction = std::clamp(start / rated, 0.0, 0.99);
    if (magnitude <= rated && magnitude > start && startFraction > 0.0) {
        const double span = std::max(1e-9, 1.0 - startFraction);
        const double travel = std::clamp((magnitude / rated - startFraction) / span, 0.0, 1.0);
        return std::min(1.0, 0.15 * travel + 0.85 * level);
    }
    return std::min(1.0, level);
}

double loadRatedCurrent(const Component& component)
{
    const double resistance = component.value > 0.0 ? component.value : 0.0;
    if (!(resistance > 0.0) || !(component.ratedVoltage > 0.0)) {
        return 0.0;
    }
    return component.ratedVoltage / resistance;
}

double loadRatedPower(const Component& component)
{
    return component.ratedVoltage * loadRatedCurrent(component);
}

bool isProbeType(ComponentType type)
{
    return type == ComponentType::Oscilloscope;
}

bool isConnectorType(ComponentType type)
{
    return type == ComponentType::TerminalBlock;
}

int clearLoadDamage(Circuit& circuit)
{
    int restored = 0;
    for (auto& component : circuit.components) {
        if (isLoadType(component.type) && component.damaged) {
            component.damaged = false;
            ++restored;
        }
    }
    if (restored > 0) {
        circuit.touch();
    }
    return restored;
}

bool isResistiveType(ComponentType type)
{
    return type == ComponentType::Resistor || type == ComponentType::Switch ||
           type == ComponentType::Bulb || type == ComponentType::Fan ||
           isProbeType(type);
}

double componentResistance(const Component& component)
{
    if (component.type == ComponentType::Switch) {
        return component.switchClosed ? 1e-3 : 1e12;
    }
    if (isLoadType(component.type) && component.damaged) {
        return 1e12;
    }
    return component.value;
}

const char* waveformKey(Waveform waveform)
{
    switch (waveform) {
    case Waveform::DC:
        return "DC";
    case Waveform::Sine:
        return "SINE";
    case Waveform::Pulse:
        return "PULSE";
    }
    return "DC";
}

bool waveformFromKey(const std::string& key, Waveform* out)
{
    if (key == "DC") {
        *out = Waveform::DC;
    } else if (key == "SINE") {
        *out = Waveform::Sine;
    } else if (key == "PULSE") {
        *out = Waveform::Pulse;
    } else {
        return false;
    }
    return true;
}

Point snapPoint(float x, float y)
{
    return Point{static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y))};
}

void Circuit::clear()
{
    components.clear();
    wires.clear();
    nextComponentId = 1;
    nextWireId = 1;
    touch();
}

Component* Circuit::findComponent(int id)
{
    for (auto& component : components) {
        if (component.id == id) {
            return &component;
        }
    }
    return nullptr;
}

const Component* Circuit::findComponent(int id) const
{
    for (const auto& component : components) {
        if (component.id == id) {
            return &component;
        }
    }
    return nullptr;
}

Wire* Circuit::findWire(int id)
{
    for (auto& wire : wires) {
        if (wire.id == id) {
            return &wire;
        }
    }
    return nullptr;
}

const Wire* Circuit::findWire(int id) const
{
    for (const auto& wire : wires) {
        if (wire.id == id) {
            return &wire;
        }
    }
    return nullptr;
}

std::string Circuit::uniqueName(ComponentType type) const
{
    const std::string prefix = componentPrefix(type);
    for (int index = 1;; ++index) {
        const std::string candidate = prefix + std::to_string(index);
        bool used = false;
        for (const auto& component : components) {
            if (component.name == candidate) {
                used = true;
                break;
            }
        }
        if (!used) {
            return candidate;
        }
    }
}

int Circuit::addComponent(ComponentType type, int gridX, int gridY)
{
    Component component;
    component.id = nextComponentId++;
    component.type = type;
    component.name = uniqueName(type);
    component.gridX = gridX;
    component.gridY = gridY;
    component.rotation = 0;

    switch (type) {
    case ComponentType::Resistor:
        component.value = 1000.0;
        break;
    case ComponentType::Capacitor:
        component.value = 1e-6;
        break;
    case ComponentType::Inductor:
        component.value = 1e-3;
        break;
    case ComponentType::VoltageSource:
        component.value = 5.0;
        component.source.mode = Waveform::DC;
        component.source.dc = 5.0;
        component.source.amplitude = 5.0;
        component.source.frequency = 1000.0;
        break;
    case ComponentType::AcSource:
        component.value = 12.0;
        component.source.mode = Waveform::Sine;
        component.source.dc = 0.0;
        component.source.amplitude = 12.0;
        component.source.frequency = 50.0;
        break;
    case ComponentType::SignalSource:
        component.value = 5.0;
        component.source.mode = Waveform::Sine;
        component.source.dc = 0.0;
        component.source.amplitude = 5.0;
        component.source.frequency = 1000.0;
        component.source.period = 1e-3;
        component.source.pulseWidth = 0.5e-3;
        break;
    case ComponentType::Switch:
        component.value = 0.0;
        component.switchClosed = true;
        break;
    case ComponentType::Bulb:
        // 12 V / 12 W lamp (1 A at nominal).
        component.value = 12.0;
        component.ratedVoltage = 12.0;
        break;
    case ComponentType::Fan:
        // 12 V / 1.4 W fan (0.12 A at nominal, runs from about 7 V).
        component.value = 100.0;
        component.ratedVoltage = 12.0;
        break;
    case ComponentType::Ground:
        component.value = 0.0;
        break;
    case ComponentType::Diode:
        component.value = 0.7;
        break;
    case ComponentType::Led:
        component.value = 2.0;
        break;
    case ComponentType::Zener:
        component.value = 5.1;
        break;
    case ComponentType::Npn:
    case ComponentType::Pnp:
    case ComponentType::Nmos:
    case ComponentType::OpAmp:
    case ComponentType::Timer555:
    case ComponentType::GateAnd:
    case ComponentType::GateOr:
    case ComponentType::GateNot:
    case ComponentType::Regulator7805:
    case ComponentType::PowerNmos:
    case ComponentType::PowerPmos:
        component.value = 0.0;
        break;
    case ComponentType::PowerInductor:
        component.value = 100e-6;
        component.seriesR = 0.06;
        break;
    case ComponentType::ElectrolyticCap:
        component.value = 220e-6;
        component.seriesR = 0.12;
        break;
    case ComponentType::PwmSource:
        component.value = 5.0;
        component.source.mode = Waveform::Pulse;
        component.source.dc = 0.0;
        component.source.amplitude = 5.0;
        component.source.frequency = 50e3;
        component.source.period = 20e-6;
        component.source.duty = 0.5;
        component.source.pulseWidth = 10e-6;
        component.source.rise = 50e-9;
        break;
    case ComponentType::SchottkyDiode:
        component.value = 0.4;
        break;
    case ComponentType::Oscilloscope:
        // Differential probe input: 1 MΩ to ground, as on a real scope.
        component.value = 1e6;
        break;
    case ComponentType::TerminalBlock:
        // A terminal block is a mechanical part: no value of its own.
        component.value = 0.0;
        break;
    }

    // Loads carry realistic start-up / maximum ratings.
    applyLoadDefaults(component);

    components.push_back(component);
    touch();
    return component.id;
}

int Circuit::addWire(Point a, Point b)
{
    if (a == b) {
        return 0;
    }
    std::vector<Point> points;
    points.push_back(a);
    if (a.x != b.x && a.y != b.y) {
        points.push_back(Point{b.x, a.y});
    }
    points.push_back(b);
    return addWirePoints(std::move(points));
}

int Circuit::addWirePoints(std::vector<Point> points)
{
    if (points.size() < 2) {
        return 0;
    }
    Wire wire;
    wire.id = nextWireId++;
    wire.points = std::move(points);
    normalizeWire(wire);
    if (wire.points.size() < 2) {
        return 0;
    }
    wires.push_back(std::move(wire));
    touch();
    return wires.back().id;
}

void Circuit::removeComponent(int id)
{
    const Component* component = findComponent(id);
    if (!component) {
        return;
    }
    Point pins[kMaxComponentPins];
    const int count = componentPins(*component, pins);

    wires.erase(std::remove_if(wires.begin(), wires.end(), [&](const Wire& wire) {
        for (const auto& point : wire.points) {
            for (int p = 0; p < count; ++p) {
                if (point == pins[p]) {
                    return true;
                }
            }
        }
        return false;
    }), wires.end());

    components.erase(std::remove_if(components.begin(), components.end(),
                                    [id](const Component& c) { return c.id == id; }),
                     components.end());
    touch();
}

void Circuit::removeWire(int id)
{
    const auto oldSize = wires.size();
    wires.erase(std::remove_if(wires.begin(), wires.end(),
                               [id](const Wire& wire) { return wire.id == id; }),
                wires.end());
    if (wires.size() != oldSize) {
        touch();
    }
}

bool Circuit::moveComponent(int id, int newGridX, int newGridY)
{
    Component* component = findComponent(id);
    if (!component) {
        return false;
    }
    if (component->gridX == newGridX && component->gridY == newGridY) {
        return false;
    }

    Point oldPins[kMaxComponentPins];
    const int oldCount = componentPins(*component, oldPins);

    component->gridX = newGridX;
    component->gridY = newGridY;

    Point newPins[kMaxComponentPins];
    const int newCount = componentPins(*component, newPins);
    followPins(id, oldPins, oldCount, newPins, newCount);
    touch();
    return true;
}

bool Circuit::rotateComponent(int id)
{
    Component* component = findComponent(id);
    if (!component) {
        return false;
    }

    Point oldPins[kMaxComponentPins];
    const int oldCount = componentPins(*component, oldPins);

    component->rotation = (component->rotation + 90) % 360;

    Point newPins[kMaxComponentPins];
    const int newCount = componentPins(*component, newPins);
    followPins(id, oldPins, oldCount, newPins, newCount);
    touch();
    return true;
}

void Circuit::followPins(int /*componentId*/, const Point oldPins[kMaxComponentPins],
                         int oldCount, const Point newPins[kMaxComponentPins],
                         int newCount)
{
    if (oldCount != newCount) {
        return;
    }
    for (auto& wire : wires) {
        if (wire.points.size() < 2) {
            continue;
        }
        bool changed = false;
        for (int index : {0, static_cast<int>(wire.points.size()) - 1}) {
            for (int p = 0; p < oldCount; ++p) {
                if (wire.points[static_cast<size_t>(index)] == oldPins[p]) {
                    wire.points[static_cast<size_t>(index)] = newPins[p];
                    changed = true;
                    break;
                }
            }
        }
        if (changed) {
            normalizeWire(wire);
        }
    }
}

namespace {

// Intersection of two segment bodies. Returns false for parallel/collinear
// segments and for intersections that touch an endpoint of either segment,
// because those are handled as real connections elsewhere.
bool segmentBodyCrossing(Point a, Point b, Point c, Point d, Point* out)
{
    const long long rx = b.x - a.x;
    const long long ry = b.y - a.y;
    const long long sx = d.x - c.x;
    const long long sy = d.y - c.y;
    const long long denom = rx * sy - ry * sx;
    if (denom == 0) {
        return false;  // parallel or collinear
    }
    const long long qpx = c.x - a.x;
    const long long qpy = c.y - a.y;
    const long long tNum = qpx * sy - qpy * sx;
    const long long uNum = qpx * ry - qpy * rx;
    const auto interior = [denom](long long value) {
        return denom > 0 ? (value > 0 && value < denom)
                         : (value < 0 && value > denom);
    };
    if (!interior(tNum) || !interior(uNum)) {
        return false;
    }
    if (!out) {
        return true;
    }
    // Wires are axis aligned, so the crossing sits on integer grid points.
    out->x = static_cast<int>(std::lround(
        static_cast<double>(a.x) + static_cast<double>(rx) * static_cast<double>(tNum) /
                                       static_cast<double>(denom)));
    out->y = static_cast<int>(std::lround(
        static_cast<double>(a.y) + static_cast<double>(ry) * static_cast<double>(tNum) /
                                       static_cast<double>(denom)));
    return true;
}

}  // namespace

std::vector<WireCrossing> findWireCrossings(const Circuit& circuit)
{
    std::vector<WireCrossing> crossings;
    for (size_t i = 0; i < circuit.wires.size(); ++i) {
        const Wire& first = circuit.wires[i];
        if (first.points.size() < 2) {
            continue;
        }
        for (size_t j = i + 1; j < circuit.wires.size(); ++j) {
            const Wire& second = circuit.wires[j];
            if (second.points.size() < 2) {
                continue;
            }
            for (size_t si = 0; si + 1 < first.points.size(); ++si) {
                for (size_t sj = 0; sj + 1 < second.points.size(); ++sj) {
                    Point point;
                    if (!segmentBodyCrossing(first.points[si], first.points[si + 1],
                                             second.points[sj], second.points[sj + 1],
                                             &point)) {
                        continue;
                    }
                    WireCrossing crossing;
                    crossing.straightWireId = first.id;
                    crossing.hoppingWireId = second.id;
                    crossing.straightWireIndex = static_cast<int>(i);
                    crossing.hoppingWireIndex = static_cast<int>(j);
                    crossing.point = point;
                    crossings.push_back(crossing);
                }
            }
        }
    }
    return crossings;
}

void Circuit::normalizeWire(Wire& wire)
{
    if (wire.points.size() < 2) {
        return;
    }
    const Point first = wire.points.front();
    const Point last = wire.points.back();
    std::vector<Point> points;
    points.push_back(first);
    if (first.x != last.x && first.y != last.y) {
        points.push_back(Point{last.x, first.y});
    }
    points.push_back(last);

    // Remove consecutive duplicates (including a corner equal to an endpoint).
    std::vector<Point> compact;
    for (const auto& point : points) {
        if (compact.empty() || compact.back() != point) {
            compact.push_back(point);
        }
    }
    wire.points = std::move(compact);
}

}  // namespace circuitlab
