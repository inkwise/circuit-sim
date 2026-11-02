#include "circuit/Circuit.h"

#include "circuit/Router.h"
#include "circuit/Thermoelectric.h"

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

static bool legacyPinOffsets(ComponentType type, Point* out, int* count);

int componentPins(const Component& component, Point out[kMaxComponentPins])
{
    if (component.legacySymbol) {
        // A part saved before the symbols shrank keeps its old pin positions.
        Point legacy[kMaxComponentPins];
        int legacyCount = 0;
        if (legacyPinOffsets(component.type, legacy, &legacyCount)) {
            const int count = std::min(legacyCount, kMaxComponentPins);
            for (int i = 0; i < count; ++i) {
                const Point rotated = rotateLocal(legacy[i].x, legacy[i].y, component.rotation);
                out[i] = Point{component.gridX + rotated.x, component.gridY + rotated.y};
            }
            return count;
        }
    }
    const PinLayout& layout = pinLayout(component.type);
    const int count = std::min(layout.count, kMaxComponentPins);
    for (int i = 0; i < count; ++i) {
        const PinDef& pin = layout.pins[static_cast<size_t>(i)];
        const Point rotated = rotateLocal(pin.x, pin.y, component.rotation);
        out[i] = Point{component.gridX + rotated.x, component.gridY + rotated.y};
    }
    return count;
}

static bool legacyPinOffsets(ComponentType type, Point* out, int* count)
{
    // Local pin offsets of the previous, larger symbols. Parts that did not
    // change (ground, 4-and-more pin chips) are reported as "no legacy layout".
    static const Point kTwoPin[2] = {{0, -2}, {0, 2}};
    static const Point kTransistor[3] = {{2, -2}, {-2, 0}, {2, 2}};
    static const Point kGate3[3] = {{-3, -1}, {-3, 1}, {3, 0}};
    static const Point kGateNot[2] = {{-3, 0}, {3, 0}};
    static const Point kRegulator[3] = {{-3, 0}, {0, 3}, {3, 0}};
    static const Point kOpAmp[5] = {{-4, -2}, {-4, 2}, {4, 0}, {0, -4}, {0, 4}};
    static const Point kTimer555[8] = {{-4, -3}, {-4, -1}, {-4, 1},  {-4, 3},
                                       {4, 3},   {4, 1},   {4, -1},  {4, -3}};
    const auto assign = [&](const Point* pins, int pinCount) {
        for (int i = 0; i < pinCount; ++i) {
            out[i] = pins[i];
        }
        *count = pinCount;
        return true;
    };
    switch (type) {
    case ComponentType::Resistor:
    case ComponentType::Capacitor:
    case ComponentType::Inductor:
    case ComponentType::VoltageSource:
    case ComponentType::AcSource:
    case ComponentType::SignalSource:
    case ComponentType::Switch:
    case ComponentType::Bulb:
    case ComponentType::Fan:
    case ComponentType::Diode:
    case ComponentType::Led:
    case ComponentType::Zener:
    case ComponentType::SchottkyDiode:
    case ComponentType::PowerInductor:
    case ComponentType::ElectrolyticCap:
    case ComponentType::PwmSource:
    case ComponentType::Oscilloscope:
    case ComponentType::TerminalBlock:
    case ComponentType::Tec12706:
    case ComponentType::Tec12710:
        return assign(kTwoPin, 2);
    case ComponentType::Npn:
    case ComponentType::Pnp:
    case ComponentType::Nmos:
    case ComponentType::PowerNmos:
    case ComponentType::PowerPmos:
        return assign(kTransistor, 3);
    case ComponentType::GateAnd:
    case ComponentType::GateOr:
        return assign(kGate3, 3);
    case ComponentType::GateNot:
        return assign(kGateNot, 2);
    case ComponentType::Regulator7805:
        return assign(kRegulator, 3);
    case ComponentType::OpAmp:
        return assign(kOpAmp, 5);
    case ComponentType::Timer555:
        return assign(kTimer555, 8);
    default:
        return false;
    }
}

