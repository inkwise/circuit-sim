#pragma once

#include "circuit/Circuit.h"

#include <string>

namespace circuitlab {

// Human-readable number with an SI prefix, e.g. 2500 -> "2.5 kΩ",
// 1e-6 -> "1 µF", 0.5 -> "500 m".
std::string formatSI(double value, const char* unit, int significantDigits = 3);

// A number that has been typed by the user into a value field.
struct ParsedNumber {
    bool ok = false;         // false when the text held no number at all
    bool hasPrefix = false;  // the text carried its own prefix / exponent
    double value = 0.0;
};

// Parses what a user types into a parameter field: plain numbers ("4700",
// "0.0000001"), SI prefixes ("4.7k", "100n", "1M", "1meg"), the "k2" / "R7"
// short forms ("1k2" = 1.2 kΩ, "4R7" = 4.7 Ω), scientific notation ("1e-3",
// "2.5E3"), a comma decimal separator ("4,7k") and full-width digits from a
// Chinese input method ("１２"). A unit word after the number (Ω, F, H, V, A,
// Hz, ohm, %) is ignored, so "10 kΩ" and "10k" mean the same thing.
ParsedNumber parseEngineering(const std::string& text);

// Plain number for editing in a text field, without an SI prefix:
// 4700 -> "4700", 1e-7 -> "0.0000001", 1e-9 -> "1e-09".
std::string formatEditable(double value);

// A value split into an SI prefix, e.g. 4700 -> {4.7, 3} for "4.7 k".
struct SiValue {
    double scaled = 0.0;
    int exponent = 0;
};

// Splits `value` onto the nearest SI prefix inside [minimumExponent,
// maximumExponent], so a field can open showing "4.7" with "k" preselected
// instead of "4700".
SiValue splitSI(double value, int minimumExponent = -12, int maximumExponent = 9);

// Single-letter SI prefix for an exponent: -9 -> "n", 0 -> "".
std::string siPrefix(int exponent);

// Label shown under a component on the schematic.
std::string componentValueLabel(const Component& component);

// A compact axis label without a trailing unit space.
std::string formatAxis(double value, const char* unit);

}  // namespace circuitlab
