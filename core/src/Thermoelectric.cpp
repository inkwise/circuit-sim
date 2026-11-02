#include "circuit/Thermoelectric.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace circuitlab {
namespace {

// Hot face temperature the model pins the heat sink to. A real cooler sits on
// an aluminium block that holds the hot ceramic near ambient; the app assumes
// that block never warms up, which keeps the model to a single thermal node.
constexpr double kHotSideCelsius = 20.0;
constexpr double kKelvin = 273.15;

// TEC1-12706 and TEC1-12710 differ only in pellet size. The coefficients below
// reproduce the datasheet points (Vmax 15.4 V, Imax 6 A / 10.5 A, dTmax 68 K,
// Qmax 57 W / 92 W) from the three classic Peltier relations
//   V = alpha*dT + I*R
//   Qc = alpha*I*Tc - I^2*R/2 - K*dT
// The heat capacity is deliberately tiny: a real 12706 cold plate holds about
// 15 J/K and would need a minute to settle, while the app's transient window is
// at most 50 ms. Shrinking the modelled mass keeps the curve visible; the
// temperatures it settles at are the physical ones.
const ThermoelectricSpec kTec12706 = {
    "TEC1-12706", 127,
    0.053,    // V/K
    1.98,     // ohm
    0.55,     // W/K
    0.0088,   // J/K  (models about 15 J/K in ~1/1700 of the time)
    15.4,     // V
    6.0,      // A
    57.0,     // W
    68.0,     // K
    12.0,     // V nominal
    6.0,      // A
    57.0,     // W
};

const ThermoelectricSpec kTec12710 = {
    "TEC1-12710", 127,
    0.0494,   // V/K
    1.147,    // ohm
    0.84,     // W/K
    0.0135,   // J/K
    15.4,     // V
    10.5,     // A
    92.0,     // W
    68.0,     // K
    12.0,     // V nominal
    10.5,     // A
    92.0,     // W
};

}  // namespace

const ThermoelectricSpec* thermoelectricSpec(ComponentType type)
{
    switch (type) {
    case ComponentType::Tec12706:
        return &kTec12706;
    case ComponentType::Tec12710:
        return &kTec12710;
    default:
        return nullptr;
    }
}

bool isThermoelectricType(ComponentType type)
{
    return thermoelectricSpec(type) != nullptr;
}

double thermoelectricHotSide()
{
    return kHotSideCelsius;
}

bool solveThermoelectric(const ThermoelectricSpec& spec, double appliedVoltage,
                         double hotC, double heatLoadW, ThermoelectricPoint* out)
{
    if (!out || !(spec.resistance > 0.0) || !(spec.seebeck > 0.0)) {
        return false;
    }
    const double hotKelvin = hotC + kKelvin;
    // Cooling capacity falls monotonically as the temperature difference grows,
    // so a bisection over the physically possible range is enough (and always
    // converges: the curve is smooth and each step halves the interval).
    const auto balance = [&](double deltaT) {
        const double coldKelvin = hotKelvin - deltaT;
        const double current = (appliedVoltage - spec.seebeck * deltaT) / spec.resistance;
        const double peltier = spec.seebeck * current * coldKelvin;
        const double joule = 0.5 * spec.resistance * current * current;
        return peltier - joule - spec.conductance * deltaT - heatLoadW;
    };
    double low = -250.0;             // cold face far below the hot face reversed
    double high = hotKelvin - 1.0;   // cold face just above absolute zero
    double lowValue = balance(low);
    double highValue = balance(high);
    if (!std::isfinite(lowValue) || !std::isfinite(highValue)) {
        return false;
    }
    if (lowValue < 0.0) {
        // Even a colder plate cannot balance the heat coming in: the module is
        // overwhelmed and there is no steady state.
        return false;
    }
    if (highValue > 0.0) {
        high = hotKelvin - 0.001;
        highValue = balance(high);
        if (!(highValue <= 0.0)) {
            return false;
        }
    }
    for (int iteration = 0; iteration < 200; ++iteration) {
        const double middle = 0.5 * (low + high);
        const double value = balance(middle);
        if (std::fabs(value) < 1e-9) {
            low = high = middle;
            break;
        }
        if (value > 0.0) {
            low = middle;
        } else {
            high = middle;
        }
    }
    const double deltaT = 0.5 * (low + high);
    ThermoelectricPoint point;
    point.deltaT = deltaT;
    point.coldTemperature = hotC - deltaT;
    point.current =
        (appliedVoltage - spec.seebeck * deltaT) / spec.resistance;
    point.voltage = appliedVoltage;
    const double coldKelvin = hotKelvin - deltaT;
    point.heatPumped = spec.seebeck * point.current * coldKelvin -
                       0.5 * spec.resistance * point.current * point.current -
                       spec.conductance * deltaT;
    point.inputPower = appliedVoltage * point.current;
    point.heatRejected = point.inputPower + point.heatPumped;
    point.cop = point.inputPower > 1e-12 ? point.heatPumped / point.inputPower : 0.0;
    point.reachable = spec.maxDeltaT > 0.0 ? deltaT / spec.maxDeltaT : 0.0;
    *out = point;
    return true;
}