void migrateLegacyPinSpacing(Circuit& circuit)
{
    // Wires keep their endpoints where they touched a pin, but the pins have
    // moved: those endpoints are moved onto the new pins, and a wire whose
    // route changed is routed again afterwards (a plain corner could land on
    // another part's pin and silently join two nets).
    const auto pointOnSegment = [](Point point, Point a, Point b) {
        const long long cross = static_cast<long long>(point.x - a.x) * (b.y - a.y) -
                                static_cast<long long>(point.y - a.y) * (b.x - a.x);
        if (cross != 0) {
            return false;
        }
        return point.x >= std::min(a.x, b.x) && point.x <= std::max(a.x, b.x) &&
               point.y >= std::min(a.y, b.y) && point.y <= std::max(a.y, b.y);
    };

    std::vector<std::vector<Point>> before;
    before.reserve(circuit.wires.size());
    for (const auto& wire : circuit.wires) {
        before.push_back(wire.points);
    }

    // Pass 1: wire vertices that sat on an old pin move onto the new pin.
    std::vector<std::vector<Point>> after = before;
    std::vector<bool> changed(before.size(), false);
    for (size_t index = 0; index < after.size(); ++index) {
        for (const auto& component : circuit.components) {
            Point legacy[kMaxComponentPins];
            int legacyCount = 0;
            if (!legacyPinOffsets(component.type, legacy, &legacyCount)) {
                continue;
            }
            Point current[kMaxComponentPins];
            const int count = componentPins(component, current);
            for (int pin = 0; pin < std::min(count, legacyCount); ++pin) {
                const Point rotated =
                    rotateLocal(legacy[pin].x, legacy[pin].y, component.rotation);
                const Point previous{component.gridX + rotated.x,
                                     component.gridY + rotated.y};
                if (previous == current[pin]) {
                    continue;
                }
                for (Point& vertex : after[index]) {
                    if (vertex == previous) {
                        vertex = current[pin];
                        changed[index] = true;
                    }
                }
            }
        }
    }

    // Pass 2: a wire that taps into the middle of another wire has to follow
    // that wire when it moves, otherwise the junction silently disappears.
    struct MovedSegment {
        Point oldA;
        Point oldB;
        int deltaX;
        int deltaY;
    };
    std::vector<MovedSegment> movedSegments;
    for (size_t index = 0; index < before.size(); ++index) {
        if (!changed[index]) {
            continue;
        }
        const size_t segments = std::min(before[index].size(), after[index].size());
        for (size_t s = 0; s + 1 < segments; ++s) {
            const Point oldA = before[index][s];
            const Point oldB = before[index][s + 1];
            const Point newA = after[index][s];
            const Point newB = after[index][s + 1];
            const int deltaX = newA.x - oldA.x;
            const int deltaY = newA.y - oldA.y;
            if (deltaX != newB.x - oldB.x || deltaY != newB.y - oldB.y) {
                continue;  // the segment did not move as a whole
            }
            if (deltaX == 0 && deltaY == 0) {
                continue;
            }
            movedSegments.push_back(MovedSegment{oldA, oldB, deltaX, deltaY});
        }
    }
    if (!movedSegments.empty()) {
        for (size_t index = 0; index < after.size(); ++index) {
            for (Point& vertex : after[index]) {
                bool attachedToPin = false;
                for (const auto& component : circuit.components) {
                    Point pins[kMaxComponentPins];
                    const int count = componentPins(component, pins);
                    for (int pin = 0; pin < count; ++pin) {
                        if (pins[pin] == vertex) {
                            attachedToPin = true;
                        }
                    }
                }
                if (attachedToPin) {
                    continue;
                }
                for (const MovedSegment& segment : movedSegments) {
                    if (pointOnSegment(vertex, segment.oldA, segment.oldB)) {
                        vertex.x += segment.deltaX;
                        vertex.y += segment.deltaY;
                        changed[index] = true;
                        break;
                    }
                }
            }
        }
    }

    std::vector<Wire> kept;
    std::vector<Wire> movedWires;
    for (size_t index = 0; index < circuit.wires.size(); ++index) {
        Wire wire = circuit.wires[index];
        if (!changed[index]) {
            kept.push_back(wire);
            continue;
        }
        wire.points = after[index];
        movedWires.push_back(std::move(wire));
    }
    if (movedWires.empty()) {
        return;
    }

    circuit.wires = std::move(kept);
    for (const Wire& wire : movedWires) {
        const Point start = wire.points.front();
        const Point end = wire.points.back();
        std::vector<Point> route;
        if (!routeWire(circuit, start, end, &route)) {
            route = lShapedRoute(start, end);
        }
        circuit.addWirePoints(std::move(route));
    }

    // Two pins that touched each other were a connection (a ground symbol
    // dropped straight onto a pin, two parts sharing a pin). The smaller
    // symbols no longer reach, so join their new positions with a short wire.
    struct TouchingPins {
        Point was;
        std::vector<Point> now;
    };
    std::vector<TouchingPins> groups;
    for (const auto& component : circuit.components) {
        Point current[kMaxComponentPins];
        const int count = componentPins(component, current);
        Point legacy[kMaxComponentPins];
        int legacyCount = 0;
        const bool haveLegacy = legacyPinOffsets(component.type, legacy, &legacyCount);
        for (int pin = 0; pin < count; ++pin) {
            Point was = current[pin];
            if (haveLegacy && pin < legacyCount) {
                const Point rotated =
                    rotateLocal(legacy[pin].x, legacy[pin].y, component.rotation);
                was = Point{component.gridX + rotated.x, component.gridY + rotated.y};
            }
            auto found = std::find_if(groups.begin(), groups.end(),
                                      [&](const TouchingPins& group) {
                                          return group.was == was;
                                      });
            if (found == groups.end()) {
                groups.push_back(TouchingPins{was, {current[pin]}});
            } else {
                found->now.push_back(current[pin]);
            }
        }
    }
    for (const TouchingPins& group : groups) {
        for (size_t index = 1; index < group.now.size(); ++index) {
            if (group.now[index] == group.now[0]) {
                continue;
            }
            std::vector<Point> route;
            if (!routeWire(circuit, group.now[0], group.now[index], &route)) {
                route = lShapedRoute(group.now[0], group.now[index]);
            }
            circuit.addWirePoints(std::move(route));
        }
    }
    circuit.touch();
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
    return type == ComponentType::Bulb || type == ComponentType::Fan ||
           isThermoelectricType(type);
}

