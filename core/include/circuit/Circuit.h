#pragma once

#include <array>
#include <string>
#include <vector>

namespace circuitlab {

struct Point {
    int x = 0;
    int y = 0;
};

inline bool operator==(const Point& a, const Point& b) { return a.x == b.x && a.y == b.y; }
inline bool operator!=(const Point& a, const Point& b) { return !(a == b); }

// Upper bound for every component, including the 8-pin 555.
constexpr int kMaxComponentPins = 8;

// The catalog array in ComponentCatalog.cpp is indexed by this enum, so the
// order here must stay in sync with the order of the catalog entries.
enum class ComponentType {
    Resistor,
    Capacitor,
    Inductor,
    VoltageSource,   // DC source by default, may use another waveform
    AcSource,        // mains-style sinusoidal source
    SignalSource,    // bench signal generator (sine / square)
    Switch,
    Bulb,
    Fan,
    Ground,
    Diode,
    Led,
    Zener,
    Npn,
    Pnp,
    Nmos,
    // A named logic-level power MOSFET, listed with the other transistors.
    Irl540N,         // IRL540N: 100 V / 28 A / 44 mΩ, on-resistance fitted
    OpAmp,
    Timer555,
    GateAnd,
    GateOr,
    GateNot,
    Regulator7805,
    // Switching-regulator parts: a PWM driven power switch, its freewheel
    // diode and the LC filter that turns the chopped rail into a DC rail.
    PowerInductor,   // inductor with winding resistance (L + DCR)
    ElectrolyticCap, // bulk capacitor with equivalent series resistance
    PwmSource,       // PWM generator with an adjustable duty cycle
    PowerNmos,       // logic-level power N-MOSFET (low-side switch)
    PowerPmos,       // power P-MOSFET (high-side switch)
    SchottkyDiode,   // low forward-drop freewheel diode
    // Instruments and connectors.
    Oscilloscope,    // 1 MΩ differential probe with ripple statistics
    TerminalBlock,   // 2-pin screw terminal (electrically a short)
    // Peltier coolers: a DC load whose current falls as the cold face gets
    // cold, with a lumped thermal model of that cold face.
    Tec12706,        // TEC1-12706, 12 V / 6 A, Qmax 57 W
    Tec12710,        // TEC1-12710, 12 V / 10.5 A, Qmax 92 W
    // The parts a real board uses: a known logic-level MOSFET, a ceramic
    // (MLCC) capacitor with its ESL, and a power resistor with a power rating.
    CeramicCap,      // MLCC: C + ESR + ESL (series RLC, what decoupling is)
    PowerResistor,   // wirewound / cement resistor: R + winding L + power rating
};

enum class Waveform {
    DC,
    Sine,
    Pulse,
};

struct SourceSpec {
    Waveform mode = Waveform::DC;
    double dc = 5.0;          // DC value / sine offset / pulse low level
    double amplitude = 5.0;   // sine amplitude / pulse high level
    double frequency = 1000.0;
    double rise = 1e-6;
    double pulseWidth = 10e-3;
    double period = 20e-3;
    // Duty cycle of a pulse source. It is authoritative in the UI and is kept
    // in sync with pulseWidth = duty * period.
    double duty = 0.5;
};

struct Component {
    int id = 0;
    ComponentType type = ComponentType::Resistor;
    std::string name;
    double value = 1000.0;    // R (ohm), C (F), L (H); unused for sources/ground
    SourceSpec source;
    bool switchClosed = true;
    double ratedVoltage = 0.0;  // loads only: nominal voltage (full brightness/speed)
    // Load ratings, modelled on real parts: below `minVoltage` the part does not
    // start, above `maxVoltage` (overvoltage) or `maxCurrent` (overcurrent) it is
    // destroyed. `maxVoltage` is what "a fan may be overdriven by 10%" means.
    double minVoltage = 0.0;
    double maxVoltage = 0.0;
    double maxCurrent = 0.0;
    // Nominal electrical spec of a load: a lamp is a "12 V / 12 W" part and a
    // Peltier module a "12 V / 6 A" one. They are kept consistent with the
    // equivalent resistance (R = V / I, P = V * I), and all three are editable.
    double ratedCurrent = 0.0;  // A
    double ratedPower = 0.0;    // W
    bool damaged = false;       // loads only: open after a fault; recoverable
    bool damagedByCurrent = false;  // true when the fault was overcurrent
    // Peltier modules only: heat leaking into the cold face (W) and the cold
    // face temperature at the start of the transient (degC).
    double heatLoad = 0.0;
    double coldStart = 20.0;
    // Parts placed before the symbols shrank keep the old, twice-as-large pin
    // spacing, so the wires that were drawn around them still line up. Any part
    // placed now is drawn and wired at the compact size.
    bool legacySymbol = false;
    // Series resistance of a real part: winding resistance of a power inductor
    // (DCR) or the ESR of an electrolytic capacitor. 0 means ideal.
    double seriesR = 0.0;
    // Series inductance of a real part: the ESL of a ceramic capacitor, or the
    // winding inductance of a wirewound resistor. 0 means ideal.
    double seriesL = 0.0;
    int gridX = 0;
    int gridY = 0;
    int rotation = 0;         // 0, 90, 180, 270
    // Mirror the symbol around its vertical axis before rotating it. Rotation
    // alone cannot put a transistor's base on the other side, or flip a diode
    // that is already upside down.
    bool mirrored = false;
};

struct Wire {
    int id = 0;
    std::vector<Point> points;
};

// Number of electrical pins (ground has one).
int pinCount(ComponentType type);

// World/grid coordinates of each pin, in netlist order. `out` must have room
// for kMaxComponentPins entries. Returns the number of pins written.
int componentPins(const Component& component, Point out[kMaxComponentPins]);

// Half extents of the symbol body in grid cells, with the rotation applied and
// the legacy (twice as large) size taken into account.
void componentBodyHalfExtents(const Component& component, int* halfWidth, int* halfHeight);

// Axis-aligned bounds in grid units, with a small margin for the symbol body.
void componentBounds(const Component& component, Point* minOut, Point* maxOut);

const char* componentPrefix(ComponentType type);
const char* componentTypeKey(ComponentType type);   // stable English key for files
bool componentTypeFromKey(const std::string& key, ComponentType* out);
const char* waveformKey(Waveform waveform);
bool waveformFromKey(const std::string& key, Waveform* out);

bool isSourceType(ComponentType type);
// True for the visual loads (bulb, fan, Peltier module).
bool isLoadType(ComponentType type);
// True for every part that can be destroyed by running it outside its ratings:
// the visual loads plus a resistor with a power rating.
bool isBurnableType(ComponentType type);
// Fills the load ratings (start-up / maximum voltage / maximum current) with
// realistic defaults when they are missing, e.g. for projects saved before the
// ratings existed.
void applyLoadDefaults(Component& component);

// Why a part failed; None means it is operating inside its ratings.
enum class LoadFault {
    None,
    OverVoltage,
    OverCurrent,
    // Only parts whose limit is a power rating (a wirewound / cement resistor).
    OverPower,
};

// Fault a load would suffer at this terminal voltage and current.
LoadFault loadFault(const Component& component, double voltage, double current);

// Operating level of a load, 0..1. It is 0 below the start-up voltage, follows
// the electrical power above it and saturates at the nominal point, so a lamp
// dims smoothly and a motor spins up with voltage.
double loadLevel(const Component& component, double voltage);

// Nominal current and power of a load: the stored spec when it has one,
// otherwise derived from ratedVoltage and the equivalent resistance.
double loadRatedCurrent(const Component& component);
double loadRatedPower(const Component& component);

// The four specs of a load are linked by R = V / I and P = V * I. Editing one
// recomputes the others, so the parameter card can never show a load whose
// numbers contradict its resistance.
// - A new rated voltage keeps the resistance (same filament / winding), so the
//   current and power follow.
void setLoadRatedVoltage(Component& component, double volts);
// - A new rated current or power is the spec edit: the resistance follows.
void setLoadRatedCurrent(Component& component, double amps);
void setLoadRatedPower(Component& component, double watts);
// - A new resistance keeps the rated voltage.
void setLoadResistance(Component& component, double ohms);
// Recomputes the stored current/power from the voltage and the resistance.
void syncLoadSpec(Component& component);

// True for the measuring probe (oscilloscope with a 1 MΩ input).
bool isProbeType(ComponentType type);
// True for connectors / terminal blocks, which are modelled as a short.
bool isConnectorType(ComponentType type);
// Clears overvoltage damage on all loads and bumps the revision when anything
// was restored. Returns the number of recovered loads.
struct Circuit;
int clearLoadDamage(Circuit& circuit);
// True for devices represented by a SPICE resistor (R, switch, bulb, fan).
bool isResistiveType(ComponentType type);
// Resistance used by the netlist / current fallback. Switches are nearly a
// short when closed and nearly open when open.
double componentResistance(const Component& component);

// Round a world coordinate to the nearest grid point.
Point snapPoint(float x, float y);

// The bundled example circuits are written in the old, twice-as-large layout.
// This maps their wire endpoints onto the current (half-size) pin positions and
// re-routes them, so the shipped examples use the compact symbols.
void migrateLegacyPinSpacing(Circuit& circuit);

// Projects saved before the symbols shrank are shrunk here: the wiring is
// migrated and then checked, and only when every pin stays on the same net does
// the circuit keep the compact size. Returns true when the circuit was shrunk;
// false means the caller should fall back to `legacySymbol` (same circuit, old
// pin spacing) so nothing comes loose.
bool shrinkLegacyGeometry(Circuit& circuit);

// True when both circuits connect the same pins to each other (used to validate
// a geometry migration).
bool sameConnectivity(const Circuit& before, const Circuit& after);

struct Circuit {
    std::vector<Component> components;
    std::vector<Wire> wires;
    int revision = 0;
    int nextComponentId = 1;
    int nextWireId = 1;

