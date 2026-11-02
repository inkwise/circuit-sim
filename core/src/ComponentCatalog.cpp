#include "circuit/ComponentCatalog.h"

#include <initializer_list>

namespace circuitlab {
namespace {

struct CatalogEntry {
    ComponentInfo info;
    PinLayout layout;
};

CatalogEntry makeEntry(ComponentType type, ComponentCategory category,
                       SpiceFamily family, const char* typeKey, const char* label,
                       const char* description, const char* prefix,
                       std::initializer_list<PinDef> pins, int bodyHalfWidth,
                       int bodyHalfHeight, bool forcesTransient = false,
                       bool available = true)
{
    CatalogEntry entry;
    entry.info = ComponentInfo{type, category, family, typeKey, label, description,
                               prefix, forcesTransient, available};
    entry.layout.count = static_cast<int>(pins.size());
    int index = 0;
    for (const PinDef& pin : pins) {
        entry.layout.pins[static_cast<size_t>(index++)] = pin;
    }
    entry.layout.bodyHalfWidth = bodyHalfWidth;
    entry.layout.bodyHalfHeight = bodyHalfHeight;
    return entry;
}

const std::array<CatalogEntry, 35>& catalog()
{
    static const std::array<CatalogEntry, 35> entries = {{
        makeEntry(ComponentType::Resistor, ComponentCategory::Basic,
                  SpiceFamily::Resistor, "R", "电阻", "1 kΩ", "R",
                  {{0, -1, "A"}, {0, 1, "B"}}, 1, 1),
        makeEntry(ComponentType::Capacitor, ComponentCategory::Basic,
                  SpiceFamily::Capacitor, "C", "电容", "1 µF", "C",
                  {{0, -1, "A"}, {0, 1, "B"}}, 1, 1),
        makeEntry(ComponentType::Inductor, ComponentCategory::Basic,
                  SpiceFamily::Inductor, "L", "电感", "1 mH", "L",
                  {{0, -1, "A"}, {0, 1, "B"}}, 1, 1),
        makeEntry(ComponentType::VoltageSource, ComponentCategory::PowerSignal,
                  SpiceFamily::VoltageSource, "V", "直流电源", "5 V", "V",
                  {{0, -1, "A"}, {0, 1, "B"}}, 1, 1),
        makeEntry(ComponentType::AcSource, ComponentCategory::PowerSignal,
                  SpiceFamily::VoltageSource, "VAC", "交流电源", "12 V · 50 Hz", "V",
                  {{0, -1, "A"}, {0, 1, "B"}}, 1, 1),
        makeEntry(ComponentType::SignalSource, ComponentCategory::PowerSignal,
                  SpiceFamily::VoltageSource, "VSIG", "信号源", "5 V · 1 kHz", "V",
                  {{0, -1, "A"}, {0, 1, "B"}}, 1, 1),
        makeEntry(ComponentType::Switch, ComponentCategory::SwitchLoad,
                  SpiceFamily::Resistor, "SW", "开关", "可闭合 / 断开", "R",
                  {{0, -1, "A"}, {0, 1, "B"}}, 1, 1),
        makeEntry(ComponentType::Bulb, ComponentCategory::SwitchLoad,
                  SpiceFamily::Resistor, "BULB", "灯泡", "10 Ω 负载", "R",
                  {{0, -1, "A"}, {0, 1, "B"}}, 1, 1),
        makeEntry(ComponentType::Fan, ComponentCategory::SwitchLoad,
                  SpiceFamily::Resistor, "FAN", "风扇", "100 Ω 负载", "R",
                  {{0, -1, "A"}, {0, 1, "B"}}, 1, 1),
        makeEntry(ComponentType::Ground, ComponentCategory::PowerSignal,
                  SpiceFamily::Ground, "GND", "地线", "电路参考点", "GND",
                  {{0, -1, "GND"}}, 1, 1),
        makeEntry(ComponentType::Diode, ComponentCategory::Semiconductor,
                  SpiceFamily::Diode, "D", "二极管", "1N4148", "D",
                  {{0, -1, "A"}, {0, 1, "K"}}, 1, 1, false, true),
        makeEntry(ComponentType::Led, ComponentCategory::Semiconductor,
                  SpiceFamily::Diode, "LED", "LED", "红光 2 V", "D",
                  {{0, -1, "A"}, {0, 1, "K"}}, 1, 1, false, true),
        makeEntry(ComponentType::Zener, ComponentCategory::Semiconductor,
                  SpiceFamily::Diode, "DZ", "稳压二极管", "5.1 V", "D",
                  {{0, -1, "A"}, {0, 1, "K"}}, 1, 1, false, true),
        makeEntry(ComponentType::Npn, ComponentCategory::Transistor,
                  SpiceFamily::Bjt, "QNPN", "NPN 三极管", "2N3904", "Q",
                  {{1, -1, "C"}, {-1, 0, "B"}, {1, 1, "E"}}, 1, 1, false, true),
        makeEntry(ComponentType::Pnp, ComponentCategory::Transistor,
                  SpiceFamily::Bjt, "QPNP", "PNP 三极管", "2N3906", "Q",
                  {{1, -1, "C"}, {-1, 0, "B"}, {1, 1, "E"}}, 1, 1, false, true),
        makeEntry(ComponentType::Nmos, ComponentCategory::Transistor,
                  SpiceFamily::Mos, "NMOS", "N-MOS", "2N7000", "M",
                  {{1, -1, "D"}, {-1, 0, "G"}, {1, 1, "S"}}, 1, 1, false, true),
        makeEntry(ComponentType::OpAmp, ComponentCategory::AnalogIC,
                  SpiceFamily::Subcircuit, "OPAMP", "运放", "理想单运放", "X",
                  {{-2, -1, "IN+"}, {-2, 1, "IN-"}, {2, 0, "OUT"},
                   {0, -2, "V+"}, {0, 2, "V-"}}, 1, 1, false, true),
        makeEntry(ComponentType::Timer555, ComponentCategory::AnalogIC,
                  SpiceFamily::Subcircuit, "IC555", "555 定时器", "8 脚 DIP", "X",
                  {{-2, -3, "GND"}, {-2, -1, "TRIG"}, {-2, 1, "OUT"},
                   {-2, 3, "RESET"}, {2, 3, "CTRL"}, {2, 1, "THRES"},
                   {2, -1, "DISCH"}, {2, -3, "VCC"}}, 1, 3, true, true),
        makeEntry(ComponentType::GateAnd, ComponentCategory::Digital,
                  SpiceFamily::Subcircuit, "GAND", "AND 与门", "2 输入", "X",
                  {{-2, -1, "A"}, {-2, 1, "B"}, {2, 0, "Y"}}, 1, 1, false, true),
        makeEntry(ComponentType::GateOr, ComponentCategory::Digital,
                  SpiceFamily::Subcircuit, "GOR", "OR 或门", "2 输入", "X",
                  {{-2, -1, "A"}, {-2, 1, "B"}, {2, 0, "Y"}}, 1, 1, false, true),
        makeEntry(ComponentType::GateNot, ComponentCategory::Digital,
                  SpiceFamily::Subcircuit, "GNOT", "NOT 非门", "1 输入", "X",
                  {{-2, 0, "A"}, {2, 0, "Y"}}, 1, 1, false, true),
        makeEntry(ComponentType::Regulator7805, ComponentCategory::AnalogIC,
                  SpiceFamily::Subcircuit, "REG7805", "7805 稳压器", "5 V", "X",
                  {{-2, 0, "IN"}, {0, 2, "GND"}, {2, 0, "OUT"}}, 1, 1, false, true),
        makeEntry(ComponentType::PowerInductor, ComponentCategory::PowerFilter,
                  SpiceFamily::Inductor, "LDC", "功率电感",
                  "100 µH · DCR 60 mΩ", "L",
                  {{0, -1, "A"}, {0, 1, "B"}}, 1, 1),
        makeEntry(ComponentType::ElectrolyticCap, ComponentCategory::PowerFilter,
                  SpiceFamily::Capacitor, "CESR", "电解电容",
                  "220 µF · ESR 120 mΩ", "C",
                  {{0, -1, "A"}, {0, 1, "B"}}, 1, 1),
        makeEntry(ComponentType::PwmSource, ComponentCategory::PowerSignal,
                  SpiceFamily::VoltageSource, "VPWM", "PWM 信号源",
                  "5 V · 50 kHz · 50%", "V",
                  // A PWM generator is an ordinary pulse source: the transient
                  // window comes from the switching rule, not the 555 rule.
                  {{0, -1, "OUT"}, {0, 1, "GND"}}, 1, 1, false, true),
        makeEntry(ComponentType::PowerNmos, ComponentCategory::Transistor,
                  SpiceFamily::Mos, "NMOSP", "功率 N-MOS",
                  "低边开关 · 逻辑电平", "M",
                  {{1, -1, "D"}, {-1, 0, "G"}, {1, 1, "S"}}, 1, 1, false, true),
        makeEntry(ComponentType::PowerPmos, ComponentCategory::Transistor,
                  SpiceFamily::Mos, "PMOSP", "功率 P-MOS",
                  "高边开关", "M",
                  {{1, -1, "D"}, {-1, 0, "G"}, {1, 1, "S"}}, 1, 1, false, true),
        makeEntry(ComponentType::SchottkyDiode, ComponentCategory::Semiconductor,
                  SpiceFamily::Diode, "DSCH", "肖特基二极管",
                  "SS34 · 0.4 V 续流", "D",
                  {{0, -1, "A"}, {0, 1, "K"}}, 1, 1, false, true),
        makeEntry(ComponentType::Oscilloscope, ComponentCategory::Instrument,
                  SpiceFamily::Resistor, "SCOPE", "示波器",
                  "1 MΩ 探头 · 测纹波", "SC",
                  {{0, -1, "CH1"}, {0, 1, "GND"}}, 1, 1),
        makeEntry(ComponentType::TerminalBlock, ComponentCategory::Connector,
                  SpiceFamily::Port, "TERM", "2 脚端子",
                  "接线端子 / 板级接口", "J",
                  {{0, -1, "1"}, {0, 1, "2"}}, 1, 1),
        // Peltier coolers: DC loads whose cold face cools down over time.
        makeEntry(ComponentType::Tec12706, ComponentCategory::SwitchLoad,
                  SpiceFamily::Thermoelectric, "TEC12706", "制冷片 12706",
                  "TEC1-12706 · 12 V 6 A · 57 W", "TEC",
                  {{0, -1, "A"}, {0, 1, "B"}}, 1, 1, true, true),
        makeEntry(ComponentType::Tec12710, ComponentCategory::SwitchLoad,
                  SpiceFamily::Thermoelectric, "TEC12710", "制冷片 12710",
                  "TEC1-12710 · 12 V 10.5 A · 92 W", "TEC",
                  {{0, -1, "A"}, {0, 1, "B"}}, 1, 1, true, true),
        // Real board parts: a known logic-level MOSFET, an MLCC and a power
        // resistor.
        makeEntry(ComponentType::Irl540N, ComponentCategory::Transistor,
                  SpiceFamily::Mos, "NMOSIRL540", "IRL540N",
                  "100 V · 28 A · 44 mΩ 逻辑电平", "M",
                  {{1, -1, "D"}, {-1, 0, "G"}, {1, 1, "S"}}, 1, 1, false, true),
        makeEntry(ComponentType::CeramicCap, ComponentCategory::PowerFilter,
                  SpiceFamily::Capacitor, "CMLCC", "陶瓷电容",
                  "100 nF · ESR 10 mΩ · ESL 2 nH", "C",
                  {{0, -1, "A"}, {0, 1, "B"}}, 1, 1),
        makeEntry(ComponentType::PowerResistor, ComponentCategory::Basic,
                  SpiceFamily::Resistor, "RPWR", "功率电阻",
                  "1 Ω · 2 W · 绕线 1 µH", "R",
                  {{0, -1, "A"}, {0, 1, "B"}}, 1, 1),
    }};
    return entries;
}

}  // namespace

const ComponentInfo& componentInfo(ComponentType type)
{
    const size_t index = static_cast<size_t>(type);
    if (index >= catalog().size()) {
        return catalog()[0].info;
    }
    return catalog()[index].info;
}

const PinLayout& pinLayout(ComponentType type)
{
    const size_t index = static_cast<size_t>(type);
    if (index >= catalog().size()) {
        return catalog()[0].layout;
    }
    return catalog()[index].layout;
}

const char* componentPinName(ComponentType type, int pin)
{
    const PinLayout& layout = pinLayout(type);
    if (pin < 0 || pin >= layout.count) {
        return "";
    }
    return layout.pins[static_cast<size_t>(pin)].name;
}

const std::vector<ComponentCategory>& componentCategories()
{
    static const std::vector<ComponentCategory> categories = {
        ComponentCategory::Basic,       ComponentCategory::PowerSignal,
        ComponentCategory::Semiconductor, ComponentCategory::Transistor,
        ComponentCategory::AnalogIC,    ComponentCategory::Digital,
        ComponentCategory::SwitchLoad,  ComponentCategory::PowerFilter,
        ComponentCategory::Instrument,  ComponentCategory::Connector,
    };
    return categories;
}

const char* categoryLabel(ComponentCategory category)
{
    switch (category) {
    case ComponentCategory::Basic:
        return "基础";
    case ComponentCategory::PowerSignal:
        return "电源与信号";
    case ComponentCategory::Semiconductor:
        return "半导体";
    case ComponentCategory::Transistor:
        return "晶体管";
    case ComponentCategory::AnalogIC:
        return "模拟芯片";
    case ComponentCategory::Digital:
        return "数字逻辑";
    case ComponentCategory::SwitchLoad:
        return "开关与负载";
    case ComponentCategory::PowerFilter:
        return "功率与滤波";
    case ComponentCategory::Instrument:
        return "仪器";
    case ComponentCategory::Connector:
        return "接口";
    }
    return "";
}

std::vector<ComponentType> componentsIn(ComponentCategory category)
{
    std::vector<ComponentType> result;
    for (const CatalogEntry& entry : catalog()) {
        if (entry.info.category == category && entry.info.available) {
            result.push_back(entry.info.type);
        }
    }
    return result;
}

const std::vector<ComponentType>& allComponentTypes()
{
    static const std::vector<ComponentType> types = [] {
        std::vector<ComponentType> result;
        result.reserve(catalog().size());
        for (const CatalogEntry& entry : catalog()) {
            result.push_back(entry.info.type);
        }
        return result;
    }();
    return types;
}

bool needsPinSensing(ComponentType type)
{
    const SpiceFamily family = componentInfo(type).family;
    return family == SpiceFamily::Bjt || family == SpiceFamily::Mos ||
           family == SpiceFamily::Subcircuit ||
           family == SpiceFamily::Thermoelectric;
}

bool forcesTransient(ComponentType type)
{
    return componentInfo(type).forcesTransient;
}

bool parseComponentTypeKey(const std::string& key, ComponentType* out)
{
    for (const CatalogEntry& entry : catalog()) {
        if (key == entry.info.typeKey) {
            if (out) {
                *out = entry.info.type;
            }
            return true;
        }
    }
    return false;
}

}  // namespace circuitlab
