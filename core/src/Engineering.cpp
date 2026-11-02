#include "circuit/Engineering.h"

#include "circuit/Thermoelectric.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

bool isAsciiDigit(char c)
{
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

// Rewrites typed text into plain ASCII so one parser can handle every input
// method: full-width digits/points from a Chinese IME become ASCII, the micro
// sign in both spellings becomes "u", and any other symbol (Ω, the unit words
// and so on) becomes a separator.
std::string normalizeTypedNumber(const std::string& text)
{
    std::string result;
    result.reserve(text.size());
    for (size_t index = 0; index < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        unsigned int codePoint = lead;
        size_t length = 1;
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
            codePoint = lead & 0x1Fu;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            codePoint = lead & 0x0Fu;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            codePoint = lead & 0x07u;
        }
        if (length > 1) {
            if (index + length > text.size()) {
                break;
            }
            for (size_t extra = 1; extra < length; ++extra) {
                codePoint = (codePoint << 6) |
                            (static_cast<unsigned char>(text[index + extra]) & 0x3Fu);
            }
        }
        index += length;

        if (codePoint < 0x80) {
            result += static_cast<char>(codePoint);
        } else if (codePoint >= 0xFF01 && codePoint <= 0xFF5E) {
            // Full-width ASCII forms (１２３．－) typed with a Chinese IME.
            result += static_cast<char>(codePoint - 0xFEE0);
        } else if (codePoint == 0x3000) {
            result += ' ';
        } else if (codePoint == 0x00B5 || codePoint == 0x03BC) {
            // U+00B5 MICRO SIGN and U+03BC GREEK SMALL LETTER MU.
            result += 'u';
        } else {
            result += ' ';
        }
    }
    return result;
}

// Comma decimal separator versus thousands separator: "4,7k" is 4.7 kΩ while
// "1,000" is one thousand. A comma followed by exactly three digits is read as
// a thousands separator; anything else is treated as a decimal point.
void normalizeCommas(std::string* text)
{
    std::string& value = *text;
    if (value.find('.') != std::string::npos) {
        // A real decimal point is already there: commas are thousands groups.
        std::string cleaned;
        for (char c : value) {
            if (c != ',') {
                cleaned += c;
            }
        }
        value = cleaned;
        return;
    }
    std::string cleaned;
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] != ',') {
            cleaned += value[index];
            continue;
        }
        size_t digits = 0;
        while (index + 1 + digits < value.size() && isAsciiDigit(value[index + 1 + digits])) {
            ++digits;
        }
        if (digits == 3) {
            continue;  // Thousands separator: drop it.
        }
        cleaned += (cleaned.find('.') == std::string::npos) ? '.' : ',';
    }
    value = cleaned;
}

// Scale for a prefix letter. `meg` is handled before this is called.
bool prefixScaleForLetter(char letter, double* scale, bool* recognised)
{
    switch (letter) {
    case 'p':
        *scale = 1e-12;
        *recognised = true;
        return true;
    case 'n':
    case 'N':
        *scale = 1e-9;
        *recognised = true;
        return true;
    case 'u':
    case 'U':
        *scale = 1e-6;
        *recognised = true;
        return true;
    case 'm':
        *scale = 1e-3;
        *recognised = true;
        return true;
    case 'r':
    case 'R':
        // The resistor marker: "4R7" reads as 4.7 Ω.
        *scale = 1.0;
        *recognised = true;
        return true;
    case 'k':
    case 'K':
        *scale = 1e3;
        *recognised = true;
        return true;
    case 'M':
        *scale = 1e6;
        *recognised = true;
        return true;
    case 'G':
        *scale = 1e9;
        *recognised = true;
        return true;
    default:
        break;
    }
    *recognised = false;
    return false;
}

bool startsWithIgnoreCase(const std::string& text, size_t index, const char* word)
{
    const size_t length = std::strlen(word);
    if (index + length > text.size()) {
        return false;
    }
    for (size_t offset = 0; offset < length; ++offset) {
        const char a = static_cast<char>(
            std::tolower(static_cast<unsigned char>(text[index + offset])));
        if (a != static_cast<char>(std::tolower(static_cast<unsigned char>(word[offset])))) {
            return false;
        }
    }
    return true;
}

}  // namespace

