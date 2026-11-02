#include "circuit/Engineering.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace circuitlab {
namespace {

const char* prefixForExponent(int exponent)
{
    switch (exponent) {
    case -12:
        return "p";
    case -9:
        return "n";
    case -6:
        return "µ";
    case -3:
        return "m";
    case 0:
        return "";
    case 3:
        return "k";
    case 6:
        return "M";
    case 9:
        return "G";
    default:
        return "";
    }
}

std::string trimZeros(std::string text)
{
    if (text.find('.') == std::string::npos && text.find('e') == std::string::npos) {
        return text;
    }
    while (!text.empty() && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text;
}

}  // namespace

std::string formatSI(double value, const char* unit, int significantDigits)
{
    if (!std::isfinite(value)) {
        return "—";
    }
    if (value == 0.0) {
        return std::string("0 ") + unit;
    }

    const bool negative = value < 0.0;
    double scaled = std::fabs(value);
    int exponent = 0;

    while (scaled >= 1000.0 && exponent < 9) {
        scaled /= 1000.0;
        exponent += 3;
    }
    while (scaled < 1.0 && exponent > -12) {
        scaled *= 1000.0;
        exponent -= 3;
    }

    char number[64];
    std::snprintf(number, sizeof(number), "%.*g", significantDigits, scaled);
    std::string text = trimZeros(number);
    if (negative) {
        text.insert(text.begin(), '-');
    }
    std::string result = text;
    const char* prefix = prefixForExponent(exponent);
    if (prefix[0] != '\0') {
        result += ' ';
        result += prefix;
    } else {
        result += ' ';
    }
    result += unit;
    return result;
}

std::string componentValueLabel(const Component& component)
{
    switch (component.type) {
    case ComponentType::Resistor:
        return formatSI(component.value, "Ω");
    case ComponentType::Bulb:
    case ComponentType::Fan:
        if (component.damaged) {
            return component.damagedByCurrent ? "过流损坏" : "过压损坏";
        }
        return formatSI(component.ratedVoltage, "V") + " / " +
               formatSI(loadRatedCurrent(component), "A");
    case ComponentType::Capacitor:
        return formatSI(component.value, "F");
    case ComponentType::Inductor:
        return formatSI(component.value, "H");
    case ComponentType::PowerInductor:
        return formatSI(component.value, "H") + " / " + formatSI(component.seriesR, "Ω");
    case ComponentType::ElectrolyticCap:
        return formatSI(component.value, "F") + " / " + formatSI(component.seriesR, "Ω");
    case ComponentType::Switch:
        return component.switchClosed ? "闭合" : "断开";
    case ComponentType::VoltageSource:
    case ComponentType::AcSource:
    case ComponentType::SignalSource:
    case ComponentType::PwmSource:
        switch (component.source.mode) {
        case Waveform::DC:
            return formatSI(component.source.dc, "V");
        case Waveform::Sine:
            return formatSI(component.source.amplitude, "V") + " " +
                   formatSI(component.source.frequency, "Hz");
        case Waveform::Pulse: {
            char text[96];
            std::snprintf(text, sizeof(text), "%s 方波 %s %d%%",
                          formatSI(component.source.amplitude, "V").c_str(),
                          formatSI(component.source.frequency, "Hz").c_str(),
                          static_cast<int>(component.source.duty * 100.0 + 0.5));
            return text;
        }
        }
        return formatSI(component.source.dc, "V");
    case ComponentType::Ground:
        return "GND";
    case ComponentType::Diode:
        return "1N4148";
    case ComponentType::Led:
        return "LED";
    case ComponentType::Zener:
        return formatSI(component.value, "V") + " 稳压";
    case ComponentType::SchottkyDiode:
        return "SS34";
    case ComponentType::Npn:
        return "2N3904";
    case ComponentType::Pnp:
        return "2N3906";
    case ComponentType::Nmos:
        return "2N7000";
    case ComponentType::PowerNmos:
        return "功率 N-MOS";
    case ComponentType::PowerPmos:
        return "功率 P-MOS";
    case ComponentType::OpAmp:
        return "理想运放";
    case ComponentType::Timer555:
        return "555";
    case ComponentType::GateAnd:
        return "AND";
    case ComponentType::GateOr:
        return "OR";
    case ComponentType::GateNot:
        return "NOT";
    case ComponentType::Regulator7805:
        return "5 V";
    case ComponentType::Oscilloscope:
        return formatSI(component.value, "Ω") + " 探头";
    case ComponentType::TerminalBlock:
        return "接线端子";
    }
    return {};
}

std::string formatAxis(double value, const char* unit)
{
    if (value == 0.0) {
        return std::string("0");
    }
    const std::string formatted = formatSI(value, unit, 3);
    // Axis labels stay compact: "1.2m" rather than "1.2 m V".
    std::string compact;
    for (char c : formatted) {
        if (c == ' ') {
            continue;
        }
        compact += c;
    }
    return compact;
}

}  // namespace circuitlab