void applyLoadDefaults(Component& component)
{
    if (!isLoadType(component.type)) {
        return;
    }
    if (const ThermoelectricSpec* tec = thermoelectricSpec(component.type)) {
        // A Peltier module is a fixed part: the datasheet spec is not user
        // editable, and exceeding the maximum voltage or current is what
        // destroys it (thermal runaway). The tolerance on the current leaves
        // room for the inrush current of a plate that is still at ambient.
        component.ratedVoltage = tec->ratedVoltage;
        component.ratedCurrent = tec->ratedCurrent;
        component.ratedPower = tec->ratedPower;
        component.minVoltage = std::max(0.5, tec->ratedVoltage * 0.08);
        component.maxVoltage = tec->maxVoltage;
        // A plate that is still at ambient draws Vmax/R at turn-on, which is
        // well above Imax: the datasheet limit is a steady-state one, so the
        // limit here only trips on a genuinely abusive supply.
        component.maxCurrent = tec->maxCurrent * 1.35;
        component.value = component.value > 0.0 ? component.value : 0.0;
        component.heatLoad = std::max(0.0, component.heatLoad);
        if (!(component.coldStart > -273.0) || component.coldStart > 200.0) {
            component.coldStart = thermoelectricHotSide();
        }
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
    syncLoadSpec(component);
}

void syncLoadSpec(Component& component)
{
    if (!isLoadType(component.type)) {
        return;
    }
    if (isThermoelectricType(component.type)) {
        return;  // fixed datasheet part
    }
    if (!(component.ratedVoltage > 0.0)) {
        return;
    }
    if (component.value > 0.0) {
        component.ratedCurrent = component.ratedVoltage / component.value;
        component.ratedPower = component.ratedVoltage * component.ratedCurrent;
    } else {
        component.ratedCurrent = 0.0;
        component.ratedPower = 0.0;
    }
}

void setLoadRatedVoltage(Component& component, double volts)
{
    if (!(volts > 0.0)) {
        return;
    }
    component.ratedVoltage = volts;
    // The filament / winding stays the same, so this only rescales the spec.
    syncLoadSpec(component);
}

void setLoadResistance(Component& component, double ohms)
{
    if (!(ohms > 0.0)) {
        return;
    }
    component.value = ohms;
    syncLoadSpec(component);
}

void setLoadRatedCurrent(Component& component, double amps)
{
    if (!(amps > 0.0) || !(component.ratedVoltage > 0.0)) {
        return;
    }
    component.ratedCurrent = amps;
    component.value = component.ratedVoltage / amps;
    component.ratedPower = component.ratedVoltage * amps;
}

void setLoadRatedPower(Component& component, double watts)
{
    if (!(watts > 0.0) || !(component.ratedVoltage > 0.0)) {
        return;
    }
    component.ratedPower = watts;
    component.ratedCurrent = watts / component.ratedVoltage;
    component.value = component.ratedVoltage / component.ratedCurrent;
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
    if (component.ratedCurrent > 0.0) {
        return component.ratedCurrent;
    }
    const double resistance = component.value > 0.0 ? component.value : 0.0;
    if (!(resistance > 0.0) || !(component.ratedVoltage > 0.0)) {
        return 0.0;
    }
    return component.ratedVoltage / resistance;
}

double loadRatedPower(const Component& component)
{
    if (component.ratedPower > 0.0) {
        return component.ratedPower;
    }
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
    case ComponentType::Tec12706:
    case ComponentType::Tec12710: {
        const ThermoelectricSpec* tec = thermoelectricSpec(component.type);
        component.value = 0.0;
        component.heatLoad = 0.0;
        component.coldStart = thermoelectricHotSide();
        if (tec) {
            component.ratedVoltage = tec->ratedVoltage;
            component.ratedCurrent = tec->ratedCurrent;
            component.ratedPower = tec->ratedPower;
        }
        break;
    }
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
    // Keep the routed shape: a wire may have as many corners as the router
    // needed. Only make sure every segment is orthogonal, then drop the
    // duplicate and collinear points.
    std::vector<Point> points;
    points.reserve(wire.points.size() * 2);
    for (size_t index = 0; index + 1 < wire.points.size(); ++index) {
        const Point a = wire.points[index];
        const Point b = wire.points[index + 1];
        if (points.empty() || points.back() != a) {
            points.push_back(a);
        }
        if (a.x != b.x && a.y != b.y) {
            points.push_back(Point{b.x, a.y});
        }
    }
    points.push_back(wire.points.back());

    std::vector<Point> compact;
    compact.reserve(points.size());
    for (const Point& point : points) {
        if (!compact.empty() && compact.back() == point) {
            continue;
        }
        if (compact.size() >= 2) {
            const Point& previous = compact[compact.size() - 2];
            const Point& last = compact.back();
            const bool sameRow = previous.y == last.y && last.y == point.y;
            const bool sameColumn = previous.x == last.x && last.x == point.x;
            if (sameRow || sameColumn) {
                compact.back() = point;  // Collinear: keep only the far end.
                continue;
            }
        }
        compact.push_back(point);
    }
    wire.points = std::move(compact);
}

}  // namespace circuitlab
