// Headless acceptance tests for the circuit core and the ngspice bridge.
// Built with the Android NDK and run under Termux (see scripts/run-engine-tests.sh).

#include "circuit/Circuit.h"
#include "circuit/ComponentCatalog.h"
#include "circuit/Engineering.h"
#include "circuit/Measure.h"
#include "circuit/Examples.h"
#include "circuit/Flow.h"
#include "circuit/Netlist.h"
#include "circuit/ProjectIO.h"
#include "circuit/Simulation.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

using namespace circuitlab;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message)
{
    if (condition) {
        std::printf("PASS: %s\n", message.c_str());
    } else {
        std::printf("FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

const Component* byName(const Circuit& circuit, const std::string& name)
{
    for (const auto& component : circuit.components) {
        if (component.name == name) {
            return &component;
        }
    }
    return nullptr;
}

int nodeOf(const CompiledCircuit& compiled, const Circuit& circuit, const std::string& name,
           bool positive)
{
    const Component* component = byName(circuit, name);
    if (!component) {
        return -1;
    }
    const auto found = compiled.nodes.find(component->id);
    if (found == compiled.nodes.end()) {
        return -1;
    }
    return positive ? found->second.a() : found->second.b();
}

double valueAtOrNearest(const std::vector<double>& time, const std::vector<double>& values,
                        double target)
{
    if (time.empty() || values.empty()) {
        return std::nan("");
    }
    size_t best = 0;
    double bestDistance = std::fabs(time[0] - target);
    for (size_t i = 1; i < time.size() && i < values.size(); ++i) {
        const double distance = std::fabs(time[i] - target);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return values[best];
}

bool hasCurrent(const SimulationResult& result, const std::string& name, double* magnitude)
{
    const auto found = result.deviceCurrent.find(name);
    if (found == result.deviceCurrent.end() || found->second.empty()) {
        return false;
    }
    if (magnitude) {
        *magnitude = std::fabs(found->second.front());
    }
    return true;
}

// V1 -> switch -> bulb -> back to V1. The bulb and switch share the same
// current; open the switch and the current disappears.
Circuit makeSwitchBulbCircuit()
{
    Circuit circuit;
    const int v1 = circuit.addComponent(ComponentType::VoltageSource, 4, 10);
    if (Component* source = circuit.findComponent(v1)) {
        source->source.mode = Waveform::DC;
        source->source.dc = 5.0;
    }
    const int sw = circuit.addComponent(ComponentType::Switch, 10, 8);
    if (Component* component = circuit.findComponent(sw)) {
        component->rotation = 90;
    }
    const int bulb = circuit.addComponent(ComponentType::Bulb, 14, 10);
    if (Component* component = circuit.findComponent(bulb)) {
        component->value = 10.0;
    }
    circuit.addComponent(ComponentType::Ground, 8, 17);

    circuit.addWirePoints({Point{4, 8}, Point{8, 8}});
    circuit.addWirePoints({Point{12, 8}, Point{14, 8}});
    circuit.addWirePoints({Point{14, 12}, Point{4, 12}});
    circuit.addWirePoints({Point{8, 12}, Point{8, 16}});
    return circuit;
}

void testDividerNetlist()
{
    const Circuit circuit = makeDividerDemo();
    const CompiledCircuit compiled = compileCircuit(circuit);
    std::printf("---- divider netlist ----\n%s-------------------------\n", compiled.netlist.c_str());
    check(compiled.ok, "divider netlist compiles");
    check(compiled.error.empty(), "divider has no compile error");
    check(compiled.groundCount == 1, "divider has one ground");

    const int v1Top = nodeOf(compiled, circuit, "V1", true);
    const int v1Bottom = nodeOf(compiled, circuit, "V1", false);
    const int r1Bottom = nodeOf(compiled, circuit, "R1", false);
    const int r2Top = nodeOf(compiled, circuit, "R2", true);
    check(v1Bottom == 0, "V1 negative pin is ground");
    check(v1Top > 0, "V1 positive pin has a non-ground node");
    check(r1Bottom == r2Top, "R1/R2 junction shares one node");
    check(r1Bottom != v1Top, "R1/R2 junction differs from source node");
}

void testDividerOperatingPoint()
{
    NgspiceEngine engine;
    std::string error;
    check(engine.initialize(error), "ngspice initializes: " + error);

    const Circuit circuit = makeDividerDemo();
    const CompiledCircuit compiled = compileCircuit(circuit);
    const int sourceNode = nodeOf(compiled, circuit, "V1", true);
    const int middleNode = nodeOf(compiled, circuit, "R1", false);

    SimulationRequest request;
    request.circuit = circuit;
    request.kind = AnalysisKind::OperatingPoint;
    request.revision = 7;
    SimulationResult result = engine.runAsync(std::move(request)).get();

    if (!result.ok) {
        std::printf("---- ngspice log ----\n%s\n--------------------\n", result.log.c_str());
    }
    check(result.ok, "divider operating point runs");
    check(result.revision == 7, "result carries the circuit revision");
    check(result.nodeVoltage.find(sourceNode) != result.nodeVoltage.end(),
          "source node vector is present");
    check(result.nodeVoltage.find(middleNode) != result.nodeVoltage.end(),
          "middle node vector is present");
    if (result.nodeVoltage.find(sourceNode) != result.nodeVoltage.end()) {
        const double source = result.nodeVoltage[sourceNode].front();
        check(std::fabs(source - 5.0) < 0.001, "source node is 5 V");
    }
    if (result.nodeVoltage.find(middleNode) != result.nodeVoltage.end()) {
        const double middle = result.nodeVoltage[middleNode].front();
        std::printf("divider middle node = %s\n", formatSI(middle, "V").c_str());
        check(std::fabs(middle - 2.5) < 0.001, "divider middle node is 2.5 V");
    }
}

void testRcTransient()
{
    NgspiceEngine engine;
    std::string error;
    check(engine.initialize(error), "ngspice initializes for transient: " + error);

    const Circuit circuit = makeRcDemo();
    const CompiledCircuit compiled = compileCircuit(circuit);
    const int rcNode = nodeOf(compiled, circuit, "R1", false);

    SimulationRequest request;
    request.circuit = circuit;
    request.kind = AnalysisKind::Transient;
    request.transient.step = 10e-6;
    request.transient.stop = 5e-3;
    request.revision = 9;
    SimulationResult result = engine.runAsync(std::move(request)).get();

    if (!result.ok) {
        std::printf("---- ngspice RC log ----\n%s\n------------------------\n", result.log.c_str());
    }
    check(result.ok, "RC transient runs");
    check(!result.time.empty(), "RC transient has a time vector");
    check(result.nodeVoltage.find(rcNode) != result.nodeVoltage.end(),
          "RC node vector is present");

    if (result.nodeVoltage.find(rcNode) != result.nodeVoltage.end()) {
        const auto& values = result.nodeVoltage[rcNode];
        const double atTau = valueAtOrNearest(result.time, values, 1e-3);
        const double finalValue = values.empty() ? 0.0 : values.back();
        std::printf("RC at 1 ms = %s, final = %s, points = %zu\n",
                    formatSI(atTau, "V").c_str(), formatSI(finalValue, "V").c_str(),
                    result.time.size());
        check(atTau > 3.10 && atTau < 3.22, "RC reaches about 3.16 V at 1 ms");
        check(finalValue > 4.8, "RC approaches 5 V at the end");
    }
}

void testSwitchBulbCurrents()
{
    NgspiceEngine engine;
    std::string error;
    check(engine.initialize(error), "ngspice initializes for switch/bulb: " + error);

    Circuit circuit = makeSwitchBulbCircuit();
    const CompiledCircuit compiled = compileCircuit(circuit);
    check(compiled.ok, "switch/bulb netlist compiles");
    std::printf("---- switch/bulb netlist ----\n%s-----------------------------\n",
                compiled.netlist.c_str());

    SimulationRequest request;
    request.circuit = circuit;
    request.kind = AnalysisKind::OperatingPoint;
    request.revision = 11;
    SimulationResult result = engine.runAsync(std::move(request)).get();
    if (!result.ok) {
        std::printf("---- switch/bulb log ----\n%s\n-------------------------\n",
                    result.log.c_str());
    }
    check(result.ok, "switch/bulb operating point runs");

    double switchCurrent = 0.0;
    double bulbCurrent = 0.0;
    check(hasCurrent(result, "r1", &switchCurrent), "switch branch current is present");
    check(hasCurrent(result, "r2", &bulbCurrent), "bulb branch current is present");
    std::printf("switch current = %s, bulb current = %s\n",
                formatSI(switchCurrent, "A").c_str(), formatSI(bulbCurrent, "A").c_str());
    check(std::fabs(bulbCurrent - 5.0 / 10.001) < 0.01,
          "closed switch drives about 0.5 A through the bulb");
    check(std::fabs(switchCurrent - bulbCurrent) < 0.01,
          "switch and bulb carry the same current");

    const int bulbTop = nodeOf(compiled, circuit, "R2", true);
    if (result.nodeVoltage.find(bulbTop) != result.nodeVoltage.end()) {
        check(std::fabs(result.nodeVoltage[bulbTop].front() - 5.0) < 0.05,
              "bulb sees almost the full 5 V");
    }

    for (auto& component : circuit.components) {
        if (component.type == ComponentType::Switch) {
            component.switchClosed = false;
        }
    }
    SimulationRequest openRequest;
    openRequest.circuit = circuit;
    openRequest.kind = AnalysisKind::OperatingPoint;
    openRequest.revision = 12;
    SimulationResult openResult = engine.runAsync(std::move(openRequest)).get();
    double openCurrent = 0.0;
    check(openResult.ok, "open-switch operating point runs");
    check(hasCurrent(openResult, "r2", &openCurrent), "open-switch bulb current is present");
    std::printf("open-switch bulb current = %s\n", formatSI(openCurrent, "A").c_str());
    check(openCurrent < 1e-6, "open switch blocks the current");
}

void testAcSignalCurrents()
{
    NgspiceEngine engine;
    std::string error;
    check(engine.initialize(error), "ngspice initializes for AC: " + error);

    Circuit circuit;
    const int vac = circuit.addComponent(ComponentType::AcSource, 4, 10);
    if (Component* source = circuit.findComponent(vac)) {
        source->source.mode = Waveform::Sine;
        source->source.dc = 0.0;
        source->source.amplitude = 10.0;
        source->source.frequency = 1000.0;
    }
    const int load = circuit.addComponent(ComponentType::Resistor, 14, 10);
    if (Component* resistor = circuit.findComponent(load)) {
        resistor->value = 1000.0;
    }
    circuit.addComponent(ComponentType::Ground, 8, 17);
    circuit.addWirePoints({Point{4, 8}, Point{14, 8}});
    circuit.addWirePoints({Point{14, 12}, Point{4, 12}});
    circuit.addWirePoints({Point{8, 12}, Point{8, 16}});

    SimulationRequest request;
    request.circuit = circuit;
    request.kind = AnalysisKind::Transient;
    request.transient.step = 10e-6;
    request.transient.stop = 2e-3;
    request.revision = 13;
    SimulationResult result = engine.runAsync(std::move(request)).get();
    if (!result.ok) {
        std::printf("---- AC log ----\n%s\n-------------\n", result.log.c_str());
    }
    check(result.ok, "AC transient runs");
    check(!result.time.empty(), "AC transient has a time axis");
    const auto sourceCurrent = result.deviceCurrent.find("v1");
    const auto loadCurrent = result.deviceCurrent.find("r1");
    check(sourceCurrent != result.deviceCurrent.end() && !sourceCurrent->second.empty(),
          "AC source current is present");
    check(loadCurrent != result.deviceCurrent.end() && !loadCurrent->second.empty(),
          "AC load current is present");
    if (loadCurrent != result.deviceCurrent.end() && !loadCurrent->second.empty()) {
        double peak = 0.0;
        for (double value : loadCurrent->second) {
            peak = std::max(peak, std::fabs(value));
        }
        std::printf("AC load peak current = %s\n", formatSI(peak, "A").c_str());
        check(peak > 0.008 && peak < 0.012, "AC load current peaks near 10 mA");
    }
}

void testMultiPinLayout()
{
    Circuit circuit;
    const int transistor = circuit.addComponent(ComponentType::Npn, 10, 10);
    const Component* component = circuit.findComponent(transistor);
    check(component != nullptr, "BJT component is created");
    if (!component) {
        return;
    }

    Point pins[kMaxComponentPins];
    int count = componentPins(*component, pins);
    check(count == 3, "BJT has three pins");
    check(pins[0] == Point{12, 8}, "BJT collector pin is at the top right");
    check(pins[1] == Point{8, 10}, "BJT base pin is on the left");
    check(pins[2] == Point{12, 12}, "BJT emitter pin is at the bottom right");

    check(circuit.rotateComponent(transistor), "BJT rotates");
    count = componentPins(*circuit.findComponent(transistor), pins);
    check(count == 3, "rotated BJT still has three pins");
    check(pins[0] == Point{12, 12}, "rotated collector pin follows the rotation");
    check(pins[1] == Point{10, 8}, "rotated base pin follows the rotation");
    check(pins[2] == Point{8, 12}, "rotated emitter pin follows the rotation");

    const int timer = circuit.addComponent(ComponentType::Timer555, 20, 10);
    Point timerPins[kMaxComponentPins];
    const int timerCount = componentPins(*circuit.findComponent(timer), timerPins);
    check(timerCount == 8, "555 has eight pins");
    bool distinct = true;
    for (int a = 0; a < timerCount; ++a) {
        for (int b = a + 1; b < timerCount; ++b) {
            if (timerPins[a] == timerPins[b]) {
                distinct = false;
            }
        }
    }
    check(distinct, "555 pins are all distinct");

    Point boundsMin;
    Point boundsMax;
    componentBounds(*circuit.findComponent(timer), &boundsMin, &boundsMax);
    bool inside = true;
    for (int pin = 0; pin < timerCount; ++pin) {
        if (timerPins[pin].x < boundsMin.x || timerPins[pin].x > boundsMax.x ||
            timerPins[pin].y < boundsMin.y || timerPins[pin].y > boundsMax.y) {
            inside = false;
        }
    }
    check(inside, "component bounds contain every pin");
}

void testNpnSwitchAndPinCurrents()
{
    NgspiceEngine engine;
    std::string error;
    check(engine.initialize(error), "ngspice initializes for BJT: " + error);

    Circuit circuit;
    const int source = circuit.addComponent(ComponentType::VoltageSource, 4, 10);
    if (Component* component = circuit.findComponent(source)) {
        component->source.mode = Waveform::DC;
        component->source.dc = 5.0;
        component->source.amplitude = 5.0;
    }
    const int baseResistor = circuit.addComponent(ComponentType::Resistor, 8, 6);
    if (Component* component = circuit.findComponent(baseResistor)) {
        component->value = 10e3;
    }
    const int collectorResistor = circuit.addComponent(ComponentType::Resistor, 18, 6);
    if (Component* component = circuit.findComponent(collectorResistor)) {
        component->value = 100.0;
    }
    const int transistor = circuit.addComponent(ComponentType::Npn, 10, 10);
    circuit.addComponent(ComponentType::Ground, 12, 13);

    circuit.addWirePoints({Point{4, 8}, Point{4, 4}});
    circuit.addWirePoints({Point{4, 4}, Point{18, 4}});
    circuit.addWirePoints({Point{8, 8}, Point{8, 10}});
    circuit.addWirePoints({Point{18, 8}, Point{12, 8}});
    circuit.addWirePoints({Point{4, 12}, Point{12, 12}});

    const CompiledCircuit compiled = compileCircuit(circuit);
    check(compiled.ok, "BJT netlist compiles: " + compiled.error);
    check(compiled.netlist.find("Q1 ") != std::string::npos,
          "BJT device line is generated");
    check(compiled.netlist.find(".model QNPN") != std::string::npos,
          "NPN model card is generated");
    check(compiled.netlist.find("VQ1P0") != std::string::npos,
          "BJT pins get sense sources");

    SimulationRequest request;
    request.circuit = circuit;
    request.kind = AnalysisKind::OperatingPoint;
    const SimulationResult result = engine.runAsync(std::move(request)).get();
    check(result.ok, "BJT operating point runs: " + result.error);

    const auto pins = result.pinCurrent.find(transistor);
    check(pins != result.pinCurrent.end() && pins->second.size() == 3,
          "BJT exposes three pin currents");
    const bool haveAllPins =
        pins != result.pinCurrent.end() && pins->second.size() == 3 &&
        !pins->second[0].empty() && !pins->second[1].empty() &&
        !pins->second[2].empty();
    check(haveAllPins, "BJT pin current vectors are present");
    if (haveAllPins) {
        const double ic = pins->second[0].front();
        const double ib = pins->second[1].front();
        const double ie = pins->second[2].front();
        std::printf("NPN pin currents: Ic=%.6g A Ib=%.6g A Ie=%.6g A\n", ic, ib,
                    ie);
        check(ic > 0.01, "NPN collector current flows into the device");
        check(ib > 1e-6 && ib < 1e-3, "NPN base current is small and positive");
        check(ie < -0.01, "NPN emitter current flows out of the device");
        check(std::fabs(ic + ib + ie) < 1e-6,
              "NPN pin currents satisfy Kirchhoff");
    }

    const Component* transistorComponent = circuit.findComponent(transistor);
    if (transistorComponent) {
        const auto nodes = compiled.nodes.find(transistor);
        if (nodes != compiled.nodes.end() && nodes->second.count == 3) {
            const auto collector = result.nodeVoltage.find(nodes->second.pins[0]);
            const auto emitter = result.nodeVoltage.find(nodes->second.pins[2]);
            if (collector != result.nodeVoltage.end() &&
                emitter != result.nodeVoltage.end() &&
                !collector->second.empty() && !emitter->second.empty()) {
                const double vce = collector->second.front() - emitter->second.front();
                std::printf("NPN Vce = %.4f V\n", vce);
                check(vce > 0.0 && vce < 1.0, "NPN is saturated in the test circuit");
            }
        }
    }
}

void testDiodeForwardBias()
{
    NgspiceEngine engine;
    std::string error;
    check(engine.initialize(error), "ngspice initializes for diode: " + error);

    Circuit circuit;
    const int source = circuit.addComponent(ComponentType::VoltageSource, 4, 10);
    if (Component* component = circuit.findComponent(source)) {
        component->source.mode = Waveform::DC;
        component->source.dc = 5.0;
    }
    const int resistor = circuit.addComponent(ComponentType::Resistor, 8, 6);
    if (Component* component = circuit.findComponent(resistor)) {
        component->value = 1000.0;
    }
    const int diode = circuit.addComponent(ComponentType::Diode, 12, 10);
    circuit.addComponent(ComponentType::Ground, 12, 13);

    circuit.addWirePoints({Point{4, 8}, Point{4, 4}});
    circuit.addWirePoints({Point{4, 4}, Point{8, 4}});
    circuit.addWirePoints({Point{8, 8}, Point{12, 8}});
    circuit.addWirePoints({Point{4, 12}, Point{12, 12}});

    const CompiledCircuit compiled = compileCircuit(circuit);
    check(compiled.ok, "diode netlist compiles: " + compiled.error);
    check(compiled.netlist.find("D1 ") != std::string::npos,
          "diode device line is generated");
    check(compiled.netlist.find(".model D1N4148") != std::string::npos,
          "diode model card is generated");

    SimulationRequest request;
    request.circuit = circuit;
    request.kind = AnalysisKind::OperatingPoint;
    const SimulationResult result = engine.runAsync(std::move(request)).get();
    check(result.ok, "diode operating point runs: " + result.error);

    const auto pins = result.pinCurrent.find(diode);
    const bool havePins = pins != result.pinCurrent.end() && pins->second.size() == 2 &&
                          !pins->second[0].empty() && !pins->second[1].empty();
    check(havePins, "diode exposes two pin currents");
    if (havePins) {
        const double forward = pins->second[0].front();
        const double returnCurrent = pins->second[1].front();
        std::printf("diode current = %.6g A\n", forward);
        check(forward > 0.003 && forward < 0.006,
              "diode conducts about 4 mA at 5 V through 1 kΩ");
        check(std::fabs(forward + returnCurrent) < 1e-9,
              "diode pin currents have opposite signs");
    }
}

void testNmosSwitch()
{
    NgspiceEngine engine;
    std::string error;
    check(engine.initialize(error), "ngspice initializes for MOS: " + error);

    Circuit circuit;
    const int source = circuit.addComponent(ComponentType::VoltageSource, 4, 10);
    if (Component* component = circuit.findComponent(source)) {
        component->source.mode = Waveform::DC;
        component->source.dc = 5.0;
    }
    const int gateResistor = circuit.addComponent(ComponentType::Resistor, 8, 6);
    if (Component* component = circuit.findComponent(gateResistor)) {
        component->value = 10e3;
    }
    const int drainResistor = circuit.addComponent(ComponentType::Resistor, 18, 6);
    if (Component* component = circuit.findComponent(drainResistor)) {
        component->value = 100.0;
    }
    const int transistor = circuit.addComponent(ComponentType::Nmos, 10, 10);
    circuit.addComponent(ComponentType::Ground, 12, 13);

    circuit.addWirePoints({Point{4, 8}, Point{4, 4}});
    circuit.addWirePoints({Point{4, 4}, Point{18, 4}});
    circuit.addWirePoints({Point{8, 8}, Point{8, 10}});
    circuit.addWirePoints({Point{18, 8}, Point{12, 8}});
    circuit.addWirePoints({Point{4, 12}, Point{12, 12}});

    const CompiledCircuit compiled = compileCircuit(circuit);
    check(compiled.ok, "MOS netlist compiles: " + compiled.error);
    check(compiled.netlist.find("M1 ") != std::string::npos,
          "MOS device line is generated");

    SimulationRequest request;
    request.circuit = circuit;
    request.kind = AnalysisKind::OperatingPoint;
    const SimulationResult result = engine.runAsync(std::move(request)).get();
    check(result.ok, "MOS operating point runs: " + result.error);
    if (!result.ok) {
        std::printf("MOS netlist:\n%s\nMOS log:\n%s\n", compiled.netlist.c_str(),
                    result.log.c_str());
    }

    const auto pins = result.pinCurrent.find(transistor);
    const bool havePins = pins != result.pinCurrent.end() && pins->second.size() == 3 &&
                          !pins->second[0].empty() && !pins->second[1].empty() &&
                          !pins->second[2].empty();
    check(havePins, "MOS exposes three pin currents");
    if (havePins) {
        const double drain = pins->second[0].front();
        const double gate = pins->second[1].front();
        const double sourceCurrent = pins->second[2].front();
        std::printf("MOS currents: Id=%.6g A Ig=%.3g A Is=%.6g A\n", drain, gate,
                    sourceCurrent);
        check(drain > 0.01, "MOS drain current flows into the device");
        check(std::fabs(gate) < 1e-9, "MOS gate current is negligible");
        check(sourceCurrent < -0.01, "MOS source current flows out of the device");
    }
}

void testOpAmpFollower()
{
    NgspiceEngine engine;
    std::string error;
    check(engine.initialize(error), "ngspice initializes for op-amp: " + error);

    Circuit circuit;
    const int input = circuit.addComponent(ComponentType::VoltageSource, 4, 10);
    if (Component* component = circuit.findComponent(input)) {
        component->source.mode = Waveform::DC;
        component->source.dc = 2.0;
    }
    const int supply = circuit.addComponent(ComponentType::VoltageSource, 20, 10);
    if (Component* component = circuit.findComponent(supply)) {
        component->source.mode = Waveform::DC;
        component->source.dc = 5.0;
    }
    const int opamp = circuit.addComponent(ComponentType::OpAmp, 12, 10);
    const int outputLoad = circuit.addComponent(ComponentType::Resistor, 20, 10);
    if (Component* component = circuit.findComponent(outputLoad)) {
        component->value = 10e3;
        component->rotation = 90;
    }
    circuit.addComponent(ComponentType::Ground, 12, 19);

    // Input source to IN+.
    circuit.addWirePoints({Point{4, 8}, Point{8, 8}});
    // Supply positive rail to V+.
    circuit.addWirePoints({Point{20, 8}, Point{12, 8}});
    circuit.addWirePoints({Point{12, 8}, Point{12, 6}});
    // Ground bus below the symbol, away from the IN- pin and the feedback.
    circuit.addWirePoints({Point{4, 12}, Point{4, 18}});
    circuit.addWirePoints({Point{20, 12}, Point{20, 18}});
    circuit.addWirePoints({Point{4, 18}, Point{12, 18}});
    circuit.addWirePoints({Point{20, 18}, Point{12, 18}});
    circuit.addWirePoints({Point{12, 14}, Point{12, 18}});
    // Feedback below the symbol: IN- -> OUT.
    circuit.addWirePoints({Point{8, 12}, Point{8, 16}});
    circuit.addWirePoints({Point{8, 16}, Point{18, 16}});
    circuit.addWirePoints({Point{18, 16}, Point{18, 10}});
    circuit.addWirePoints({Point{18, 10}, Point{16, 10}});
    // Output load to ground.
    circuit.addWirePoints({Point{16, 10}, Point{18, 10}});
    circuit.addWirePoints({Point{22, 10}, Point{22, 14}});
    circuit.addWirePoints({Point{22, 14}, Point{12, 14}});

    const CompiledCircuit compiled = compileCircuit(circuit);
    check(compiled.ok, "op-amp netlist compiles: " + compiled.error);
    check(compiled.netlist.find(".subckt CL_OPAMP") != std::string::npos,
          "op-amp subcircuit card is generated");

    SimulationRequest request;
    request.circuit = circuit;
    request.kind = AnalysisKind::OperatingPoint;
    const SimulationResult result = engine.runAsync(std::move(request)).get();
    check(result.ok, "op-amp operating point runs: " + result.error);
    if (!result.ok) {
        std::printf("op-amp log:\n%s\n", result.log.c_str());
    }

    const auto nodes = compiled.nodes.find(opamp);
    check(nodes != compiled.nodes.end() && nodes->second.count == 5,
          "op-amp compiles with five pin nodes");
    if (result.ok && nodes != compiled.nodes.end() && nodes->second.count == 5) {
        const auto entry = result.nodeVoltage.find(nodes->second.pins[2]);
        check(entry != result.nodeVoltage.end() && !entry->second.empty(),
              "op-amp output node voltage is saved");
        if (entry != result.nodeVoltage.end() && !entry->second.empty()) {
            std::printf("op-amp follower output = %.4f V\n", entry->second.front());
            check(std::fabs(entry->second.front() - 2.0) < 0.05,
                  "op-amp follower output follows the input");
        }
    }
}

void testLogicGate()
{
    NgspiceEngine engine;
    std::string error;
    check(engine.initialize(error), "ngspice initializes for logic gate: " + error);

    Circuit circuit;
    const int source = circuit.addComponent(ComponentType::VoltageSource, 2, 10);
    if (Component* component = circuit.findComponent(source)) {
        component->source.mode = Waveform::DC;
        component->source.dc = 5.0;
    }
    const int gate = circuit.addComponent(ComponentType::GateAnd, 10, 10);
    const int load = circuit.addComponent(ComponentType::Resistor, 16, 10);
    if (Component* component = circuit.findComponent(load)) {
        component->value = 1000.0;
        component->rotation = 90;
    }
    circuit.addComponent(ComponentType::Ground, 2, 13);
    circuit.addComponent(ComponentType::Ground, 18, 11);

    // Both gate inputs tied to the 5 V source.
    circuit.addWirePoints({Point{2, 8}, Point{4, 8}});
    circuit.addWirePoints({Point{4, 8}, Point{7, 8}});
    circuit.addWirePoints({Point{7, 8}, Point{7, 9}});
    circuit.addWirePoints({Point{4, 8}, Point{4, 11}});
    circuit.addWirePoints({Point{4, 11}, Point{7, 11}});
    // Output to the load.
    circuit.addWirePoints({Point{13, 10}, Point{14, 10}});

    const CompiledCircuit compiled = compileCircuit(circuit);
    check(compiled.ok, "logic gate netlist compiles: " + compiled.error);
    check(compiled.netlist.find(".subckt CL_GATE_AND") != std::string::npos,
          "logic gate subcircuit card is generated");

    SimulationRequest request;
    request.circuit = circuit;
    request.kind = AnalysisKind::OperatingPoint;
    const SimulationResult result = engine.runAsync(std::move(request)).get();
    check(result.ok, "logic gate operating point runs: " + result.error);
    if (!result.ok) {
        std::printf("logic gate log:\n%s\n", result.log.c_str());
    }

    const auto nodes = compiled.nodes.find(gate);
    if (result.ok && nodes != compiled.nodes.end() && nodes->second.count == 3) {
        const auto entry = result.nodeVoltage.find(nodes->second.pins[2]);
        if (entry != result.nodeVoltage.end() && !entry->second.empty()) {
            std::printf("AND gate output = %.4f V\n", entry->second.front());
            check(entry->second.front() > 4.5,
                  "AND gate outputs high when both inputs are high");
        }
    }
}

void testRegulator7805()
{
    NgspiceEngine engine;
    std::string error;
    check(engine.initialize(error), "ngspice initializes for 7805: " + error);

    Circuit circuit;
    const int source = circuit.addComponent(ComponentType::VoltageSource, 4, 10);
    if (Component* component = circuit.findComponent(source)) {
        component->source.mode = Waveform::DC;
        component->source.dc = 12.0;
    }
    const int regulator = circuit.addComponent(ComponentType::Regulator7805, 12, 10);
    const int load = circuit.addComponent(ComponentType::Resistor, 18, 10);
    if (Component* component = circuit.findComponent(load)) {
        component->value = 100.0;
        component->rotation = 90;
    }
    circuit.addComponent(ComponentType::Ground, 4, 13);
    circuit.addComponent(ComponentType::Ground, 12, 14);
    circuit.addComponent(ComponentType::Ground, 20, 11);

    circuit.addWirePoints({Point{4, 8}, Point{9, 8}});
    circuit.addWirePoints({Point{9, 8}, Point{9, 10}});
    circuit.addWirePoints({Point{15, 10}, Point{16, 10}});

    const CompiledCircuit compiled = compileCircuit(circuit);
    check(compiled.ok, "7805 netlist compiles: " + compiled.error);
    check(compiled.netlist.find(".subckt CL_7805") != std::string::npos,
          "7805 subcircuit card is generated");

    SimulationRequest request;
    request.circuit = circuit;
    request.kind = AnalysisKind::OperatingPoint;
    const SimulationResult result = engine.runAsync(std::move(request)).get();
    check(result.ok, "7805 operating point runs: " + result.error);
    if (!result.ok) {
        std::printf("7805 log:\n%s\n", result.log.c_str());
    }

    const auto nodes = compiled.nodes.find(regulator);
    if (result.ok && nodes != compiled.nodes.end() && nodes->second.count == 3) {
        const auto entry = result.nodeVoltage.find(nodes->second.pins[2]);
        if (entry != result.nodeVoltage.end() && !entry->second.empty()) {
            std::printf("7805 output = %.4f V\n", entry->second.front());
            check(std::fabs(entry->second.front() - 5.0) < 0.15,
                  "7805 regulates the output to about 5 V");
        }
        const auto pins = result.pinCurrent.find(regulator);
        if (pins != result.pinCurrent.end() && pins->second.size() == 3 &&
            !pins->second[0].empty() && !pins->second[2].empty()) {
            std::printf("7805 currents: Iin=%.3f A Iout=%.3f A\n",
                        pins->second[0].front(), pins->second[2].front());
            check(pins->second[0].front() > 0.03,
                  "7805 input current supplies the load");
            check(pins->second[2].front() < -0.03,
                  "7805 output current flows to the load");
        }
    }
}

void testTimer555Astable()
{
    NgspiceEngine engine;
    std::string error;
    check(engine.initialize(error), "ngspice initializes for 555: " + error);

    Circuit circuit;
    const int source = circuit.addComponent(ComponentType::VoltageSource, 4, 10);
    if (Component* component = circuit.findComponent(source)) {
        component->source.mode = Waveform::DC;
        component->source.dc = 5.0;
    }
    const int timer = circuit.addComponent(ComponentType::Timer555, 20, 10);

    const int ra = circuit.addComponent(ComponentType::Resistor, 28, 7);
    if (Component* component = circuit.findComponent(ra)) {
        component->value = 1000.0;
    }
    const int rb = circuit.addComponent(ComponentType::Resistor, 26, 11);
    if (Component* component = circuit.findComponent(rb)) {
        component->value = 1000.0;
        component->rotation = 90;
    }
    const int timingCap = circuit.addComponent(ComponentType::Capacitor, 22, 16);
    if (Component* component = circuit.findComponent(timingCap)) {
        component->value = 1e-6;
    }
    const int load = circuit.addComponent(ComponentType::Resistor, 10, 11);
    if (Component* component = circuit.findComponent(load)) {
        component->value = 10e3;
        component->rotation = 90;
    }

    // Grounds: VCC source, 555 GND pin, timing capacitor and output load.
    circuit.addComponent(ComponentType::Ground, 4, 13);
    circuit.addComponent(ComponentType::Ground, 16, 6);
    circuit.addComponent(ComponentType::Ground, 22, 19);
    circuit.addComponent(ComponentType::Ground, 8, 12);

    // Power rail and RESET pull-up.
    circuit.addWirePoints({Point{4, 8}, Point{4, 4}});
    circuit.addWirePoints({Point{4, 4}, Point{30, 4}});
    circuit.addWirePoints({Point{24, 4}, Point{24, 7}});
    circuit.addWirePoints({Point{16, 13}, Point{16, 20}});
    circuit.addWirePoints({Point{16, 20}, Point{30, 20}});
    circuit.addWirePoints({Point{30, 20}, Point{30, 4}});
    // GND pin.
    circuit.addWirePoints({Point{16, 7}, Point{16, 5}});

    // RA from VCC to DISCH, RB from DISCH to THRES.
    circuit.addWirePoints({Point{28, 4}, Point{28, 5}});
    circuit.addWirePoints({Point{28, 9}, Point{24, 9}});
    circuit.addWirePoints({Point{28, 9}, Point{28, 11}});

    // Timing capacitor from THRES to ground, and TRIG tied to THRES.
    circuit.addWirePoints({Point{24, 11}, Point{22, 11}});
    circuit.addWirePoints({Point{22, 11}, Point{22, 14}});
    circuit.addWirePoints({Point{16, 9}, Point{14, 9}});
    circuit.addWirePoints({Point{14, 9}, Point{14, 6}});
    circuit.addWirePoints({Point{14, 6}, Point{26, 6}});
    circuit.addWirePoints({Point{26, 6}, Point{26, 8}});
    circuit.addWirePoints({Point{26, 8}, Point{22, 8}});
    circuit.addWirePoints({Point{22, 8}, Point{22, 11}});

    // Output load.
    circuit.addWirePoints({Point{16, 11}, Point{12, 11}});

    const CompiledCircuit compiled = compileCircuit(circuit);
    check(compiled.ok, "555 netlist compiles: " + compiled.error);
    check(compiled.netlist.find(".subckt CL_555") != std::string::npos,
          "555 subcircuit card is generated");

    SimulationRequest request;
    request.circuit = circuit;
    request.kind = AnalysisKind::Transient;
    request.transient.step = 10e-6;
    request.transient.stop = 8e-3;
    const SimulationResult result = engine.runAsync(std::move(request)).get();
    check(result.ok, "555 astable transient runs: " + result.error);
    if (!result.ok) {
        std::printf("555 netlist:\n%s\n555 log:\n%s\n", compiled.netlist.c_str(),
                    result.log.c_str());
        return;
    }

    const auto nodes = compiled.nodes.find(timer);
    check(nodes != compiled.nodes.end() && nodes->second.count == 8,
          "555 compiles with eight pin nodes");
    if (nodes != compiled.nodes.end() && nodes->second.count == 8) {
        const auto out = result.nodeVoltage.find(nodes->second.pins[2]);
        if (out != result.nodeVoltage.end() && !out->second.empty()) {
            double minimum = out->second.front();
            double maximum = out->second.front();
            int rising = 0;
            bool high = minimum > 2.5;
            for (double value : out->second) {
                minimum = std::min(minimum, value);
                maximum = std::max(maximum, value);
                const bool nowHigh = value > 2.5;
                if (nowHigh && !high) {
                    ++rising;
                }
                high = nowHigh;
            }
            std::printf("555 OUT min=%.3f V max=%.3f V rising edges=%d\n", minimum,
                        maximum, rising);
            check(maximum > 2.8, "555 output reaches a high level");
            check(minimum < 0.8, "555 output reaches a low level");
            check(rising >= 2, "555 astable oscillates repeatedly");
        } else {
            check(false, "555 output node voltage is saved");
        }

        const auto pins = result.pinCurrent.find(timer);
        check(pins != result.pinCurrent.end() && pins->second.size() == 8,
              "555 exposes eight pin currents");
    }
}

void testFlowMultiPinBjt()
{
    NgspiceEngine engine;
    std::string error;
    check(engine.initialize(error), "ngspice initializes for BJT flow: " + error);

    Circuit circuit;
    const int source = circuit.addComponent(ComponentType::VoltageSource, 4, 10);
    if (Component* component = circuit.findComponent(source)) {
        component->source.mode = Waveform::DC;
        component->source.dc = 5.0;
    }
    const int baseResistor = circuit.addComponent(ComponentType::Resistor, 8, 6);
    if (Component* component = circuit.findComponent(baseResistor)) {
        component->value = 10e3;
    }
    const int collectorResistor = circuit.addComponent(ComponentType::Resistor, 18, 6);
    if (Component* component = circuit.findComponent(collectorResistor)) {
        component->value = 100.0;
    }
    const int transistor = circuit.addComponent(ComponentType::Npn, 10, 10);
    circuit.addComponent(ComponentType::Ground, 12, 13);
    circuit.addWirePoints({Point{4, 8}, Point{4, 4}});
    circuit.addWirePoints({Point{4, 4}, Point{18, 4}});
    circuit.addWirePoints({Point{8, 8}, Point{8, 10}});
    circuit.addWirePoints({Point{18, 8}, Point{12, 8}});
    circuit.addWirePoints({Point{4, 12}, Point{12, 12}});

    const CompiledCircuit compiled = compileCircuit(circuit);
    check(compiled.ok, "BJT flow netlist compiles: " + compiled.error);
    SimulationRequest request;
    request.circuit = circuit;
    request.kind = AnalysisKind::OperatingPoint;
    const SimulationResult result = engine.runAsync(std::move(request)).get();
    check(result.ok, "BJT flow operating point runs: " + result.error);

    FlowNetwork network = buildFlowNetwork(circuit, compiled);
    check(network.ok, "multi-pin flow network builds");
    std::map<int, std::vector<double>> pinSample;
    for (const auto& entry : result.pinCurrent) {
        std::vector<double> values;
        for (const auto& samples : entry.second) {
            values.push_back(samples.empty() ? 0.0 : samples.front());
        }
        pinSample[entry.first] = std::move(values);
    }
    solveFlowCurrents(network, pinSample);

    int pinEdges = 0;
    bool collectorEdge = false;
    bool emitterEdge = false;
    for (const FlowEdge& edge : network.edges) {
        if (edge.componentId == transistor && edge.componentPin >= 0) {
            ++pinEdges;
            if (edge.componentPin == 0 && edge.current > 0.01) {
                collectorEdge = true;
            }
            if (edge.componentPin == 2 && edge.current < -0.01) {
                emitterEdge = true;
            }
        }
    }
    check(pinEdges == 3, "BJT flow network has one edge per pin");
    check(collectorEdge, "BJT collector edge carries current into the device");
    check(emitterEdge, "BJT emitter edge carries current out of the device");

    bool collectorWire = false;
    bool baseWire = false;
    for (const FlowEdge& edge : network.edges) {
        if (edge.wireId == 0) {
            continue;
        }
        if (edge.pointA == Point{18, 8} && edge.pointB == Point{12, 8} &&
            std::fabs(edge.current) > 0.01) {
            collectorWire = true;
        }
        if (edge.pointA == Point{8, 8} && edge.pointB == Point{8, 10} &&
            std::fabs(edge.current) > 1e-5) {
            baseWire = true;
        }
    }
    check(collectorWire, "collector wire carries the transistor current");
    check(baseWire, "base wire carries the base current");
}

void testLedAndZener()
{
    NgspiceEngine engine;
    std::string error;
    check(engine.initialize(error), "ngspice initializes for LED/zener: " + error);

    // LED: 5 V -> 220 Ω -> LED -> ground.
    Circuit ledCircuit;
    const int ledSource = ledCircuit.addComponent(ComponentType::VoltageSource, 4, 10);
    if (Component* component = ledCircuit.findComponent(ledSource)) {
        component->source.mode = Waveform::DC;
        component->source.dc = 5.0;
    }
    const int ledResistor = ledCircuit.addComponent(ComponentType::Resistor, 8, 6);
    if (Component* component = ledCircuit.findComponent(ledResistor)) {
        component->value = 220.0;
    }
    const int led = ledCircuit.addComponent(ComponentType::Led, 12, 10);
    ledCircuit.addComponent(ComponentType::Ground, 12, 13);
    ledCircuit.addWirePoints({Point{4, 8}, Point{4, 4}});
    ledCircuit.addWirePoints({Point{4, 4}, Point{8, 4}});
    ledCircuit.addWirePoints({Point{8, 8}, Point{12, 8}});
    ledCircuit.addWirePoints({Point{4, 12}, Point{12, 12}});

    const CompiledCircuit ledCompiled = compileCircuit(ledCircuit);
    check(ledCompiled.ok, "LED netlist compiles: " + ledCompiled.error);
    SimulationRequest ledRequest;
    ledRequest.circuit = ledCircuit;
    ledRequest.kind = AnalysisKind::OperatingPoint;
    const SimulationResult ledResult = engine.runAsync(std::move(ledRequest)).get();
    check(ledResult.ok, "LED operating point runs: " + ledResult.error);
    if (!ledResult.ok) {
        std::printf("LED netlist:\n%s\nLED log:\n%s\n", ledCompiled.netlist.c_str(),
                    ledResult.log.c_str());
    }
    const auto ledPins = ledResult.pinCurrent.find(led);
    if (ledPins != ledResult.pinCurrent.end() && ledPins->second.size() == 2 &&
        !ledPins->second[0].empty()) {
        const double current = ledPins->second[0].front();
        std::printf("LED current = %.5f A\n", current);
        check(current > 0.006 && current < 0.02,
              "LED conducts a few mA at 5 V through 220 Ω");
    } else {
        check(false, "LED exposes a pin current");
    }

    // Zener in reverse bias: 12 V -> 1 kΩ -> cathode; anode to ground.
    Circuit zenerCircuit;
    const int zenerSource = zenerCircuit.addComponent(ComponentType::VoltageSource, 4, 10);
    if (Component* component = zenerCircuit.findComponent(zenerSource)) {
        component->source.mode = Waveform::DC;
        component->source.dc = 12.0;
    }
    const int zenerResistor = zenerCircuit.addComponent(ComponentType::Resistor, 8, 6);
    if (Component* component = zenerCircuit.findComponent(zenerResistor)) {
        component->value = 1000.0;
    }
    const int zener = zenerCircuit.addComponent(ComponentType::Zener, 12, 10);
    if (Component* component = zenerCircuit.findComponent(zener)) {
        component->value = 5.1;
        component->rotation = 180;
    }
    zenerCircuit.addComponent(ComponentType::Ground, 12, 13);
    zenerCircuit.addWirePoints({Point{4, 8}, Point{4, 4}});
    zenerCircuit.addWirePoints({Point{4, 4}, Point{8, 4}});
    zenerCircuit.addWirePoints({Point{8, 8}, Point{12, 8}});
    zenerCircuit.addWirePoints({Point{4, 12}, Point{12, 12}});

    const CompiledCircuit zenerCompiled = compileCircuit(zenerCircuit);
    check(zenerCompiled.ok, "zener netlist compiles: " + zenerCompiled.error);
    SimulationRequest zenerRequest;
    zenerRequest.circuit = zenerCircuit;
    zenerRequest.kind = AnalysisKind::OperatingPoint;
    const SimulationResult zenerResult = engine.runAsync(std::move(zenerRequest)).get();
    check(zenerResult.ok, "zener operating point runs: " + zenerResult.error);
    const auto zenerNodes = zenerCompiled.nodes.find(zener);
    if (zenerResult.ok && zenerNodes != zenerCompiled.nodes.end() &&
        zenerNodes->second.count == 2) {
        const auto cathode = zenerResult.nodeVoltage.find(zenerNodes->second.pins[1]);
        if (cathode != zenerResult.nodeVoltage.end() && !cathode->second.empty()) {
            std::printf("zener cathode = %.3f V\n", cathode->second.front());
            check(cathode->second.front() > 4.7 && cathode->second.front() < 5.5,
                  "zener clamps the reverse voltage near 5.1 V");
        }
    }
}

void testLoadDamageRecovery()
{
    Circuit circuit;
    circuit.addComponent(ComponentType::Bulb, 0, 0);
    circuit.addComponent(ComponentType::Fan, 4, 0);
    circuit.addComponent(ComponentType::Resistor, 8, 0);
    for (auto& component : circuit.components) {
        component.damaged = isLoadType(component.type);
    }

    const int revision = circuit.revision;
    check(clearLoadDamage(circuit) == 2, "recovery restores every damaged load");
    bool anyDamaged = false;
    for (const auto& component : circuit.components) {
        anyDamaged = anyDamaged || component.damaged;
    }
    check(!anyDamaged, "recovered loads are no longer damaged");
    check(circuit.revision != revision, "recovery bumps the circuit revision");
    check(clearLoadDamage(circuit) == 0, "recovery is a no-op when nothing is damaged");
}

// A wire drawn as three pieces with no component pin on the middle piece.
// The old animation only looked at component pins on each wire, so the middle
// piece never showed electrons. The flow solver must give it the loop current.
std::string lowerAscii(std::string text)
{
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

// Feed real ngspice branch currents through the divider loop and check that
// every wire piece flows in the same direction around the circuit.
void testFlowDividerCurrents()
{
    NgspiceEngine engine;
    std::string error;
    engine.initialize(error);

    const Circuit circuit = makeDividerDemo();
    const CompiledCircuit compiled = compileCircuit(circuit);
    check(compiled.ok, "divider compiles for flow");

    SimulationRequest request;
    request.circuit = circuit;
    request.kind = AnalysisKind::OperatingPoint;
    request.revision = 21;
    SimulationResult result = engine.runAsync(std::move(request)).get();
    check(result.ok, "divider operating point runs for flow");

    std::map<int, double> currents;
    for (const Component& component : circuit.components) {
        const auto found = result.deviceCurrent.find(lowerAscii(component.name));
        if (found != result.deviceCurrent.end() && !found->second.empty()) {
            currents[component.id] = found->second.front();
            std::printf("%s flow current = %s\n", component.name.c_str(),
                        formatSI(found->second.front(), "A").c_str());
        }
    }

    FlowNetwork network = buildFlowNetwork(circuit, compiled);
    solveFlowCurrents(network, currents);

    const double expected = 5.0 / 20000.0;
    bool everyPieceFlows = true;
    int pieces = 0;
    for (const FlowEdge& edge : network.edges) {
        if (edge.isDevice()) {
            continue;
        }
        ++pieces;
        if (std::fabs(std::fabs(edge.current) - expected) > 1e-9) {
            everyPieceFlows = false;
        }
    }
    check(pieces == 5, "divider copper is split into five flow pieces");
    check(everyPieceFlows, "every divider wire piece carries the loop current");

    const auto currentOn = [&](Point a, Point b) {
        for (const FlowEdge& edge : network.edges) {
            if (!edge.isDevice() && edge.pointA == a && edge.pointB == b) {
                return edge.current;
            }
        }
        return std::nan("");
    };
    check(currentOn(Point{8, 8}, Point{16, 8}) > 0.0,
          "top wire flows from the source positive pin toward R1");
    check(currentOn(Point{16, 8}, Point{16, 6}) > 0.0,
          "top wire corner keeps flowing toward R1");
    check(currentOn(Point{16, 10}, Point{16, 12}) > 0.0,
          "R1 to R2 wire flows downward");
    check(currentOn(Point{16, 16}, Point{8, 16}) > 0.0,
          "bottom wire flows toward the ground junction");
    check(currentOn(Point{8, 12}, Point{8, 16}) < 0.0,
          "return wire flows up toward the source negative pin");

    // A carrier that reaches the end of any live edge must find a live
    // continuation, otherwise the stream would visibly stop at a junction.
    uint32_t rng = 0x1234567u;
    bool continuous = true;
    for (size_t i = 0; i < network.edges.size(); ++i) {
        const FlowEdge& edge = network.edges[i];
        if (std::fabs(edge.current) < 1e-12) {
            continue;
        }
        const int arrival = edge.current > 0.0 ? edge.vertexB : edge.vertexA;
        if (flowChooseOutgoing(network, arrival, static_cast<int>(i), &rng) < 0) {
            continuous = false;
        }
    }
    check(continuous, "every live edge continues at its downstream junction");
}

// Two ground symbols on the same net but with separate copper islands. Each
// island's ground pin must take the return current so its wire flows too.
void testFlowSeparateGroundIslands()
{
    Circuit circuit;
    const int v1 = circuit.addComponent(ComponentType::VoltageSource, 4, 10);
    const int r1 = circuit.addComponent(ComponentType::Resistor, 20, 10);
    if (Component* resistor = circuit.findComponent(r1)) {
        resistor->value = 1000.0;
    }
    circuit.addComponent(ComponentType::Ground, 4, 16);
    circuit.addComponent(ComponentType::Ground, 20, 16);

    circuit.addWirePoints({Point{4, 8}, Point{20, 8}});
    const int leftGroundWire = circuit.addWirePoints({Point{4, 12}, Point{4, 15}});
    const int rightGroundWire = circuit.addWirePoints({Point{20, 12}, Point{20, 15}});

    const CompiledCircuit compiled = compileCircuit(circuit);
    check(compiled.ok, "split-ground circuit compiles");
    FlowNetwork network = buildFlowNetwork(circuit, compiled);

    std::map<int, double> deviceCurrents;
    deviceCurrents[v1] = -5e-3;
    deviceCurrents[r1] = 5e-3;
    solveFlowCurrents(network, deviceCurrents);

    double left = 0.0;
    double right = 0.0;
    for (const FlowEdge& edge : network.edges) {
        if (edge.isDevice()) {
            continue;
        }
        if (edge.wireId == leftGroundWire) {
            left = edge.current;
        }
        if (edge.wireId == rightGroundWire) {
            right = edge.current;
        }
    }
    check(std::fabs(left + 5e-3) < 1e-6,
          "the source-side ground wire carries the return current");
    check(std::fabs(right - 5e-3) < 1e-6,
          "the load-side ground wire carries the return current");
}

void testFlowWireBetweenJunctions()
{
    Circuit circuit;
    const int v1 = circuit.addComponent(ComponentType::VoltageSource, 4, 10);
    const int r1 = circuit.addComponent(ComponentType::Resistor, 20, 10);
    if (Component* resistor = circuit.findComponent(r1)) {
        resistor->value = 1000.0;
    }
    circuit.addComponent(ComponentType::Ground, 4, 17);

    const int leftWire = circuit.addWirePoints({Point{4, 8}, Point{10, 8}});
    const int middleWire = circuit.addWirePoints({Point{10, 8}, Point{14, 8}});
    const int rightWire = circuit.addWirePoints({Point{14, 8}, Point{20, 8}});
    circuit.addWirePoints({Point{20, 12}, Point{4, 12}});
    const int groundWire = circuit.addWirePoints({Point{4, 12}, Point{4, 16}});
    const int stubWire = circuit.addWirePoints({Point{10, 8}, Point{10, 4}});

    const CompiledCircuit compiled = compileCircuit(circuit);
    check(compiled.ok, "junction wire circuit compiles");
    FlowNetwork network = buildFlowNetwork(circuit, compiled);
    check(network.ok, "junction wire flow network builds");

    std::map<int, double> deviceCurrents;
    deviceCurrents[v1] = -5e-3;  // source delivers 5 mA (B -> A inside)
    deviceCurrents[r1] = 5e-3;
    solveFlowCurrents(network, deviceCurrents);

    double middleCurrent = 0.0;
    bool foundMiddle = false;
    bool stubQuiet = true;
    bool groundQuiet = true;
    int topPieces = 0;
    bool topAllLive = true;
    for (const FlowEdge& edge : network.edges) {
        if (edge.isDevice()) {
            continue;
        }
        if (edge.wireId == middleWire) {
            foundMiddle = true;
            middleCurrent = edge.current;
        }
        if (edge.wireId == stubWire && std::fabs(edge.current) > 1e-12) {
            stubQuiet = false;
        }
        if (edge.wireId == groundWire && std::fabs(edge.current) > 1e-12) {
            groundQuiet = false;
        }
        if (edge.wireId == leftWire || edge.wireId == middleWire ||
            edge.wireId == rightWire) {
            ++topPieces;
            if (std::fabs(edge.current) < 4.9e-3) {
                topAllLive = false;
            }
        }
    }
    std::printf("middle wire current = %s\n",
                formatSI(std::fabs(middleCurrent), "A").c_str());
    check(foundMiddle, "the wire between junctions is split into the flow graph");
    check(std::fabs(middleCurrent - 5e-3) < 1e-6,
          "the wire between junctions carries the loop current");
    check(middleCurrent > 0.0,
          "flow on the middle wire runs from source to load");
    check(topPieces == 3, "the top route is split into three wire pieces");
    check(topAllLive, "every piece of the top route carries current");
    check(stubQuiet, "a dead-end wire carries no electrons");
    check(groundQuiet, "an unused ground spur carries no electrons");
}

// A branch point splits the source current between two devices. Each branch
// must carry its device current instead of stopping at the junction.
void testFlowBranchSplit()
{
    Circuit circuit;
    const int v1 = circuit.addComponent(ComponentType::VoltageSource, 0, 10);
    const int r1 = circuit.addComponent(ComponentType::Resistor, 16, 6);
    const int r2 = circuit.addComponent(ComponentType::Resistor, 4, 14);
    if (Component* resistor = circuit.findComponent(r1)) {
        resistor->rotation = 90;
    }
    if (Component* resistor = circuit.findComponent(r2)) {
        resistor->rotation = 90;
    }
    circuit.addComponent(ComponentType::Ground, 0, 17);

    const int trunk = circuit.addWirePoints({Point{0, 8}, Point{6, 8}});
    const int branchOne =
        circuit.addWirePoints({Point{6, 8}, Point{18, 8}, Point{18, 6}});
    const int branchTwo = circuit.addWirePoints({Point{6, 8}, Point{6, 14}});
    circuit.addWirePoints({Point{14, 6}, Point{14, 12}});
    circuit.addWirePoints({Point{14, 12}, Point{0, 12}});
    circuit.addWirePoints({Point{0, 12}, Point{0, 16}});
    circuit.addWirePoints({Point{2, 14}, Point{0, 14}});

    const CompiledCircuit compiled = compileCircuit(circuit);
    check(compiled.ok, "branch flow circuit compiles");
    FlowNetwork network = buildFlowNetwork(circuit, compiled);

    std::map<int, double> deviceCurrents;
    deviceCurrents[v1] = -10e-3;
    deviceCurrents[r1] = 4e-3;
    deviceCurrents[r2] = 6e-3;
    solveFlowCurrents(network, deviceCurrents);

    double trunkCurrent = 0.0;
    bool branchOneLive = true;
    double branchOneCurrent = 0.0;
    double branchTwoCurrent = 0.0;
    int branchOnePieces = 0;
    for (const FlowEdge& edge : network.edges) {
        if (edge.isDevice()) {
            continue;
        }
        if (edge.wireId == trunk) {
            trunkCurrent = edge.current;
        }
        if (edge.wireId == branchOne) {
            ++branchOnePieces;
            branchOneCurrent = edge.current;
            if (std::fabs(edge.current - 4e-3) > 1e-6) {
                branchOneLive = false;
            }
        }
        if (edge.wireId == branchTwo) {
            branchTwoCurrent = edge.current;
        }
    }
    check(std::fabs(trunkCurrent - 10e-3) < 1e-6,
          "the shared wire carries the summed branch current");
    check(branchOnePieces == 2, "an L-shaped branch is split into two pieces");
    check(branchOneLive, "every piece of a branch carries its current");
    check(std::fabs(std::fabs(branchOneCurrent) - 4e-3) < 1e-6,
          "the first branch gets its share of the current");
    check(std::fabs(std::fabs(branchTwoCurrent) - 6e-3) < 1e-6,
          "the second branch gets its share of the current");
    check(branchOneCurrent > 0.0 && branchTwoCurrent > 0.0,
          "branch flow runs away from the junction toward the loads");
}

void testProjectRoundTrip()
{
    const std::string directory = "./circuitlab_test_projects";
    std::string error;
    check(ensureProjectDirectory(directory, &error), "project directory can be created");

    Circuit circuit = makeSwitchBulbCircuit();
    for (auto& component : circuit.components) {
        if (component.type == ComponentType::Switch) {
            component.switchClosed = false;
        }
        if (component.type == ComponentType::Bulb) {
            component.ratedVoltage = 6.0;
            component.damaged = true;
        }
    }
    const std::string path = directory + "/roundtrip.ckt";
    check(saveProject(circuit, path, "测试工程", &error), "project saves with a name");

    Circuit loaded;
    std::string name;
    check(loadProject(path, &loaded, &name, &error), "project loads back");
    check(name == "测试工程", "project name round-trips");
    check(loaded.components.size() == circuit.components.size(), "component count round-trips");
    bool foundOpenSwitch = false;
    bool foundDamagedBulb = false;
    for (const auto& component : loaded.components) {
        if (component.type == ComponentType::Switch && !component.switchClosed) {
            foundOpenSwitch = true;
        }
        if (component.type == ComponentType::Bulb && component.damaged &&
            std::fabs(component.ratedVoltage - 6.0) < 1e-9) {
            foundDamagedBulb = true;
            check(componentResistance(component) > 1e9,
                  "damaged bulb is an open circuit");
        }
    }
    check(foundOpenSwitch, "open switch state round-trips");
    check(foundDamagedBulb, "load rating and damage state round-trip");

    const auto listed = listProjects(directory);
    bool found = false;
    for (const auto& project : listed) {
        if (project.name == "测试工程") {
            found = true;
        }
    }
    check(found, "project list exposes the saved project");

    check(deleteProjectFile(path, &error), "project file can be deleted");
    rmdir(directory.c_str());
}

void testNewComponentProjectRoundTrip()
{
    const std::string directory = "./circuitlab_new_projects";
    std::string error;
    check(ensureProjectDirectory(directory, &error), "new component project dir");

    Circuit circuit;
    const int diode = circuit.addComponent(ComponentType::Diode, 4, 4);
    const int zener = circuit.addComponent(ComponentType::Zener, 8, 4);
    if (Component* component = circuit.findComponent(zener)) {
        component->value = 6.2;
    }
    const int transistor = circuit.addComponent(ComponentType::Npn, 12, 4);
    const int timer = circuit.addComponent(ComponentType::Timer555, 16, 4);
    const int opamp = circuit.addComponent(ComponentType::OpAmp, 20, 4);
    (void)diode;
    (void)transistor;
    (void)timer;
    (void)opamp;

    const std::string path = directory + "/newparts.ckt";
    check(saveProject(circuit, path, "新元件", &error), "new component project saves");
    Circuit loaded;
    check(loadProject(path, &loaded, &error), "new component project loads");
    check(loaded.components.size() == circuit.components.size(),
          "new component count round-trips");
    bool diodeOk = false;
    bool zenerOk = false;
    bool npnOk = false;
    bool timerOk = false;
    bool opampOk = false;
    for (const auto& component : loaded.components) {
        if (component.type == ComponentType::Diode) {
            diodeOk = true;
        } else if (component.type == ComponentType::Zener &&
                   std::fabs(component.value - 6.2) < 1e-9) {
            zenerOk = true;
        } else if (component.type == ComponentType::Npn) {
            Point pins[kMaxComponentPins];
            npnOk = componentPins(component, pins) == 3;
        } else if (component.type == ComponentType::Timer555) {
            timerOk = pinCount(component.type) == 8;
        } else if (component.type == ComponentType::OpAmp) {
            opampOk = pinCount(component.type) == 5;
        }
    }
    check(diodeOk, "diode type round-trips");
    check(zenerOk, "zener breakdown value round-trips");
    check(npnOk, "NPN type and pins round-trip");
    check(timerOk, "555 type and pins round-trip");
    check(opampOk, "op-amp type and pins round-trip");

    check(deleteProjectFile(path, &error), "new component project deletes");
    rmdir(directory.c_str());
}

void testTimer555Example()
{
    const Circuit circuit = makeTimer555Demo();
    const CompiledCircuit compiled = compileCircuit(circuit);
    check(compiled.ok, "555 demo compiles: " + compiled.error);
    check(compiled.netlist.find(".subckt CL_555") != std::string::npos,
          "555 demo includes the timer model");
}

void testInvalidCircuit()
{
    NgspiceEngine engine;
    std::string error;
    engine.initialize(error);

    Circuit circuit;
    circuit.addComponent(ComponentType::VoltageSource, 0, 0);
    circuit.addComponent(ComponentType::Resistor, 4, 0);
    const CompiledCircuit compiled = compileCircuit(circuit);
    check(!compiled.ok, "a circuit without ground is rejected");
    check(compiled.error.find("接地") != std::string::npos,
          "missing-ground error mentions ground");

    SimulationRequest request;
    request.circuit = circuit;
    request.kind = AnalysisKind::OperatingPoint;
    SimulationResult result = engine.runAsync(std::move(request)).get();
    check(!result.ok, "engine rejects a circuit without ground");
    check(result.nodeVoltage.empty(), "failed run does not expose stale vectors");
    check(!result.error.empty(), "failed run reports an error");
}


// The PWM source must emit a duty-cycle accurate pulse, and the power stage
// parts must reach ngspice with their series resistance split out.
void testPwmDutyCycle()
{
    Circuit circuit;
    const int source = circuit.addComponent(ComponentType::PwmSource, 4, 8);
    if (Component* component = circuit.findComponent(source)) {
        component->source.mode = Waveform::Pulse;
        component->source.dc = 0.0;
        component->source.amplitude = 5.0;
        component->source.frequency = 50e3;
        component->source.period = 20e-6;
        component->source.duty = 0.25;
        component->source.pulseWidth = 5e-6;
        component->source.rise = 50e-9;
    }
    circuit.addComponent(ComponentType::Resistor, 8, 8);
    circuit.addComponent(ComponentType::Ground, 4, 14);
    circuit.addWirePoints({Point{4, 6}, Point{8, 6}});
    circuit.addWirePoints({Point{8, 10}, Point{4, 10}});
    circuit.addWirePoints({Point{4, 10}, Point{4, 13}});

    const CompiledCircuit compiled = compileCircuit(circuit);
    check(compiled.ok, "PWM source netlist compiles: " + compiled.error);
    const std::string pulse = "PULSE(0 5 0 5e-08 5e-08 5e-06 2e-05)";
    if (compiled.ok) {
        std::printf("PWM netlist line: %s\n",
                    compiled.netlist.substr(compiled.netlist.find("V1 "),
                                            compiled.netlist.find('\n', compiled.netlist.find("V1 ")) -
                                                compiled.netlist.find("V1 ")).c_str());
    }
    check(compiled.netlist.find(pulse) != std::string::npos,
          "PWM source emits a 25% duty cycle pulse");
    // The PWM generator must follow the switching-window rule, not the 555
    // oscillator rule (which used to clamp the window to 10 k points).
    check(!forcesTransient(ComponentType::PwmSource),
          "the PWM source does not use the 555 oscillator window");
    check(forcesTransient(ComponentType::Timer555),
          "the 555 timer still forces a long transient window");
}

void testPowerStageParts()
{
    Circuit circuit;
    circuit.addComponent(ComponentType::VoltageSource, 4, 8);
    const int inductor = circuit.addComponent(ComponentType::PowerInductor, 10, 8);
    if (Component* part = circuit.findComponent(inductor)) {
        part->value = 100e-6;
        part->seriesR = 0.05;
    }
    const int capacitor = circuit.addComponent(ComponentType::ElectrolyticCap, 16, 8);
    if (Component* part = circuit.findComponent(capacitor)) {
        part->value = 220e-6;
        part->seriesR = 0.1;
    }
    circuit.addComponent(ComponentType::SchottkyDiode, 22, 8);
    const int mos = circuit.addComponent(ComponentType::PowerNmos, 28, 8);
    const int probe = circuit.addComponent(ComponentType::Oscilloscope, 34, 8);
    const int terminal = circuit.addComponent(ComponentType::TerminalBlock, 40, 8);
    circuit.addComponent(ComponentType::Ground, 4, 14);

    // Everything hangs between a top and a bottom rail; the gate is tied to the
    // top rail so the MOSFET has no floating terminal.
    circuit.addWirePoints({Point{4, 6}, Point{40, 6}});
    circuit.addWirePoints({Point{4, 10}, Point{40, 10}});
    circuit.addWirePoints({Point{4, 10}, Point{4, 13}});
    circuit.addWirePoints({Point{26, 8}, Point{26, 6}});

    const CompiledCircuit compiled = compileCircuit(circuit);
    check(compiled.ok, "power stage netlist compiles: " + compiled.error);
    const std::string& deck = compiled.netlist;
    check(deck.find("L1X") != std::string::npos && deck.find("RL1") != std::string::npos,
          "power inductor emits its winding resistance");
    check(deck.find("RC1") != std::string::npos,
          "electrolytic capacitor emits its ESR");
    check(deck.find(".model DSCH D(") != std::string::npos,
          "Schottky diode card is included");
    check(deck.find(".model MPWR_N NMOS") != std::string::npos,
          "power N-MOS model is included");
    check(deck.find("RSC1") != std::string::npos,
          "oscilloscope is emitted as a resistor card");
    check(deck.find("RJ1") == std::string::npos,
          "terminal block emits no card of its own");

    const Component* scope = circuit.findComponent(probe);
    const Component* block = circuit.findComponent(terminal);
    const Component* mosPart = circuit.findComponent(mos);
    check(scope && std::fabs(componentResistance(*scope) - 1e6) < 1.0,
          "oscilloscope input is 1 MΩ");
    if (block) {
        const auto nodes = compiled.nodes.find(block->id);
        check(nodes != compiled.nodes.end() && nodes->second.count == 2 &&
                  nodes->second.a() != nodes->second.b(),
              "terminal block keeps its two screws on separate nets");
    }
    if (scope) {
        check(pinCurrentKey(*scope, 0) == "rsc1",
              "scope current is read through its resistor card");
    }
    if (mosPart) {
        check(pinCount(mosPart->type) == 3, "power MOSFET has three pins");
    }
    check(isProbeType(ComponentType::Oscilloscope), "oscilloscope is a probe");
    check(isConnectorType(ComponentType::TerminalBlock), "terminal block is a connector");
}

void testWaveMeasure()
{
    std::vector<double> time;
    std::vector<double> values;
    for (int i = 0; i < 4000; ++i) {
        const double t = static_cast<double>(i) * 1e-6;
        time.push_back(t);
        values.push_back(6.0 + 0.05 * std::sin(2.0 * 3.14159265358979 * 50e3 * t));
    }
    const WaveStats stats = measureWave(time, values);
    std::printf("measure: mean = %.4f V, ripple = %.4f V, f = %.0f Hz\n", stats.mean,
                stats.ripple, stats.frequency);
    check(stats.ok, "measurement accepts a transient window");
    check(std::fabs(stats.mean - 6.0) < 0.01, "measurement reports the DC average");
    check(stats.ripple > 0.09 && stats.ripple < 0.11,
          "measurement reports the peak-to-peak ripple");
    check(std::fabs(stats.frequency - 50e3) < 1000.0,
          "measurement reports the switching frequency");

    std::vector<double> flat(values.size(), 6.0);
    const WaveStats steady = measureWave(time, flat);
    check(steady.ok && steady.ripple < 1e-9, "a flat output has no ripple");

    std::vector<double> empty;
    check(!measureWave(empty, empty).ok, "measurement rejects an empty window");
}

// End-to-end check of the board the user wants to build: a 12 V rail chopped at
// 50 kHz and filtered by L-C, with the oscilloscope probe on the output.
void testMosSwitchBoard()
{
    const Circuit circuit = makeMosSwitchBoardDemo();
    const CompiledCircuit compiled = compileCircuit(circuit);
    check(compiled.ok, "MOS switch board compiles: " + compiled.error);
    if (!compiled.ok) {
        return;
    }

    int chopper = -1;
    int output = -1;
    for (const auto& component : circuit.components) {
        if (component.type != ComponentType::PowerInductor) {
            continue;
        }
        const auto nodes = compiled.nodes.find(component.id);
        if (nodes != compiled.nodes.end()) {
            chopper = nodes->second.a();
            output = nodes->second.b();
        }
    }
    check(chopper > 0 && output > 0 && chopper != output,
          "the inductor separates the chopper node from the output node");

    NgspiceEngine engine;
    std::string error;
    check(engine.initialize(error), "ngspice initializes for the switch board");

    SimulationRequest request;
    request.circuit = circuit;
    request.kind = AnalysisKind::Transient;
    request.transient.step = 1e-6;
    request.transient.stop = 50e-3;
    request.revision = 31;
    const SimulationResult result = engine.runAsync(std::move(request)).get();
    check(result.ok, "MOS switch board transient runs: " + result.error);
    if (!result.ok) {
        return;
    }

    const auto outSeries = result.nodeVoltage.find(output);
    const auto chopSeries = result.nodeVoltage.find(chopper);
    check(outSeries != result.nodeVoltage.end() && chopSeries != result.nodeVoltage.end(),
          "the switch board produces both nodes");

    const WaveStats vout = measureWave(result.time, outSeries->second);
    const WaveStats vsw = measureWave(result.time, chopSeries->second);
    std::printf("switch board: vout = %.3f V, ripple = %.1f mV, %.0f Hz, "
                "vswitch %.2f..%.2f V\n",
                vout.mean, vout.ripple * 1e3, vout.frequency, vsw.min, vsw.max);

    check(std::fabs(vout.mean - 5.7) < 0.8,
          "the LC filter turns the 12 V chopper into about half of the input");
    check(vout.ripple < 0.4, "the switching ripple at the output stays below 0.4 V");
    check(vsw.max > 10.0 && vsw.min < 1.0,
          "the chopper node still swings between the rails");
    check(std::fabs(vout.frequency - 50e3) < 5000.0,
          "the output ripple repeats at the PWM frequency");
}


void testPowerPartsProjectRoundTrip()
{
    const std::string directory = "./circuitlab_power_projects";
    std::string error;
    check(ensureProjectDirectory(directory, &error), "power part project dir");

    Circuit circuit;
    const int pwm = circuit.addComponent(ComponentType::PwmSource, 4, 4);
    if (Component* component = circuit.findComponent(pwm)) {
        component->source.mode = Waveform::Pulse;
        component->source.duty = 0.35;
        component->source.frequency = 50e3;
        component->source.period = 20e-6;
        component->source.pulseWidth = 7e-6;
    }
    const int coil = circuit.addComponent(ComponentType::PowerInductor, 8, 4);
    if (Component* component = circuit.findComponent(coil)) {
        component->value = 47e-6;
        component->seriesR = 0.08;
    }
    const int cap = circuit.addComponent(ComponentType::ElectrolyticCap, 12, 4);
    if (Component* component = circuit.findComponent(cap)) {
        component->value = 470e-6;
        component->seriesR = 0.05;
    }
    circuit.addComponent(ComponentType::Oscilloscope, 16, 4);
    circuit.addComponent(ComponentType::TerminalBlock, 20, 4);
    circuit.addComponent(ComponentType::SchottkyDiode, 24, 4);
    circuit.addComponent(ComponentType::PowerNmos, 28, 4);

    const std::string path = directory + "/power.ckt";
    check(saveProject(circuit, path, "电源元件", &error), "power parts save");
    Circuit loaded;
    check(loadProject(path, &loaded, &error), "power parts load");
    check(loaded.components.size() == circuit.components.size(),
          "power part count round-trips");
    bool dutyOk = false;
    bool coilOk = false;
    bool capOk = false;
    bool scopeOk = false;
    bool termOk = false;
    for (const auto& component : loaded.components) {
        if (component.type == ComponentType::PwmSource) {
            dutyOk = std::fabs(component.source.duty - 0.35) < 1e-9 &&
                     std::fabs(component.source.pulseWidth - 7e-6) < 1e-12;
        } else if (component.type == ComponentType::PowerInductor) {
            coilOk = std::fabs(component.value - 47e-6) < 1e-15 &&
                     std::fabs(component.seriesR - 0.08) < 1e-12;
        } else if (component.type == ComponentType::ElectrolyticCap) {
            capOk = std::fabs(component.value - 470e-6) < 1e-15 &&
                    std::fabs(component.seriesR - 0.05) < 1e-12;
        } else if (component.type == ComponentType::Oscilloscope) {
            scopeOk = std::fabs(component.value - 1e6) < 1.0;
        } else if (component.type == ComponentType::TerminalBlock) {
            termOk = true;
        }
    }
    check(dutyOk, "PWM duty cycle and pulse width round-trip");
    check(coilOk, "power inductor value and DCR round-trip");
    check(capOk, "electrolytic capacitor value and ESR round-trip");
    check(scopeOk, "oscilloscope input impedance round-trips");
    check(termOk, "terminal block round-trips");

    check(deleteProjectFile(path, &error), "power part project deletes");
    rmdir(directory.c_str());
}

// The electron animation must also cope with the new parts: a terminal block
// never carries current and the probe only draws microamps.
void testMosSwitchBoardFlow()
{
    const Circuit circuit = makeMosSwitchBoardDemo();
    const CompiledCircuit compiled = compileCircuit(circuit);
    check(compiled.ok, "switch board compiles for flow: " + compiled.error);
    FlowNetwork network = buildFlowNetwork(circuit, compiled);
    int wireEdges = 0;
    for (const FlowEdge& edge : network.edges) {
        if (!edge.isDevice()) {
            ++wireEdges;
        }
    }
    check(wireEdges > 10, "switch board produces wire flow segments");

    std::map<int, double> currents;
    for (const auto& component : circuit.components) {
        currents[component.id] = 0.0;
    }
    solveFlowCurrents(network, currents);
    bool finite = true;
    for (const FlowEdge& edge : network.edges) {
        if (!std::isfinite(edge.current)) {
            finite = false;
        }
    }
    check(finite, "flow currents stay finite with the new parts");
}


// The app drops this file into the project list on first launch, so the saved
// artifact itself has to survive a round trip and still compile.
void testMosSwitchBoardProjectFile()
{
    const std::string directory = "./circuitlab_example_projects";
    std::string error;
    check(ensureProjectDirectory(directory, &error), "example project dir");

    const std::string path = directory + "/mos_example.ckt";
    check(saveProject(makeMosSwitchBoardDemo(), path, "MOS 开关板示例", &error),
          "MOS switch board example saves as a project");

    Circuit loaded;
    std::string name;
    check(loadProject(path, &loaded, &name, &error), "MOS switch board example loads");
    check(name == "MOS 开关板示例", "example project keeps its name");

    const CompiledCircuit compiled = compileCircuit(loaded);
    check(compiled.ok, "loaded example compiles: " + compiled.error);

    bool hasPwm = false;
    bool hasSchottky = false;
    int probes = 0;
    int terminals = 0;
    bool hasPowerPmos = false;
    bool hasLampLoad = false;
    bool hasInputCap = false;
    bool hasSourceResistance = false;
    for (const auto& component : loaded.components) {
        if (component.type == ComponentType::PwmSource) {
            hasPwm = std::fabs(component.source.frequency - 50e3) < 1.0 &&
                     std::fabs(component.source.duty - 0.5) < 1e-9;
        } else if (component.type == ComponentType::SchottkyDiode) {
            hasSchottky = true;
        } else if (component.type == ComponentType::Oscilloscope) {
            ++probes;
        } else if (component.type == ComponentType::TerminalBlock) {
            ++terminals;
        } else if (component.type == ComponentType::PowerPmos) {
            hasPowerPmos = true;
        } else if (component.type == ComponentType::Bulb) {
            hasLampLoad = hasLampLoad || (component.ratedVoltage > 5.0 &&
                                          component.ratedVoltage < 7.0);
        } else if (component.type == ComponentType::ElectrolyticCap) {
            // The 100 µF input bulk capacitor sits before the switch.
            hasInputCap = hasInputCap || std::fabs(component.value - 100e-6) < 1e-12;
        } else if (component.type == ComponentType::Resistor) {
            hasSourceResistance =
                hasSourceResistance || std::fabs(component.value - 0.2) < 1e-12;
        }
    }
    check(hasPwm, "example keeps the 50 kHz 50% PWM source");
    check(hasSchottky && hasPowerPmos, "example keeps the switch and freewheel diode");
    check(terminals == 3, "example keeps the three 2-pin board terminals");
    check(probes == 2, "example ships two oscilloscope probes");
    check(hasLampLoad, "example drives a real 6 V lamp load");
    check(hasInputCap, "example has the input bulk capacitor");
    check(hasSourceResistance, "example models the supply source resistance");

    check(deleteProjectFile(path, &error), "example project deletes");
    rmdir(directory.c_str());
}


// Wrongly wired supplies used to fail inside ngspice with a bare
// "仿真失败"; the compiler now names the offending component.
void testSourceWiringErrors()
{
    {
        Circuit circuit;
        circuit.addComponent(ComponentType::VoltageSource, 4, 4);
        circuit.addComponent(ComponentType::Resistor, 8, 4);
        circuit.addComponent(ComponentType::Ground, 0, 8);
        // Both pins of the source end up on one net.
        circuit.addWirePoints({Point{4, 2}, Point{4, 6}});
        circuit.addWirePoints({Point{4, 6}, Point{0, 6}});
        circuit.addWirePoints({Point{0, 6}, Point{0, 7}});
        circuit.addWirePoints({Point{4, 2}, Point{8, 2}});
        circuit.addWirePoints({Point{8, 6}, Point{8, 2}});
        const CompiledCircuit compiled = compileCircuit(circuit);
        check(!compiled.ok, "a shorted voltage source is rejected");
        check(compiled.error.find("短路") != std::string::npos,
              "shorted source error mentions the short");
        check(compiled.error.find("V1") != std::string::npos,
              "shorted source error names the component");
    }
    {
        // A source dropped on the canvas but not wired to anything: it forms a
        // floating island and ngspice used to abort the whole analysis.
        const Circuit circuit = [&] {
            Circuit c = makeMosSwitchBoardDemo();
            c.addComponent(ComponentType::VoltageSource, 90, 90);
            return c;
        }();
        const CompiledCircuit compiled = compileCircuit(circuit);
        check(!compiled.ok, "an unwired voltage source is rejected");
        check(compiled.error.find("没有接到电路上") != std::string::npos,
              "unwired source error explains it is not connected");
        check(compiled.error.find("直流电源") != std::string::npos,
              "unwired source error names the part type");
    }
    {
        Circuit circuit;
        circuit.addComponent(ComponentType::VoltageSource, 4, 4);
        circuit.addComponent(ComponentType::VoltageSource, 10, 4);
        circuit.addComponent(ComponentType::Ground, 7, 10);
        circuit.addWirePoints({Point{4, 2}, Point{10, 2}});
        circuit.addWirePoints({Point{4, 6}, Point{10, 6}});
        circuit.addWirePoints({Point{7, 6}, Point{7, 9}});
        const CompiledCircuit compiled = compileCircuit(circuit);
        check(!compiled.ok, "two parallel voltage sources are rejected");
        check(compiled.error.find("并联") != std::string::npos,
              "parallel source error explains the parallel connection");
    }
}


// Two wires that cross without touching must be reported so the canvas can draw
// a jumper bridge: the wire added first stays straight, the later one hops.
void testWireCrossings()
{
    {
        Circuit circuit;
        const int first = circuit.addWirePoints({Point{0, 0}, Point{10, 0}});
        const int second = circuit.addWirePoints({Point{5, -4}, Point{5, 4}});
        const std::vector<WireCrossing> crossings = findWireCrossings(circuit);
        check(crossings.size() == 1, "a body crossing between two wires is found");
        if (crossings.size() == 1) {
            check(crossings[0].straightWireId == first,
                  "the earlier wire is drawn straight");
            check(crossings[0].hoppingWireId == second,
                  "the later wire is the one that hops");
            check(crossings[0].point == Point{5, 0}, "the crossing point is exact");
        }
    }
    {
        Circuit circuit;
        circuit.addWirePoints({Point{0, 0}, Point{10, 0}});
        circuit.addWirePoints({Point{5, 0}, Point{5, 6}});
        check(findWireCrossings(circuit).empty(), "a T junction is a connection, not a crossing");
    }
    {
        Circuit circuit;
        circuit.addWirePoints({Point{0, 0}, Point{5, 0}});
        circuit.addWirePoints({Point{5, 0}, Point{5, 5}});
        check(findWireCrossings(circuit).empty(), "a shared vertex is not a crossing");
    }
    {
        Circuit circuit;
        circuit.addWirePoints({Point{0, 0}, Point{10, 0}});
        circuit.addWirePoints({Point{4, 0}, Point{6, 0}});
        check(findWireCrossings(circuit).empty(), "collinear wires are not crossings");
    }
    {
        const Circuit circuit = makeMosSwitchBoardDemo();
        const std::vector<WireCrossing> crossings = findWireCrossings(circuit);
        check(crossings.size() == 4, "the example uses four jumper crossings");
        bool laterAlwaysHops = true;
        for (const WireCrossing& crossing : crossings) {
            if (crossing.hoppingWireIndex <= crossing.straightWireIndex) {
                laterAlwaysHops = false;
            }
        }
        check(laterAlwaysHops, "in the example every hop is on the later wire");
    }
}


// Loads behave like real parts: a start-up voltage, a nominal voltage, a
// maximum voltage (a fan tolerates +10 %) and a maximum current.
void testLoadRatings()
{
    Circuit circuit;
    const int fanId = circuit.addComponent(ComponentType::Fan, 0, 0);
    const Component* fan = circuit.findComponent(fanId);
    check(fan != nullptr, "fan exists");
    if (fan) {
        check(std::fabs(fan->ratedVoltage - 12.0) < 1e-9, "a fan is a 12 V part");
        check(fan->minVoltage > 0.0 && fan->minVoltage < fan->ratedVoltage,
              "a fan has a start-up voltage below nominal");
        check(std::fabs(fan->maxVoltage - fan->ratedVoltage * 1.10) < 1e-9,
              "a fan tolerates 10 % overvoltage");
        check(std::fabs(fan->maxCurrent - fan->maxVoltage / fan->value) < 1e-9,
              "the fan current limit matches the maximum voltage");
        check(std::fabs(loadRatedCurrent(*fan) - 0.12) < 1e-9,
              "the 12 V fan draws 0.12 A at nominal");
        check(std::fabs(loadRatedPower(*fan) - 1.44) < 1e-9, "the fan is a 1.4 W part");

        check(loadFault(*fan, 12.0, 0.12) == LoadFault::None, "12 V is fine for the fan");
        check(loadFault(*fan, 13.0, 0.13) == LoadFault::None,
              "a fan survives 10 % overvoltage");
        check(loadFault(*fan, 13.6, 0.136) == LoadFault::OverVoltage,
              "13.6 V destroys the fan");

        check(loadLevel(*fan, 3.0) == 0.0, "a fan does not start at 3 V");
        check(loadLevel(*fan, fan->minVoltage - 0.1) == 0.0,
              "a fan does not start below its start-up voltage");
        const double half = loadLevel(*fan, 9.0);
        check(half > 0.0 && half < 0.9, "a fan spins slower at 9 V");
        check(loadLevel(*fan, 12.0) > 0.99, "a fan runs full speed at 12 V");
    }

    const int bulbId = circuit.addComponent(ComponentType::Bulb, 6, 0);
    const Component* bulb = circuit.findComponent(bulbId);
    check(bulb != nullptr, "bulb exists");
    if (bulb) {
        check(std::fabs(loadRatedCurrent(*bulb) - 1.0) < 1e-9,
              "the default lamp draws 1 A at 12 V");
        check(std::fabs(loadRatedPower(*bulb) - 12.0) < 1e-9,
              "the default lamp is 12 W");
        check(loadFault(*bulb, 12.0, 1.0) == LoadFault::None, "12 V / 1 A is fine");
        check(loadFault(*bulb, 14.5, 14.5 / bulb->value) == LoadFault::OverVoltage,
              "14.5 V destroys the lamp");
    }

    // A load can also be limited by current, e.g. by the socket it sits in.
    Component socketLimited;
    socketLimited.type = ComponentType::Bulb;
    socketLimited.value = 6.0;
    socketLimited.ratedVoltage = 6.0;
    applyLoadDefaults(socketLimited);
    socketLimited.maxCurrent = 1.2;
    check(loadFault(socketLimited, 6.0, 1.0) == LoadFault::None, "1 A is inside the limit");
    check(loadFault(socketLimited, 6.0, 1.4) == LoadFault::OverCurrent,
          "1.4 A trips the current limit");

    // Projects saved before the ratings existed get sensible defaults.
    Component legacy;
    legacy.type = ComponentType::Fan;
    legacy.value = 100.0;
    legacy.ratedVoltage = 12.0;
    applyLoadDefaults(legacy);
    check(legacy.minVoltage > 0.0 && legacy.maxVoltage > legacy.ratedVoltage &&
              legacy.maxCurrent > 0.0,
          "missing load ratings are filled with defaults");
}

// Values typed into a parameter field: the editor pops up the on-screen
// keyboard, so the parser has to accept everything an engineer types, and the
// text shown in the field has to parse back to exactly the value it came from.
void testEngineeringInput()
{
    struct Case {
        const char* text;
        double value;
    };
    static const Case kCases[] = {
        {"4700", 4700.0},
        {"4.7k", 4700.0},
        {"4.7 k", 4700.0},
        {"4.7kΩ", 4700.0},
        {"1k2", 1200.0},          // European "1k2" = 1.2 kΩ
        {"4R7", 4.7},             // resistor "R" as the decimal marker
        {"100n", 1e-7},
        {"10u", 1e-5},
        {"10µ", 1e-5},            // U+00B5 micro sign
        {"2μ2", 2.2e-6},          // U+03BC Greek mu
        {"1m", 1e-3},
        {"1M", 1e6},
        {"1meg", 1e6},
        {"1e-3", 1e-3},
        {"2.5E3", 2500.0},
        {"4,7k", 4700.0},         // comma as the decimal separator
        {"1,000", 1000.0},        // comma as the thousands separator
        {" 12 V ", 12.0},
        {"50%", 50.0},
        {".47", 0.47},
        {"-5", -5.0},
        {"1e6Hz", 1e6},
        {"１２３", 123.0},          // full-width digits from a Chinese IME
    };
    for (const Case& item : kCases) {
        const ParsedNumber parsed = parseEngineering(item.text);
        const double tolerance = 1e-9 * std::max(1.0, std::fabs(item.value));
        check(parsed.ok && std::fabs(parsed.value - item.value) <= tolerance,
              std::string("\"") + item.text + "\" is understood as " +
                  formatSI(item.value, ""));
    }

    static const char* kRejected[] = {"", "  ", "abc", "k", "e-3", "%"};
    for (const char* text : kRejected) {
        check(!parseEngineering(text).ok,
              std::string("word-only input \"") + text + "\" is rejected");
    }

    // Opening a parameter and confirming it must not change the value: the
    // text the editor starts with has to parse back unchanged.
    static const double kRoundTrip[] = {4700.0, 1e-7, 1e-9, 12.0, 2.2e-6, 33.3, 1e6, 0.06};
    bool roundTripOk = true;
    for (double value : kRoundTrip) {
        const std::string text = formatEditable(value);
        const ParsedNumber parsed = parseEngineering(text);
        if (!parsed.ok || std::fabs(parsed.value - value) > 1e-9 * std::max(1.0, value)) {
            roundTripOk = false;
        }
    }
    check(roundTripOk, "the editable text of a value parses back to the same value");

    // The editor opens 4.7 kΩ as "4.7" with the "k" prefix selected.
    const SiValue kilo = splitSI(4700.0, 0, 6);
    check(std::fabs(kilo.scaled - 4.7) < 1e-12 && kilo.exponent == 3,
          "4700 Ω is shown as 4.7 with the kilo prefix");
    const SiValue nano = splitSI(1e-7, -12, 3);
    check(std::fabs(nano.scaled - 100.0) < 1e-9 && nano.exponent == -9,
          "100 nF is shown as 100 with the nano prefix");
    const SiValue clamped = splitSI(1e-6, 0, 6);
    check(clamped.exponent == 0, "a prefix outside the field range falls back to none");
    check(siPrefix(3) == "k" && siPrefix(-6) == "µ" && siPrefix(0).empty(),
          "SI prefix letters match the formatted values");
}

}  // namespace

int main()
{
    testDividerNetlist();
    testDividerOperatingPoint();
    testRcTransient();
    testSwitchBulbCurrents();
    testAcSignalCurrents();
    testMultiPinLayout();
    testNpnSwitchAndPinCurrents();
    testDiodeForwardBias();
    testNmosSwitch();
    testOpAmpFollower();
    testLogicGate();
    testRegulator7805();
    testTimer555Astable();
    testFlowMultiPinBjt();
    testLedAndZener();
    testLoadDamageRecovery();
    testFlowDividerCurrents();
    testFlowSeparateGroundIslands();
    testFlowWireBetweenJunctions();
    testFlowBranchSplit();
    testProjectRoundTrip();
    testNewComponentProjectRoundTrip();
    testTimer555Example();
    testInvalidCircuit();

    testPwmDutyCycle();
    testPowerStageParts();
    testWaveMeasure();
    testMosSwitchBoard();
    testPowerPartsProjectRoundTrip();
    testMosSwitchBoardFlow();
    testMosSwitchBoardProjectFile();
    testSourceWiringErrors();
    testWireCrossings();
    testLoadRatings();
    testEngineeringInput();

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}
