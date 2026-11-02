#include "circuit/Simulation.h"

#include "circuit/ComponentCatalog.h"
#include "circuit/Engineering.h"
#include "circuit/Netlist.h"

#include "ngspice/sharedspice.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace circuitlab {

struct EngineImpl {
    std::mutex mutex;
    std::atomic<bool> busy{false};
    bool initialized = false;
    std::string log;
};

namespace {

void appendLog(EngineImpl* impl, const char* text)
{
    if (!impl || !text) {
        return;
    }
    impl->log.append(text);
    if (impl->log.size() > 256 * 1024) {
        impl->log.erase(0, impl->log.size() - 256 * 1024);
    }
}

int onSendChar(char* text, int /*id*/, void* user)
{
    appendLog(static_cast<EngineImpl*>(user), text);
    return 0;
}

int onSendStat(char* /*text*/, int /*id*/, void* /*user*/)
{
    return 0;
}

int onControlledExit(int /*status*/, NG_BOOL /*immediate*/, NG_BOOL /*quit*/,
                     int /*id*/, void* /*user*/)
{
    // Never let ngspice call exit() on the Android app process.
    return 0;
}

int onSendData(pvecvaluesall /*data*/, int /*count*/, int /*id*/, void* /*user*/)
{
    return 0;
}

int onSendInitData(pvecinfoall /*data*/, int /*id*/, void* /*user*/)
{
    return 0;
}

int onBgThreadRunning(NG_BOOL /*running*/, int /*id*/, void* /*user*/)
{
    return 0;
}

void runCommand(const char* command)
{
    if (!command) {
        return;
    }
    std::string mutableCommand(command);
    ngSpice_Command(mutableCommand.data());
}

int plotCount()
{
    char** plots = ngSpice_AllPlots();
    int count = 0;
    if (plots) {
        while (plots[count]) {
            ++count;
        }
    }
    return count;
}

std::string trim(const std::string& text)
{
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string lower(std::string text)
{
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

// ngspice 47 reports numeric nodes as "V(1)" (case may vary), branch currents
// as "@r1[i]" (or "i(v1)"), and the transient scale as "time". The returned
// key is "time", "node:<n>" or "dev:<lowercase device name>".
std::string normalizeVectorKey(const char* raw)
{
    if (!raw) {
        return {};
    }
    const std::string name = lower(trim(raw));
    if (name == "time") {
        return "time";
    }
    if (name.size() > 4 && name[0] == '@' && name.back() == ']') {
        const size_t bracket = name.find('[');
        if (bracket > 1) {
            return "dev:" + name.substr(1, bracket - 1);
        }
    }
    if (name.size() > 4 && name[0] == 'i' && name[1] == '(' && name.back() == ')') {
        return "dev:" + name.substr(2, name.size() - 3);
    }
    // Voltage sources (including the per-pin sense sources) expose their
    // current as "vq1p0#branch" in the plot vector list.
    static const std::string branchSuffix = "#branch";
    if (name.size() > branchSuffix.size() &&
        name.compare(name.size() - branchSuffix.size(), branchSuffix.size(),
                     branchSuffix) == 0) {
        return "dev:" + name.substr(0, name.size() - branchSuffix.size());
    }
    if (name.size() > 3 && name[0] == 'v' && name[1] == '(' && name.back() == ')') {
        const std::string inner = name.substr(2, name.size() - 3);
        if (!inner.empty() && std::all_of(inner.begin(), inner.end(),
                                          [](unsigned char c) { return std::isdigit(c) != 0; })) {
            return "node:" + inner;
        }
    }
    return {};
}

bool parseNodeNumber(const std::string& text, int* node)
{
    if (text.empty()) {
        return false;
    }
    for (char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    try {
        *node = std::stoi(text);
    } catch (...) {
        return false;
    }
    return *node >= 0;
}

std::string errorTail(const std::string& log)
{
    std::istringstream input(log);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(input, line)) {
        line = trim(line);
        if (!line.empty()) {
            lines.push_back(line);
            if (lines.size() > 8) {
                lines.erase(lines.begin());
            }
        }
    }
    std::string result;
    for (const auto& entry : lines) {
        if (!result.empty()) {
            result += '\n';
        }
        result += entry;
    }
    if (result.size() > 1200) {
        result = result.substr(result.size() - 1200);
    }
    return result;
}

// Turns the most common ngspice failures into an actionable Chinese hint, so a
// bare "仿真失败" never comes without a next step.
std::string failureHint(const std::string& log)
{
    const std::string text = lower(log);
    if (text.find("shorted vsrc") != std::string::npos) {
        return "\n提示：有电压源被短路（两端接在同一个节点）。请检查它的两端是否落在了同一条导线上。";
    }
    if (text.find("singular matrix") != std::string::npos ||
        text.find("no dc path") != std::string::npos ||
        text.find("no path to ground") != std::string::npos) {
        return "\n提示：电路里有节点无法确定电压（浮空，或被电容完全隔断）。"
               "请给这个节点一条到地的直流通路（电阻、电感或电源）。";
    }
    if (text.find("timestep too small") != std::string::npos) {
        return "\n提示：瞬态推进不动了，通常是步长太小或波形边沿太陡。"
               "请把“设置 → 步长”调大一级再试。";
    }
    return {};
}

std::vector<std::string> splitLines(const std::string& netlist)
{
    std::vector<std::string> lines;
    std::istringstream input(netlist);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    while (!lines.empty() && trim(lines.back()).empty()) {
        lines.pop_back();
    }
    return lines;
}

std::future<SimulationResult> readyError(SimulationRequest request, const std::string& error)
{
    std::promise<SimulationResult> promise;
    SimulationResult result;
    result.ok = false;
    result.kind = request.kind;
    result.revision = request.revision;
    result.error = error;
    result.log = error;
    promise.set_value(std::move(result));
    return promise.get_future();
}

}  // namespace

NgspiceEngine::NgspiceEngine() : impl_(std::make_unique<EngineImpl>()) {}

NgspiceEngine::~NgspiceEngine()
{
    // The UI waits for its future before destroying the engine. As a safety
    // net, give a running worker a short grace period.
    for (int i = 0; i < 500 && impl_ && impl_->busy.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool NgspiceEngine::initialize(std::string& error)
{
    if (!impl_) {
        error = "ngspice 引擎未创建。";
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->initialized) {
        error.clear();
        return true;
    }

    impl_->log.clear();
    const int rc = ngSpice_Init(onSendChar, onSendStat, onControlledExit, onSendData,
                                onSendInitData, onBgThreadRunning, impl_.get());
    if (rc != 0) {
        error = "ngSpice_Init 失败（返回 " + std::to_string(rc) + "）。";
        if (!impl_->log.empty()) {
            error += "\n" + errorTail(impl_->log);
        }
        return false;
    }

    runCommand("set no_spinit");
    runCommand("set no_spiceinit");
    impl_->initialized = true;
    error.clear();
    return true;
}

bool NgspiceEngine::busy() const
{
    return impl_ && impl_->busy.load();
}

std::future<SimulationResult> NgspiceEngine::runAsync(SimulationRequest request)
{
    if (!impl_) {
        return readyError(std::move(request), "ngspice 引擎未创建。");
    }
    bool expected = false;
    if (!impl_->busy.compare_exchange_strong(expected, true)) {
        return readyError(std::move(request), "上一次仿真仍在运行。");
    }

    try {
        return std::async(std::launch::async, [this, request = std::move(request)]() mutable {
            SimulationResult result;
            try {
                result = run(request);
            } catch (const std::exception& exception) {
                result.ok = false;
                result.kind = request.kind;
                result.revision = request.revision;
                result.error = std::string("仿真异常：") + exception.what();
                result.log = result.error;
            } catch (...) {
                result.ok = false;
                result.kind = request.kind;
                result.revision = request.revision;
                result.error = "仿真发生未知异常。";
                result.log = result.error;
            }
            impl_->busy.store(false);
            return result;
        });
    } catch (...) {
        impl_->busy.store(false);
        return readyError(std::move(request), "无法启动仿真线程。");
    }
}

SimulationResult NgspiceEngine::run(const SimulationRequest& request)
{
    SimulationResult result;
    result.kind = request.kind;
    result.revision = request.revision;

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized) {
        // initialize() takes the same mutex, so inline the initialization here.
        impl_->log.clear();
        const int initRc = ngSpice_Init(onSendChar, onSendStat, onControlledExit, onSendData,
                                        onSendInitData, onBgThreadRunning, impl_.get());
        if (initRc != 0) {
            result.error = "ngSpice_Init 失败（返回 " + std::to_string(initRc) + "）。";
            result.log = result.error + "\n" + errorTail(impl_->log);
            return result;
        }
        runCommand("set no_spinit");
        runCommand("set no_spiceinit");
        impl_->initialized = true;
    }

    if (request.kind == AnalysisKind::Transient) {
        if (!(request.transient.step > 0.0) || !(request.transient.stop > 0.0)) {
            result.error = "瞬态步长和停止时间必须为正数。";
            result.log = result.error;
            return result;
        }
        const double points = request.transient.stop / request.transient.step;
        // Switching converters need a long window (startup + settling) at a
        // small step to resolve the ripple, so the cap is well above the old
        // 10 k limit.
        if (!(points >= 2.0) || points > 200000.0) {
            result.error = "瞬态点数必须介于 2 和 200000 之间。";
            result.log = result.error;
            return result;
        }
    }

    const CompiledCircuit compiled = compileCircuit(request.circuit);
    if (!compiled.ok) {
        result.error = compiled.error;
        result.log = compiled.error;
        return result;
    }

    impl_->log.clear();
    runCommand("destroy all");

    std::vector<std::string> lineStorage = splitLines(compiled.netlist);
    if (lineStorage.empty()) {
        result.error = "网表为空。";
        result.log = result.error;
        return result;
    }
    std::vector<char*> lines;
    lines.reserve(lineStorage.size() + 1);
    for (auto& line : lineStorage) {
        lines.push_back(line.data());
    }
    lines.push_back(nullptr);

    const int loadRc = ngSpice_Circ(lines.data());
    if (loadRc != 0) {
        result.error = "ngspice 无法加载电路。" + failureHint(impl_->log);
        result.log = errorTail(impl_->log);
        if (result.log.empty()) {
            result.log = result.error;
        }
        runCommand("destroy all");
        return result;
    }

    // Save every device branch current alongside the node voltages so the UI
    // can display current on the schematic. Two-pin devices use @name[i];
    // multi-pin devices use the 0 V sense source on every pin.
    std::string saveCommand = "save all";
    for (const auto& component : request.circuit.components) {
        if (component.type == ComponentType::Ground ||
            componentInfo(component.type).family == SpiceFamily::Port) {
            continue;
        }
        if (!needsPinSensing(component.type)) {
            saveCommand += ' ';
            saveCommand += pinCurrentSaveExpression(component, 0);
            continue;
        }
        const int pins = pinCount(component.type);
        for (int pin = 0; pin < pins; ++pin) {
            saveCommand += ' ';
            saveCommand += pinCurrentSaveExpression(component, pin);
        }
    }
    runCommand(saveCommand.c_str());

    const int before = plotCount();
    std::string command = "op";
    if (request.kind == AnalysisKind::Transient) {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "tran %.17g %.17g",
                      request.transient.step, request.transient.stop);
        command = buffer;
    }
    runCommand(command.c_str());
    if (plotCount() <= before) {
        result.error = "ngspice 仿真失败。" + failureHint(impl_->log);
        result.log = errorTail(impl_->log);
        if (result.log.empty()) {
            result.log = result.error;
        }
        runCommand("destroy all");
        return result;
    }

    char* plot = ngSpice_CurPlot();
    result.plotName = plot ? plot : "";

    ngSpice_LockRealloc();
    char** names = plot ? ngSpice_AllVecs(plot) : nullptr;
    if (names) {
        for (int i = 0; names[i]; ++i) {
            const std::string key = normalizeVectorKey(names[i]);
            if (key.empty()) {
                continue;
            }
            pvector_info vector = ngGet_Vec_Info(names[i]);
            if (!vector || !vector->v_realdata || vector->v_length <= 0) {
                continue;
            }
            std::vector<double> values(static_cast<size_t>(vector->v_length));
            for (int k = 0; k < vector->v_length; ++k) {
                values[static_cast<size_t>(k)] = vector->v_realdata[k];
            }
            if (key == "time") {
                result.time = std::move(values);
                continue;
            }
            if (key.compare(0, 5, "node:") == 0) {
                int node = 0;
                if (parseNodeNumber(key.substr(5), &node)) {
                    if (result.nodeVoltage.find(node) == result.nodeVoltage.end()) {
                        result.nodeVoltage.emplace(node, std::move(values));
                    }
                }
                continue;
            }
            if (key.compare(0, 4, "dev:") == 0) {
                const std::string device = key.substr(4);
                if (!device.empty() && result.deviceCurrent.find(device) ==
                                            result.deviceCurrent.end()) {
                    result.deviceCurrent.emplace(device, std::move(values));
                }
            }
        }
    }
    ngSpice_UnlockRealloc();

    // Reorganize branch currents per component pin. Two-pin devices use
    // pin A -> pin B; multi-pin devices read their per-pin sense sources.
    for (const auto& component : request.circuit.components) {
        if (component.type == ComponentType::Ground ||
            componentInfo(component.type).family == SpiceFamily::Port) {
            continue;
        }
        const int pins = pinCount(component.type);
        std::vector<std::vector<double>> values(static_cast<size_t>(pins));
        if (needsPinSensing(component.type)) {
            for (int pin = 0; pin < pins; ++pin) {
                const auto found = result.deviceCurrent.find(pinCurrentKey(component, pin));
                if (found != result.deviceCurrent.end()) {
                    values[static_cast<size_t>(pin)] = found->second;
                }
            }
        } else {
            const auto found = result.deviceCurrent.find(lower(component.name));
            if (found != result.deviceCurrent.end() && pins >= 2) {
                values[0] = found->second;
                std::vector<double> negated = found->second;
                for (double& value : negated) {
                    value = -value;
                }
                values[1] = std::move(negated);
            }
        }
        result.pinCurrent[component.id] = std::move(values);
    }

    result.nodeCount = static_cast<int>(result.nodeVoltage.size());
    result.log = errorTail(impl_->log);
    if (result.log.empty()) {
        result.log = "仿真完成。";
    }
    runCommand("destroy all");

    if (result.nodeVoltage.empty()) {
        result.ok = false;
        result.error = "仿真没有产生节点电压。" + failureHint(impl_->log);
        if (result.log.empty() || result.log == "仿真完成。") {
            result.log = result.error;
        }
        return result;
    }
    if (request.kind == AnalysisKind::Transient && result.time.empty()) {
        result.ok = false;
        result.error = "瞬态仿真没有产生时间轴。";
        return result;
    }

    result.ok = true;
    result.error.clear();
    return result;
}

}  // namespace circuitlab