ParsedNumber parseEngineering(const std::string& text)
{
    ParsedNumber result;

    // Strip every kind of space: "10 kΩ", "1 000" and tab-pasted values all
    // read the same way.
    std::string value;
    const std::string normalized = normalizeTypedNumber(text);
    for (char c : normalized) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            value += c;
        }
    }
    normalizeCommas(&value);

    size_t index = 0;
    const size_t length = value.size();
    bool negative = false;
    if (index < length && (value[index] == '+' || value[index] == '-')) {
        negative = value[index] == '-';
        ++index;
    }

    const size_t start = index;
    bool anyDigit = false;
    while (index < length && isAsciiDigit(value[index])) {
        ++index;
        anyDigit = true;
    }
    if (index < length && (value[index] == '.' || value[index] == ',')) {
        ++index;
        while (index < length && isAsciiDigit(value[index])) {
            ++index;
            anyDigit = true;
        }
    }
    if (!anyDigit) {
        return result;
    }
    double mantissa = std::strtod(value.substr(start, index - start).c_str(), nullptr);

    bool hasPrefix = false;
    double scale = 1.0;

    // Scientific notation: 1e-3, 2.5E3.
    if (index < length && (value[index] == 'e' || value[index] == 'E')) {
        size_t cursor = index + 1;
        int sign = 1;
        if (cursor < length && (value[cursor] == '+' || value[cursor] == '-')) {
            sign = value[cursor] == '-' ? -1 : 1;
            ++cursor;
        }
        const size_t digitsStart = cursor;
        while (cursor < length && isAsciiDigit(value[cursor])) {
            ++cursor;
        }
        if (cursor > digitsStart) {
            const int exponent =
                std::atoi(value.substr(digitsStart, cursor - digitsStart).c_str());
            mantissa *= std::pow(10.0, static_cast<double>(sign * exponent));
            hasPrefix = true;
            index = cursor;
        }
    }

    if (!hasPrefix && index < length) {
        double prefixScale = 1.0;
        bool recognised = false;
        size_t letters = 1;
        if (startsWithIgnoreCase(value, index, "meg")) {
            prefixScale = 1e6;
            recognised = true;
            letters = 3;
        } else if (startsWithIgnoreCase(value, index, "oh")) {
            // "ohm" / "Ohm" is a unit, not a prefix.
            letters = 0;
        } else {
            prefixScaleForLetter(value[index], &prefixScale, &recognised);
        }
        if (letters > 0 && recognised) {
            index += letters;
            hasPrefix = true;
            // The 4k7 / 4n7 / 4R7 short form: trailing digits are the fraction.
            size_t fractionDigits = 0;
            double fraction = 0.0;
            while (index < length && fractionDigits < 3 && isAsciiDigit(value[index])) {
                fraction = fraction * 10.0 + static_cast<double>(value[index] - '0');
                ++fractionDigits;
                ++index;
            }
            if (fractionDigits > 0) {
                mantissa += fraction / std::pow(10.0, static_cast<double>(fractionDigits));
            }
            scale = prefixScale;
        }
    }

    // Whatever is left is a unit word (Ω, F, H, Hz, %, ohm): ignored on
    // purpose, so the user may type the unit as well.
    const double parsed = mantissa * scale;
    if (!std::isfinite(parsed)) {
        return result;
    }
    result.ok = true;
    result.hasPrefix = hasPrefix;
    result.value = negative ? -parsed : parsed;
    return result;
}

std::string formatEditable(double value)
{
    if (!std::isfinite(value)) {
        return "0";
    }
    char buffer[64];
    const double magnitude = std::fabs(value);
    if (magnitude != 0.0 && (magnitude < 1e-4 || magnitude >= 1e7)) {
        std::snprintf(buffer, sizeof(buffer), "%.6g", value);
        return buffer;
    }
    std::snprintf(buffer, sizeof(buffer), "%.12f", value);
    std::string text = buffer;
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
    }
    if (text == "-0") {
        text = "0";
    }
    return text;
}

SiValue splitSI(double value, int minimumExponent, int maximumExponent)
{
    SiValue result;
    if (!std::isfinite(value)) {
        return result;
    }
    double magnitude = std::fabs(value);
    int exponent = 0;
    while (magnitude >= 1000.0 && exponent + 3 <= maximumExponent) {
        magnitude /= 1000.0;
        exponent += 3;
    }
    while (magnitude > 0.0 && magnitude < 1.0 && exponent - 3 >= minimumExponent) {
        magnitude *= 1000.0;
        exponent -= 3;
    }
    result.exponent = exponent;
    result.scaled = value < 0.0 ? -magnitude : magnitude;
    return result;
}

std::string siPrefix(int exponent)
{
    return prefixForExponent(exponent);
}


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
    case ComponentType::Tec12706:
    case ComponentType::Tec12710: {
        const ThermoelectricSpec* tec = thermoelectricSpec(component.type);
        if (component.damaged) {
            return component.damagedByCurrent ? "过流损坏" : "过压损坏";
        }
        return tec ? tec->model : "TEC";
    }
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
