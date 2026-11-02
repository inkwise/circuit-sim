#include "circuit/Netlist.h"

#include "circuit/ComponentCatalog.h"
#include "circuit/SpiceModels.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace circuitlab {
namespace {

struct DisjointSet {
    std::vector<int> parent;
    std::vector<int> size;

    explicit DisjointSet(int count) : parent(static_cast<size_t>(count)), size(static_cast<size_t>(count), 1)
    {
        for (int i = 0; i < count; ++i) {
            parent[static_cast<size_t>(i)] = i;
        }
    }

    int find(int x)
    {
        while (parent[static_cast<size_t>(x)] != x) {
            parent[static_cast<size_t>(x)] = parent[static_cast<size_t>(parent[static_cast<size_t>(x)])];
            x = parent[static_cast<size_t>(x)];
        }
        return x;
    }

    void unite(int a, int b)
    {
        a = find(a);
        b = find(b);
        if (a == b) {
            return;
        }
        if (size[static_cast<size_t>(a)] < size[static_cast<size_t>(b)]) {
            std::swap(a, b);
        }
        parent[static_cast<size_t>(b)] = a;
        size[static_cast<size_t>(a)] += size[static_cast<size_t>(b)];
    }
};

struct PinRecord {
    int componentIndex = 0;
    int componentId = 0;
    int pinIndex = 0;
    Point position;
    int dsuIndex = 0;
};

struct WirePointRecord {
    int wireIndex = 0;
    int wireId = 0;
    int pointIndex = 0;
    Point position;
    int dsuIndex = 0;
};

bool pointOnSegmentInterior(Point point, Point a, Point b)
{
    if (point == a || point == b) {
        return false;
    }
    const long long cross =
        static_cast<long long>(point.x - a.x) * (b.y - a.y) -
        static_cast<long long>(point.y - a.y) * (b.x - a.x);
    if (cross != 0) {
        return false;
    }
    return point.x >= std::min(a.x, b.x) && point.x <= std::max(a.x, b.x) &&
           point.y >= std::min(a.y, b.y) && point.y <= std::max(a.y, b.y);
}

std::string formatNumber(double value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.12g", value);
    return std::string(buffer);
}

std::string sourceLine(const Component& component, int nodeA, int nodeB)
{
    std::ostringstream out;
    out << spiceElementName(component) << ' ' << nodeA << ' ' << nodeB << ' ';
    switch (component.source.mode) {
    case Waveform::DC:
        out << "DC " << formatNumber(component.source.dc);
        break;
    case Waveform::Sine:
        out << "SIN(" << formatNumber(component.source.dc) << ' '
            << formatNumber(component.source.amplitude) << ' '
            << formatNumber(component.source.frequency) << ')';
        break;
    case Waveform::Pulse: {
        double period = component.source.period;
        if (!(period > 0.0)) {
            period = 1.0 / std::max(1e-9, component.source.frequency);
        }
        // The duty cycle is authoritative: pulseWidth is derived from it so the
        // two can never drift apart.
        const double duty = std::clamp(component.source.duty, 0.005, 0.995);
        const double pulseWidth = period * duty;
        const double rise =
            std::clamp(component.source.rise, 1e-12, period * 0.2);
        out << "PULSE(" << formatNumber(component.source.dc) << ' '
            << formatNumber(component.source.amplitude) << " 0 "
            << formatNumber(rise) << ' ' << formatNumber(rise) << ' '
            << formatNumber(pulseWidth) << ' ' << formatNumber(period) << ')';
        break;
    }
    }
    return out.str();
}

std::string lowerCase(std::string text)
{
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

std::string internalPinNode(const Component& component, int pin)
{
    return component.name + "N" + std::to_string(pin);
}

std::string subcircuitName(ComponentType type)
{
    switch (type) {
    case ComponentType::OpAmp:
        return "CL_OPAMP";
    case ComponentType::Timer555:
        return "CL_555";
    case ComponentType::GateAnd:
        return "CL_GATE_AND";
    case ComponentType::GateOr:
        return "CL_GATE_OR";
    case ComponentType::GateNot:
        return "CL_GATE_NOT";
    case ComponentType::Regulator7805:
        return "CL_7805";
    default:
        return {};
    }
}

}  // namespace

std::string pinSenseSourceName(const Component& component, int pin)
{
    return "V" + component.name + "P" + std::to_string(pin);
}

std::string spiceElementName(const Component& component)
{
    // Instruments keep a readable schematic name (SC1) but must be emitted as
    // resistor cards so the first letter selects the right ngspice device.
    if (isProbeType(component.type)) {
        return "R" + component.name;
    }
    return component.name;
}

std::string pinCurrentKey(const Component& component, int pin)
{
    if (needsPinSensing(component.type)) {
        return lowerCase(pinSenseSourceName(component, pin));
    }
    return lowerCase(spiceElementName(component));
}

std::string pinCurrentSaveExpression(const Component& component, int pin)
{
    if (needsPinSensing(component.type)) {
        return "i(" + lowerCase(pinSenseSourceName(component, pin)) + ")";
    }
    // Diodes expose their branch current as [id] in ngspice; resistive and
    // source devices use [i].
    if (componentInfo(component.type).family == SpiceFamily::Diode) {
        return "@" + lowerCase(spiceElementName(component)) + "[id]";
    }
    return "@" + lowerCase(spiceElementName(component)) + "[i]";
}

CompiledCircuit compileCircuit(const Circuit& circuit)
{
    CompiledCircuit result;
    result.groundCount = 0;

    if (circuit.components.empty()) {
        result.error = "电路为空，请先放置元件。";
        return result;
    }

    std::set<std::string> names;
    for (const auto& component : circuit.components) {
        if (component.name.empty() || !names.insert(component.name).second) {
            result.error = "元件名称重复或为空：" + component.name;
            return result;
        }
        if (component.type == ComponentType::Ground) {
            ++result.groundCount;
        }
    }
    if (result.groundCount == 0) {
        result.error = "电路必须包含一个接地（GND）。";
        return result;
    }

    std::vector<PinRecord> pins;
    std::vector<WirePointRecord> wirePoints;
    for (size_t ci = 0; ci < circuit.components.size(); ++ci) {
        const Component& component = circuit.components[ci];
        Point positions[kMaxComponentPins];
        const int count = componentPins(component, positions);
        for (int p = 0; p < count; ++p) {
            PinRecord record;
            record.componentIndex = static_cast<int>(ci);
            record.componentId = component.id;
            record.pinIndex = p;
            record.position = positions[p];
            pins.push_back(record);
        }
    }
    for (size_t wi = 0; wi < circuit.wires.size(); ++wi) {
        const Wire& wire = circuit.wires[wi];
        for (size_t pi = 0; pi < wire.points.size(); ++pi) {
            WirePointRecord record;
            record.wireIndex = static_cast<int>(wi);
            record.wireId = wire.id;
            record.pointIndex = static_cast<int>(pi);
            record.position = wire.points[pi];
            wirePoints.push_back(record);
        }
    }

    const int pinBase = 0;
    const int wireBase = static_cast<int>(pins.size());
    const int total = wireBase + static_cast<int>(wirePoints.size());
    if (total == 0) {
        result.error = "电路没有可连接的节点。";
        return result;
    }

    DisjointSet dsu(total);
    for (size_t i = 0; i < pins.size(); ++i) {
        pins[i].dsuIndex = pinBase + static_cast<int>(i);
    }
    for (size_t i = 0; i < wirePoints.size(); ++i) {
        wirePoints[i].dsuIndex = wireBase + static_cast<int>(i);
    }

    // All vertices of one wire are one conductor.
    for (size_t i = 0; i < circuit.wires.size(); ++i) {
        const Wire& wire = circuit.wires[i];
        if (wire.points.size() < 2) {
            continue;
        }
        int firstIndex = -1;
        for (const auto& wp : wirePoints) {
            if (wp.wireIndex == static_cast<int>(i)) {
                if (firstIndex < 0) {
                    firstIndex = wp.dsuIndex;
                } else {
                    dsu.unite(firstIndex, wp.dsuIndex);
                }
            }
        }
    }

    // Coincident pins/wire vertices are connected.
    std::map<std::pair<int, int>, std::vector<int>> byCoordinate;
    for (const auto& pin : pins) {
        byCoordinate[{pin.position.x, pin.position.y}].push_back(pin.dsuIndex);
    }
    for (const auto& wp : wirePoints) {
        byCoordinate[{wp.position.x, wp.position.y}].push_back(wp.dsuIndex);
    }
    for (auto& entry : byCoordinate) {
        auto& indices = entry.second;
        for (size_t i = 1; i < indices.size(); ++i) {
            dsu.unite(indices[0], indices[i]);
        }
    }

    // A pin touching a wire's interior is a T-junction.
    for (const auto& pin : pins) {
        for (const auto& wire : circuit.wires) {
            for (size_t s = 0; s + 1 < wire.points.size(); ++s) {
                if (pointOnSegmentInterior(pin.position, wire.points[s], wire.points[s + 1])) {
                    for (const auto& wp : wirePoints) {
                        if (wp.wireId == wire.id) {
                            dsu.unite(pin.dsuIndex, wp.dsuIndex);
                            break;
                        }
                    }
                }
            }
        }
    }

    // A wire endpoint touching another wire's interior is also a T-junction.
    for (const auto& wp : wirePoints) {
        for (size_t wi = 0; wi < circuit.wires.size(); ++wi) {
            if (circuit.wires[wi].id == wp.wireId) {
                continue;
            }
            const Wire& other = circuit.wires[wi];
            for (size_t s = 0; s + 1 < other.points.size(); ++s) {
                if (pointOnSegmentInterior(wp.position, other.points[s], other.points[s + 1])) {
                    for (const auto& otherPoint : wirePoints) {
                        if (otherPoint.wireId == other.id) {
                            dsu.unite(wp.dsuIndex, otherPoint.dsuIndex);
                            break;
                        }
                    }
                }
            }
        }
    }

    // Every ground symbol is the global SPICE node 0.
    int groundRoot = -1;
    for (const auto& pin : pins) {
        const Component& component = circuit.components[static_cast<size_t>(pin.componentIndex)];
        if (component.type != ComponentType::Ground) {
            continue;
        }
        const int root = dsu.find(pin.dsuIndex);
        if (groundRoot < 0) {
            groundRoot = root;
        } else {
            dsu.unite(groundRoot, root);
        }
    }
    if (groundRoot < 0) {
        result.error = "电路必须包含一个接地（GND）。";
        return result;
    }
    groundRoot = dsu.find(groundRoot);

    std::map<int, int> rootToNode;
    rootToNode[groundRoot] = 0;
    int nextNode = 1;
    for (int i = 0; i < total; ++i) {
        const int root = dsu.find(i);
        if (rootToNode.find(root) == rootToNode.end()) {
            rootToNode[root] = nextNode++;
        }
    }

    for (const auto& pin : pins) {
        const int node = rootToNode[dsu.find(pin.dsuIndex)];
        ComponentNodes& nodes = result.nodes[pin.componentId];
        if (nodes.count < kMaxComponentPins) {
            nodes.pins[static_cast<size_t>(nodes.count)] = node;
            ++nodes.count;
        }
    }
    for (const auto& wire : circuit.wires) {
        int node = 0;
        for (const auto& wp : wirePoints) {
            if (wp.wireId == wire.id) {
                node = rootToNode[dsu.find(wp.dsuIndex)];
                break;
            }
        }
        result.wireNodes[wire.id] = node;
    }

    // Connection points and dangling-pin statistics.
    std::set<std::pair<int, int>> uniquePoints;
    for (const auto& pin : pins) {
        uniquePoints.insert({pin.position.x, pin.position.y});
    }
    for (const auto& wp : wirePoints) {
        uniquePoints.insert({wp.position.x, wp.position.y});
    }
    for (const auto& entry : uniquePoints) {
        result.connectionPoints.push_back(Point{entry.first, entry.second});
    }

    std::map<int, int> rootPinCount;
    std::map<int, int> rootWirePointCount;
    for (const auto& pin : pins) {
        ++rootPinCount[dsu.find(pin.dsuIndex)];
    }
    for (const auto& wp : wirePoints) {
        ++rootWirePointCount[dsu.find(wp.dsuIndex)];
    }
    for (const auto& pin : pins) {
        const int root = dsu.find(pin.dsuIndex);
        if (rootPinCount[root] == 1 && rootWirePointCount[root] == 0) {
            ++result.danglingPins;
        }
    }

    // Discrete transistors and MOSFETs need every pin connected; otherwise
    // ngspice sees floating internal terminals and fails with a singular
    // matrix. Chips have internal 1G paths and may leave pins unused.
    for (const auto& component : circuit.components) {
        const SpiceFamily family = componentInfo(component.type).family;
        if (family != SpiceFamily::Bjt && family != SpiceFamily::Mos) {
            continue;
        }
        for (const auto& pin : pins) {
            if (pin.componentId != component.id) {
                continue;
            }
            const int root = dsu.find(pin.dsuIndex);
            if (rootPinCount[root] == 1 && rootWirePointCount[root] == 0) {
                result.error = "元件 " + component.name + " 的 " +
                               componentPinName(component.type, pin.pinIndex) +
                               " 脚未连接。";
                return result;
            }
        }
    }

    std::set<int> nodeNumbers;
    for (const auto& entry : result.nodes) {
        for (int i = 0; i < entry.second.count; ++i) {
            const int node = entry.second.pins[static_cast<size_t>(i)];
            if (node > 0) {
                nodeNumbers.insert(node);
            }
        }
    }
    result.nodeNumbers.assign(nodeNumbers.begin(), nodeNumbers.end());

    // A part that is not wired to anything leaves a floating island. An ideal
    // source in such an island makes the matrix singular, so ngspice aborts the
    // analysis and the app used to report the useless "没有产生节点电压".
    {
        std::map<int, int> parent;
        const auto findRoot = [&parent](int node) {
            auto it = parent.find(node);
            if (it == parent.end()) {
                parent.emplace(node, node);
                return node;
            }
            int root = node;
            while (parent[root] != root) {
                root = parent[root];
            }
            while (parent[node] != root) {
                const int next = parent[node];
                parent[node] = root;
                node = next;
            }
            return root;
        };
        const auto unite = [&](int a, int b) {
            a = findRoot(a);
            b = findRoot(b);
            if (a != b) {
                parent[b] = a;
            }
        };
        for (const auto& component : circuit.components) {
            const auto found = result.nodes.find(component.id);
            if (found == result.nodes.end() || found->second.count == 0) {
                continue;
            }
            for (int pin = 1; pin < found->second.count; ++pin) {
                unite(found->second.pins[0], found->second.pins[static_cast<size_t>(pin)]);
            }
        }
        const int groundRoot = findRoot(0);
        std::vector<std::string> island;
        bool islandHasSource = false;
        for (const auto& component : circuit.components) {
            const auto found = result.nodes.find(component.id);
            if (found == result.nodes.end() || found->second.count == 0) {
                continue;
            }
            if (findRoot(found->second.pins[0]) == groundRoot) {
                continue;
            }
            // A terminal block is documentation; it may stay unconnected.
            if (componentInfo(component.type).family == SpiceFamily::Port) {
                continue;
            }
            island.push_back(component.name + "（" + componentInfo(component.type).label + "）");
            if (componentInfo(component.type).family == SpiceFamily::VoltageSource) {
                islandHasSource = true;
            }
        }
        if (islandHasSource) {
            std::string names;
            for (size_t i = 0; i < island.size(); ++i) {
                if (i > 0) {
                    names += "、";
                }
                names += island[i];
            }
            result.error = "元件 " + names +
                           "没有接到电路上（所在的网络没有通向地 GND 的通路），"
                           "理想电源悬空时无法仿真。请把它接到要供电的位置，或者删掉它。";
            return result;
        }
    }

    // Ideal voltage sources must not be shorted or paralleled: ngspice rejects
    // both with a singular matrix, which used to surface as a bare
    // "仿真失败" with no clue for the user.
    std::map<std::pair<int, int>, std::string> sourcePairs;
    for (const auto& component : circuit.components) {
        if (componentInfo(component.type).family != SpiceFamily::VoltageSource) {
            continue;
        }
        const auto found = result.nodes.find(component.id);
        if (found == result.nodes.end() || found->second.count < 2) {
            continue;
        }
        const int nodeA = found->second.a();
        const int nodeB = found->second.b();
        if (nodeA == nodeB) {
            result.error = "元件 " + component.name +
                           " 的两端接在同一个节点上（电压源被短路）。"
                           "请把它的两端分别接到不同的网络：+ 接电源轨或负载，− 接地。";
            return result;
        }
        const std::pair<int, int> key{std::min(nodeA, nodeB), std::max(nodeA, nodeB)};
        const auto inserted = sourcePairs.emplace(key, component.name);
        if (!inserted.second) {
            result.error = "元件 " + inserted.first->second + " 与 " + component.name +
                           " 并联在同一对节点上；理想电压源不能直接并联，"
                           "请删掉其中一个，或在中间串一个小电阻。";
            return result;
        }
    }

    std::ostringstream netlist;
    std::set<std::string> modelCards;
    std::set<std::string> extraModelCards;
    std::set<std::string> subcircuitCards;
    netlist << "CircuitLab generated circuit\n";
    for (const auto& component : circuit.components) {
        if (component.type == ComponentType::Ground) {
            continue;
        }
        const auto found = result.nodes.find(component.id);
        const int requiredPins = pinLayout(component.type).count;
        if (found == result.nodes.end() || found->second.count < requiredPins) {
            result.error = "无法为元件 " + component.name + " 分配节点。";
            return result;
        }
        const ComponentNodes& nodes = found->second;
        const int nodeA = nodes.a();
        const int nodeB = nodes.b();
        switch (componentInfo(component.type).family) {
        case SpiceFamily::Resistor:
            netlist << spiceElementName(component) << ' ' << nodeA << ' ' << nodeB << ' '
                    << formatNumber(componentResistance(component)) << '\n';
            break;
        case SpiceFamily::Capacitor:
        case SpiceFamily::Inductor: {
            // Real filter parts carry a series resistance (ESR / winding
            // resistance). It is emitted as a separate card on an internal
            // node, because ngspice's L and C cards have no Rser parameter.
            const bool series = component.seriesR > 0.0;
            const std::string pairNode =
                series ? component.name + "X" : std::string();
            netlist << spiceElementName(component) << ' ' << nodeA << ' '
                    << (series ? pairNode : std::to_string(nodeB)) << ' '
                    << formatNumber(component.value) << '\n';
            if (series) {
                netlist << 'R' << component.name << ' ' << pairNode << ' '
                        << nodeB << ' ' << formatNumber(component.seriesR) << '\n';
            }
            break;
        }
        case SpiceFamily::VoltageSource:
            netlist << sourceLine(component, nodeA, nodeB) << '\n';
            break;
        case SpiceFamily::Bjt: {
            const PinLayout& layout = pinLayout(component.type);
            for (int pin = 0; pin < nodes.count; ++pin) {
                netlist << pinSenseSourceName(component, pin) << ' '
                        << nodes.pins[static_cast<size_t>(pin)] << ' '
                        << internalPinNode(component, pin) << " 0\n";
            }
            netlist << component.name;
            for (int pin = 0; pin < layout.count; ++pin) {
                netlist << ' ' << internalPinNode(component, pin);
            }
            netlist << ' ' << componentTypeKey(component.type) << '\n';
            modelCards.insert(componentTypeKey(component.type));
            break;
        }
        case SpiceFamily::Diode: {
            std::string model = "D1N4148";
            if (component.type == ComponentType::Led) {
                model = "DLED";
            }
            if (component.type == ComponentType::SchottkyDiode) {
                model = "DSCH";
                modelCards.insert(model);
            } else if (component.type == ComponentType::Zener) {
                model = component.name + "_MOD";
                extraModelCards.insert(
                    zenerModelCard(component.name, component.value));
            } else {
                modelCards.insert(model);
            }
            netlist << spiceElementName(component) << ' ' << nodeA << ' ' << nodeB << ' '
                    << model << '\n';
            break;
        }
        case SpiceFamily::Mos: {
            const PinLayout& layout = pinLayout(component.type);
            for (int pin = 0; pin < nodes.count; ++pin) {
                netlist << pinSenseSourceName(component, pin) << ' '
                        << nodes.pins[static_cast<size_t>(pin)] << ' '
                        << internalPinNode(component, pin) << " 0\n";
            }
            netlist << component.name;
            for (int pin = 0; pin < layout.count; ++pin) {
                netlist << ' ' << internalPinNode(component, pin);
            }
            // The bulk terminal follows the source for the simple three-pin
            // model used by the app.
            std::string model = "M2N7000";
            if (component.type == ComponentType::PowerNmos) {
                model = "MPWR_N";
            } else if (component.type == ComponentType::PowerPmos) {
                model = "MPWR_P";
            }
            netlist << ' ' << internalPinNode(component, 2) << ' ' << model << '\n';
            modelCards.insert(model);
            break;
        }
        case SpiceFamily::Subcircuit: {
            const std::string subckt = subcircuitName(component.type);
            if (subckt.empty() || spiceSubcircuitCard(subckt).empty()) {
                result.error = "元件 " + component.name + " 的模型暂不可用。";
                return result;
            }
            const PinLayout& layout = pinLayout(component.type);
            for (int pin = 0; pin < nodes.count; ++pin) {
                netlist << pinSenseSourceName(component, pin) << ' '
                        << nodes.pins[static_cast<size_t>(pin)] << ' '
                        << internalPinNode(component, pin) << " 0\n";
            }
            netlist << component.name;
            for (int pin = 0; pin < layout.count; ++pin) {
                netlist << ' ' << internalPinNode(component, pin);
            }
            netlist << ' ' << subckt << '\n';
            subcircuitCards.insert(subckt);
            break;
        }
        case SpiceFamily::Ground:
        case SpiceFamily::Port:
            // A ground symbol is the global node 0, and a terminal block only
            // names the nets of its screws; neither adds a SPICE card.
            break;
        }
    }
    for (const std::string& model : modelCards) {
        const std::string card = spiceModelCard(model);
        if (!card.empty()) {
            netlist << card << '\n';
        }
    }
    for (const std::string& card : extraModelCards) {
        netlist << card << '\n';
    }
    for (const std::string& subckt : subcircuitCards) {
        netlist << spiceSubcircuitCard(subckt) << '\n';
    }
    netlist << ".end\n";

    result.netlist = netlist.str();
    result.ok = true;
    return result;
}

std::string generateNetlist(const Circuit& circuit, std::string* error)
{
    const CompiledCircuit compiled = compileCircuit(circuit);
    if (!compiled.ok) {
        if (error) {
            *error = compiled.error;
        }
        return {};
    }
    if (error) {
        error->clear();
    }
    return compiled.netlist;
}

}  // namespace circuitlab