void thermoelectricFromMeasurement(const ThermoelectricSpec& spec, double voltage,
                                   double current, double coldC,
                                   ThermoelectricPoint* out)
{
    if (!out) {
        return;
    }
    ThermoelectricPoint point;
    point.voltage = voltage;
    point.current = current;
    point.coldTemperature = coldC;
    point.deltaT = kHotSideCelsius - coldC;
    const double coldKelvin = coldC + kKelvin;
    point.heatPumped = spec.seebeck * current * coldKelvin -
                       0.5 * spec.resistance * current * current -
                       spec.conductance * point.deltaT;
    point.inputPower = voltage * current;
    point.heatRejected = point.inputPower + point.heatPumped;
    point.cop = point.inputPower > 1e-12 ? point.heatPumped / point.inputPower : 0.0;
    point.reachable = spec.maxDeltaT > 0.0 ? point.deltaT / spec.maxDeltaT : 0.0;
    *out = point;
}

std::string thermoelectricSubcircuitName(const Component& component)
{
    return "CL_TEC_" + component.name;
}

std::string thermoelectricSubcircuitCard(const Component& component)
{
    const ThermoelectricSpec* spec = thermoelectricSpec(component.type);
    if (!spec) {
        return {};
    }
    char buffer[2048];
    std::snprintf(
        buffer, sizeof(buffer),
        // Electrical branch: a 0 V sense source measures the module current and
        // the controlled source drops I*R plus the Seebeck back-EMF, so the
        // current really falls as the plate gets cold.
        ".subckt %s P N TC\n"
        "VSENSE P NXA 0\n"
        "BEMF NXA N V = %.6g*I(VSENSE) + %.6g*(%.6g - V(TC))\n"
        // Thermal branch: volts are degC. The hot ceramic is pinned to the heat
        // sink temperature, the cold ceramic has the modelled heat capacity and
        // receives half the Joule heat, the external heat load, and loses the
        // Peltier heat it pumps away.
        "RTH HOT TC %.6g\n"
        "VHOT HOT 0 %.6g\n"
        "CTH TC 0 %.6g\n"
        "BCOLD 0 TC I = 0.5*%.6g*I(VSENSE)*I(VSENSE) - %.6g*I(VSENSE)*(V(TC)+273.15) + %.6g\n"
        ".ends %s\n",
        thermoelectricSubcircuitName(component).c_str(), spec->resistance,
        spec->seebeck, kHotSideCelsius, 1.0 / spec->conductance, kHotSideCelsius,
        spec->coldCapacity, spec->resistance, spec->seebeck,
        std::max(0.0, component.heatLoad),
        thermoelectricSubcircuitName(component).c_str());
    return buffer;
}

}  // namespace circuitlab