    void clear();
    bool empty() const { return components.empty() && wires.empty(); }

    Component* findComponent(int id);
    const Component* findComponent(int id) const;
    Wire* findWire(int id);
    const Wire* findWire(int id) const;

    // Adds a component with an auto-generated name (R1, C1, V1, ...).
    int addComponent(ComponentType type, int gridX, int gridY);
    int addWire(Point a, Point b);
    int addWirePoints(std::vector<Point> points);

    void removeComponent(int id);   // also removes wires attached to its pins
    void removeWire(int id);

    // Moves the component and follows wire endpoints that were attached to its
    // old pin positions.
    bool moveComponent(int id, int newGridX, int newGridY);
    // Rotates 90 degrees and follows attached wire endpoints.
    bool rotateComponent(int id);
    // Mirrors the symbol left-to-right (the rotation still applies on top) and
    // moves attached wire endpoints with the pins.
    bool mirrorComponent(int id);

    void touch() { ++revision; }

private:
    std::string uniqueName(ComponentType type) const;
    void followPins(int componentId, const Point oldPins[kMaxComponentPins],
                    int oldCount, const Point newPins[kMaxComponentPins],
                    int newCount);
    static void normalizeWire(Wire& wire);
};

// Two wires whose bodies cross without touching electrically. The wire that was
// added first is drawn straight; the later one hops over the crossing point, so
// the schematic never suggests a connection that does not exist.
struct WireCrossing {
    int straightWireId = 0;   // earlier wire, drawn straight through
    int hoppingWireId = 0;    // later wire, drawn with a bridge over the point
    int straightWireIndex = 0;
    int hoppingWireIndex = 0;
    Point point;
};

// Finds every interior/interior intersection between two wires. Endpoint
// contacts (T junctions) and coincident vertices are real connections and are
// deliberately not reported.
std::vector<WireCrossing> findWireCrossings(const Circuit& circuit);

}  // namespace circuitlab
