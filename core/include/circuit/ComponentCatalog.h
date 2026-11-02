#pragma once

#include "circuit/Circuit.h"

#include <array>
#include <string>
#include <vector>

namespace circuitlab {

enum class ComponentCategory {
    Basic,          // 基础
    PowerSignal,    // 电源信号
    Semiconductor,  // 半导体
    Transistor,     // 晶体管
    AnalogIC,       // 模拟 IC
    Digital,        // 数字
    SwitchLoad,     // 开关负载
    PowerFilter,    // 功率与滤波
    Instrument,     // 仪器
    Connector,      // 接口
};

// How a component is represented in the SPICE netlist and how its branch
// currents are probed. Subcircuits include two-pin parts such as the NOT gate.
enum class SpiceFamily {
    Resistor,
    Capacitor,
    Inductor,
    VoltageSource,
    Diode,
    Bjt,
    Mos,
    Subcircuit,
    Ground,
    // A mechanical part: the two screws of a terminal block are isolated from
    // each other, so it only names two nets and emits no SPICE card.
    Port,
};

struct PinDef {
    int x = 0;   // local grid coordinates, component center is (0, 0)
    int y = 0;
    const char* name = "";
};

struct PinLayout {
    int count = 0;
    std::array<PinDef, kMaxComponentPins> pins{};
    // Symbol body extents before rotation. Pins extend past the body.
    int bodyHalfWidth = 1;
    int bodyHalfHeight = 2;
};

struct ComponentInfo {
    ComponentType type = ComponentType::Resistor;
    ComponentCategory category = ComponentCategory::Basic;
    SpiceFamily family = SpiceFamily::Resistor;
    const char* typeKey = "R";      // stable key stored in project files
    const char* label = "";         // Chinese display name
    const char* description = "";   // default value / short hint for cards
    const char* prefix = "R";       // SPICE reference prefix
    bool forcesTransient = false;   // internally oscillating parts (555)
    bool available = true;          // included in the component picker
};

const ComponentInfo& componentInfo(ComponentType type);
const PinLayout& pinLayout(ComponentType type);
const char* componentPinName(ComponentType type, int pin);

const std::vector<ComponentCategory>& componentCategories();
const char* categoryLabel(ComponentCategory category);
std::vector<ComponentType> componentsIn(ComponentCategory category);
const std::vector<ComponentType>& allComponentTypes();

bool needsPinSensing(ComponentType type);
bool forcesTransient(ComponentType type);
bool parseComponentTypeKey(const std::string& key, ComponentType* out);

}  // namespace circuitlab
