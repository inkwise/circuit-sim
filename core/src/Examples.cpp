#include "circuit/Examples.h"

namespace circuitlab {

Circuit makeDividerDemo()
{
    Circuit circuit;
    const int v1 = circuit.addComponent(ComponentType::VoltageSource, 8, 10);
    if (Component* source = circuit.findComponent(v1)) {
        source->source.mode = Waveform::DC;
        source->source.dc = 5.0;
        source->source.amplitude = 5.0;
        source->value = 5.0;
    }

    const int r1 = circuit.addComponent(ComponentType::Resistor, 16, 8);
    if (Component* resistor = circuit.findComponent(r1)) {
        resistor->value = 10e3;
    }
    const int r2 = circuit.addComponent(ComponentType::Resistor, 16, 14);
    if (Component* resistor = circuit.findComponent(r2)) {
        resistor->value = 10e3;
    }
    circuit.addComponent(ComponentType::Ground, 8, 17);

    circuit.addWirePoints({Point{8, 8}, Point{16, 8}, Point{16, 6}});
    circuit.addWirePoints({Point{16, 10}, Point{16, 12}});
    circuit.addWirePoints({Point{8, 12}, Point{8, 16}});
    circuit.addWirePoints({Point{16, 16}, Point{8, 16}});
    return circuit;
}

Circuit makeTimer555Demo()
{
    Circuit circuit;
    const int source = circuit.addComponent(ComponentType::VoltageSource, 4, 10);
    if (Component* component = circuit.findComponent(source)) {
        component->source.mode = Waveform::DC;
        component->source.dc = 5.0;
        component->source.amplitude = 5.0;
        component->value = 5.0;
    }

    const int timer = circuit.addComponent(ComponentType::Timer555, 20, 10);
    (void)timer;

    const int ra = circuit.addComponent(ComponentType::Resistor, 28, 7);
    if (Component* component = circuit.findComponent(ra)) {
        component->value = 1000.0;
    }
    const int rb = circuit.addComponent(ComponentType::Resistor, 26, 11);
    if (Component* component = circuit.findComponent(rb)) {
        component->value = 1000.0;
        component->rotation = 90;
    }
    const int timing = circuit.addComponent(ComponentType::Capacitor, 22, 16);
    if (Component* component = circuit.findComponent(timing)) {
        component->value = 1e-6;
    }
    const int load = circuit.addComponent(ComponentType::Resistor, 10, 11);
    if (Component* component = circuit.findComponent(load)) {
        component->value = 10e3;
        component->rotation = 90;
    }

    circuit.addComponent(ComponentType::Ground, 4, 13);
    circuit.addComponent(ComponentType::Ground, 16, 6);
    circuit.addComponent(ComponentType::Ground, 22, 19);
    circuit.addComponent(ComponentType::Ground, 8, 12);

    // VCC rail and RESET pull-up.
    circuit.addWirePoints({Point{4, 8}, Point{4, 4}});
    circuit.addWirePoints({Point{4, 4}, Point{30, 4}});
    circuit.addWirePoints({Point{24, 4}, Point{24, 7}});
    circuit.addWirePoints({Point{16, 13}, Point{16, 20}});
    circuit.addWirePoints({Point{16, 20}, Point{30, 20}});
    circuit.addWirePoints({Point{30, 20}, Point{30, 4}});
    circuit.addWirePoints({Point{16, 7}, Point{16, 5}});

    // RA / RB timing network.
    circuit.addWirePoints({Point{28, 4}, Point{28, 5}});
    circuit.addWirePoints({Point{28, 9}, Point{24, 9}});
    circuit.addWirePoints({Point{28, 9}, Point{28, 11}});

    // Timing capacitor, TRIG tied to THRES, and output load.
    circuit.addWirePoints({Point{24, 11}, Point{22, 11}});
    circuit.addWirePoints({Point{22, 11}, Point{22, 14}});
    circuit.addWirePoints({Point{16, 9}, Point{14, 9}});
    circuit.addWirePoints({Point{14, 9}, Point{14, 6}});
    circuit.addWirePoints({Point{14, 6}, Point{26, 6}});
    circuit.addWirePoints({Point{26, 6}, Point{26, 8}});
    circuit.addWirePoints({Point{26, 8}, Point{22, 8}});
    circuit.addWirePoints({Point{22, 8}, Point{22, 11}});
    circuit.addWirePoints({Point{16, 11}, Point{12, 11}});
    return circuit;
}

Circuit makeRcDemo()
{    Circuit circuit;
    const int v1 = circuit.addComponent(ComponentType::SignalSource, 8, 10);
    if (Component* source = circuit.findComponent(v1)) {
        source->source.mode = Waveform::Pulse;
        source->source.dc = 0.0;
        source->source.amplitude = 5.0;
        source->source.frequency = 100.0;
        source->source.rise = 1e-6;
        source->source.pulseWidth = 5e-3;
        source->source.period = 10e-3;
        source->value = 5.0;
    }

    const int r1 = circuit.addComponent(ComponentType::Resistor, 16, 8);
    if (Component* resistor = circuit.findComponent(r1)) {
        resistor->value = 1e3;
    }
    const int c1 = circuit.addComponent(ComponentType::Capacitor, 16, 14);
    if (Component* capacitor = circuit.findComponent(c1)) {
        capacitor->value = 1e-6;
    }
    circuit.addComponent(ComponentType::Ground, 8, 17);

    circuit.addWirePoints({Point{8, 8}, Point{16, 8}, Point{16, 6}});
    circuit.addWirePoints({Point{16, 10}, Point{16, 12}});
    circuit.addWirePoints({Point{8, 12}, Point{8, 16}});
    circuit.addWirePoints({Point{16, 16}, Point{8, 16}});
    return circuit;
}

// A 12 V PWM switch board with an output LC filter: the PWM signal drives an
// NPN inverter, the inverter pulls the gate of a high-side P-MOSFET low, the
// chopper node feeds an L-C filter through a Schottky freewheel diode, and the
// oscilloscope probe reads the result. Vout settles at roughly duty * 12 V with
// a switching ripple of a few tens of millivolts.
Circuit makeThermoelectricDemo()
{
    Circuit circuit;
    const int supply = circuit.addComponent(ComponentType::VoltageSource, 10, 11);
    if (Component* source = circuit.findComponent(supply)) {
        source->source.mode = Waveform::DC;
        source->source.dc = 12.0;
        source->source.amplitude = 12.0;
        source->value = 12.0;
    }
    const int cooler = circuit.addComponent(ComponentType::Tec12706, 20, 11);
    if (Component* component = circuit.findComponent(cooler)) {
        // A small load sitting on the cold face: 8 W leaking back in.
        component->heatLoad = 8.0;
        component->coldStart = 20.0;
        applyLoadDefaults(*component);
    }
    circuit.addComponent(ComponentType::Ground, 10, 18);

    circuit.addWirePoints({Point{10, 9}, Point{20, 9}});
    circuit.addWirePoints({Point{20, 13}, Point{20, 17}, Point{10, 17}});
    circuit.addWirePoints({Point{10, 13}, Point{10, 17}});
    return circuit;
}

Circuit makeMosSwitchBoardDemo()
{
    Circuit circuit;

    // The layout keeps the four functional blocks (input, control, power stage,
    // output) well apart. Where the PWM line and the gate line have to run past
    // other wires they cross without touching, and the app draws those spots as
    // jumper bridges on the wire that was added later.

    // --- Block 1: bench supply, source resistance, input terminal, bulk cap
    const int supply = circuit.addComponent(ComponentType::VoltageSource, 4, 14);
    if (Component* source = circuit.findComponent(supply)) {
        source->source.mode = Waveform::DC;
        source->source.dc = 12.0;
        source->source.amplitude = 12.0;
        source->value = 12.0;
    }
    const int sourceResistance = circuit.addComponent(ComponentType::Resistor, 4, 6);
    if (Component* resistor = circuit.findComponent(sourceResistance)) {
        // Supply + cable resistance: it also damps the input LC.
        resistor->value = 0.2;
    }
    circuit.addComponent(ComponentType::TerminalBlock, 14, 6);
    const int inputCap = circuit.addComponent(ComponentType::ElectrolyticCap, 22, 10);
    if (Component* part = circuit.findComponent(inputCap)) {
        part->value = 100e-6;
        part->seriesR = 0.15;
    }

    // --- Block 2: MCU PWM input and the gate driver
    const int signalTerminal = circuit.addComponent(ComponentType::TerminalBlock, 10, 26);
    if (Component* terminal = circuit.findComponent(signalTerminal)) {
        terminal->rotation = 90;
    }
    const int pwm = circuit.addComponent(ComponentType::PwmSource, 20, 34);
    if (Component* source = circuit.findComponent(pwm)) {
        source->source.mode = Waveform::Pulse;
        source->source.dc = 0.0;
        source->source.amplitude = 3.3;   // 3.3 V MCU output
        source->source.frequency = 50e3;
        source->source.period = 20e-6;
        source->source.duty = 0.5;
        source->source.pulseWidth = 10e-6;
        source->source.rise = 50e-9;
        source->value = 3.3;
    }
    const int baseResistor = circuit.addComponent(ComponentType::Resistor, 30, 26);
    if (Component* resistor = circuit.findComponent(baseResistor)) {
        resistor->rotation = 90;
        resistor->value = 1e3;
    }
    circuit.addComponent(ComponentType::Npn, 36, 32);
    const int pullUp = circuit.addComponent(ComponentType::Resistor, 58, 6);
    if (Component* resistor = circuit.findComponent(pullUp)) {
        // A stiff pull-up keeps the gate rise time (and therefore the duty
        // cycle error of the asymmetric drive) small at 50 kHz.
        resistor->value = 330.0;
    }

    // --- Block 3: high-side switch, freewheel diode and the inductor
    const int switchMos = circuit.addComponent(ComponentType::PowerPmos, 52, 10);
    if (Component* mos = circuit.findComponent(switchMos)) {
        mos->rotation = 180;
    }
    const int freewheel = circuit.addComponent(ComponentType::SchottkyDiode, 44, 18);
    if (Component* diode = circuit.findComponent(freewheel)) {
        diode->rotation = 180;
    }
    const int inductor = circuit.addComponent(ComponentType::PowerInductor, 58, 18);
    if (Component* part = circuit.findComponent(inductor)) {
        part->value = 100e-6;
        part->seriesR = 0.06;
    }

    // --- Block 4: output filter, real load, probes and the output terminal
    const int bulkCap = circuit.addComponent(ComponentType::ElectrolyticCap, 66, 36);
    if (Component* part = circuit.findComponent(bulkCap)) {
        part->value = 220e-6;
        part->seriesR = 0.12;
    }
    const int ceramicCap = circuit.addComponent(ComponentType::Capacitor, 74, 36);
    if (Component* part = circuit.findComponent(ceramicCap)) {
        part->value = 100e-9;
    }
    const int load = circuit.addComponent(ComponentType::Bulb, 82, 36);
    if (Component* part = circuit.findComponent(load)) {
        // 6 V / 6 W lamp for the ~6 V output: 1 A nominal, needs about 3 V to
        // start. The limits leave room for the start-up overshoot of the
        // open-loop filter (peak ~8 V here), which a real lamp tolerates.
        part->value = 6.0;
        part->ratedVoltage = 6.0;
        part->minVoltage = 3.0;
        part->maxVoltage = 9.0;
        part->maxCurrent = 1.6;
    }
    // SC1 stays soldered to the output; SC2 is left for the user to wire to any
    // node (its CH1 pin is intentionally unconnected).
    circuit.addComponent(ComponentType::Oscilloscope, 90, 36);
    circuit.addComponent(ComponentType::TerminalBlock, 98, 34);
    circuit.addComponent(ComponentType::Oscilloscope, 106, 34);
    circuit.addComponent(ComponentType::Ground, 4, 48);

    // Rails: 12 V along the top, ground along the bottom.
    circuit.addWirePoints({Point{4, 2}, Point{66, 2}});
    circuit.addWirePoints({Point{4, 44}, Point{106, 44}});
    circuit.addWirePoints({Point{4, 44}, Point{4, 47}});

    // Supply, source resistance, input terminal and input bulk capacitor.
    circuit.addWirePoints({Point{4, 12}, Point{4, 8}});
    circuit.addWirePoints({Point{4, 4}, Point{4, 2}});
    circuit.addWirePoints({Point{4, 16}, Point{4, 44}});
    circuit.addWirePoints({Point{14, 4}, Point{14, 2}});
    circuit.addWirePoints({Point{14, 8}, Point{14, 44}});
    circuit.addWirePoints({Point{22, 8}, Point{22, 2}});
    circuit.addWirePoints({Point{22, 12}, Point{22, 44}});

    // Power stage first, so the control lines added later are the ones that get
    // the jumper bridges: source to 12 V, drain to the chopper node, diode to
    // ground.
    circuit.addWirePoints({Point{50, 8}, Point{50, 2}});
    circuit.addWirePoints({Point{50, 12}, Point{58, 12}});
    circuit.addWirePoints({Point{58, 12}, Point{58, 16}});
    circuit.addWirePoints({Point{44, 16}, Point{44, 12}});
    circuit.addWirePoints({Point{44, 12}, Point{50, 12}});
    circuit.addWirePoints({Point{44, 20}, Point{44, 44}});

    // PWM input runs to the right past the two input drops (drawn as jumper
    // bridges), then down into the PWM generator and the base resistor.
    circuit.addWirePoints({Point{12, 26}, Point{20, 26}});
    circuit.addWirePoints({Point{20, 26}, Point{28, 26}});
    circuit.addWirePoints({Point{8, 26}, Point{8, 44}});
    circuit.addWirePoints({Point{20, 26}, Point{20, 32}});
    circuit.addWirePoints({Point{20, 36}, Point{20, 44}});
    circuit.addWirePoints({Point{32, 26}, Point{32, 32}});
    circuit.addWirePoints({Point{32, 32}, Point{34, 32}});
    circuit.addWirePoints({Point{38, 34}, Point{38, 44}});

    // Gate drive: collector up, across the freewheel diode drop (jumper), then
    // up over the chopper node (jumper) into the gate.
    circuit.addWirePoints({Point{38, 30}, Point{38, 24}});
    circuit.addWirePoints({Point{38, 24}, Point{54, 24}});
    circuit.addWirePoints({Point{54, 24}, Point{54, 10}});
    circuit.addWirePoints({Point{58, 4}, Point{58, 2}});
    circuit.addWirePoints({Point{58, 8}, Point{58, 10}});
    circuit.addWirePoints({Point{58, 10}, Point{54, 10}});

    // Output rail with the filter, the lamp, the probe and the terminals.
    circuit.addWirePoints({Point{58, 20}, Point{58, 30}});
    circuit.addWirePoints({Point{58, 30}, Point{98, 30}});
    circuit.addWirePoints({Point{66, 30}, Point{66, 34}});
    circuit.addWirePoints({Point{66, 38}, Point{66, 44}});
    circuit.addWirePoints({Point{74, 30}, Point{74, 34}});
    circuit.addWirePoints({Point{74, 38}, Point{74, 44}});
    circuit.addWirePoints({Point{82, 30}, Point{82, 34}});
    circuit.addWirePoints({Point{82, 38}, Point{82, 44}});
    circuit.addWirePoints({Point{90, 30}, Point{90, 34}});
    circuit.addWirePoints({Point{90, 38}, Point{90, 44}});
    circuit.addWirePoints({Point{98, 30}, Point{98, 32}});
    circuit.addWirePoints({Point{98, 36}, Point{98, 44}});

    // SC2: ground clip wired to the board ground, CH1 left free for the user.
    circuit.addWirePoints({Point{106, 36}, Point{106, 44}});

    return circuit;
}

}  // namespace circuitlab
