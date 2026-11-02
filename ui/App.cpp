#include "App.h"

#include "Theme.h"

#include "circuit/Engineering.h"
#include "circuit/Examples.h"
#include "circuit/ProjectIO.h"

#include "imgui_internal.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <set>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace circuitlab {
namespace {

const ImU32 kAccentHovered = IM_COL32(80, 155, 240, 255);
const ImU32 kAccentActive = IM_COL32(102, 172, 250, 255);
const ImU32 kDangerHovered = IM_COL32(230, 82, 82, 255);
const ImU32 kDangerActive = IM_COL32(244, 108, 108, 255);

std::string truncateUtf8(const std::string& text, size_t maxCharacters)
{
    size_t characters = 0;
    size_t index = 0;
    while (index < text.size() && characters < maxCharacters) {
        const unsigned char c = static_cast<unsigned char>(text[index]);
        size_t length = 1;
        if ((c & 0xFC) == 0xF8) {
            length = 6;
        } else if ((c & 0xF8) == 0xF0) {
            length = 4;
        } else if ((c & 0xF0) == 0xE0) {
            length = 3;
        } else if ((c & 0xE0) == 0xC0) {
            length = 2;
        }
        index = std::min(text.size(), index + length);
        ++characters;
    }
    if (index >= text.size()) {
        return text;
    }
    return text.substr(0, index) + "…";
}

bool coloredButton(const char* label, const ImVec2& size, ImU32 base, ImU32 hovered, ImU32 active)
{
    ImGui::PushStyleColor(ImGuiCol_Button, Theme::toVec4(base));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::toVec4(hovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::toVec4(active));
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return clicked;
}

bool accentButton(const char* label, const ImVec2& size)
{
    return coloredButton(label, size, Theme::accent(), kAccentHovered, kAccentActive);
}

bool dangerButton(const char* label, const ImVec2& size)
{
    return coloredButton(label, size, Theme::danger(), kDangerHovered, kDangerActive);
}

std::string projectTimestamp(long long seconds)
{
    if (seconds <= 0) {
        return "刚刚";
    }
    const std::time_t value = static_cast<std::time_t>(seconds);
    std::tm local{};
    localtime_r(&value, &local);
    char buffer[64];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local);
    return buffer;
}

// Name of the example project the app ships with, and the marker that keeps it
// from being re-created after the user deletes it.
const char* kMosExampleName = "MOS 开关板示例";
const char* kTecExampleName = "制冷片示例 TEC1-12706";
const char* kExampleHint =
    "示例工程：按左下角启动仿真，再看 结果 → 示波 读平均值和纹波";

bool isExampleProjectName(const std::string& name)
{
    return name.find("示例") != std::string::npos;
}

// "MOS 开关板示例" -> "MOS 开关板", so a forked copy no longer looks like the
// bundled example (and is not auto-run / forked again).
std::string withoutExampleWord(const std::string& name)
{
    std::string result;
    const std::string word = "示例";
    size_t position = 0;
    while (position < name.size()) {
        const size_t found = name.find(word, position);
        if (found == std::string::npos) {
            result.append(name, position, std::string::npos);
            break;
        }
        result.append(name, position, found - position);
        position = found + word.size();
    }
    // Trim spaces left behind by the removal.
    const size_t begin = result.find_first_not_of(' ');
    if (begin == std::string::npos) {
        return {};
    }
    const size_t end = result.find_last_not_of(' ');
    return result.substr(begin, end - begin + 1);
}

std::string makeProjectPath(const std::string& directory)
{
    const long long stamp = static_cast<long long>(std::time(nullptr));
    for (int index = 0; index < 1000; ++index) {
        const std::string path = directory + "/p" + std::to_string(stamp) + "_" +
                                 std::to_string(index) + ".ckt";
        if (access(path.c_str(), F_OK) != 0) {
            return path;
        }
    }
    return directory + "/p" + std::to_string(stamp) + ".ckt";
}

std::string nextProjectName(const std::vector<ProjectInfo>& projects)
{
    for (int index = 1; index < 1000; ++index) {
        const std::string name = "工程 " + std::to_string(index);
        bool used = false;
        for (const auto& project : projects) {
            if (project.name == name) {
                used = true;
                break;
            }
        }
        if (!used) {
            return name;
        }
    }
    return "新工程";
}

}  // namespace

App::App() = default;

App::~App()
{
    if (future_.valid()) {
        future_.wait();
    }
    if (dirty_ && !currentProjectPath_.empty()) {
        if (currentProjectIsExample_) {
            forkExampleProject();
        } else {
            std::string error;
            saveProject(circuit_, currentProjectPath_, currentProjectName_, &error);
        }
    }
}

void App::init(const std::string& baseDir, float uiScale)
{
    uiScale_ = std::max(1.0f, uiScale);
    baseDir_ = baseDir;
    if (!baseDir_.empty() && baseDir_.back() != '/') {
        baseDir_ += '/';
    }
    projectsDir_ = baseDir_ + "projects";

    engine_ = std::make_unique<NgspiceEngine>();
    std::string error;
    if (!engine_->initialize(error)) {
        engineError_ = error.empty() ? "ngspice 初始化失败。" : error;
        status_ = "引擎不可用";
    } else {
        status_ = "就绪";
    }

    if (!ensureProjectDirectory(projectsDir_, &error)) {
        toast_ = error;
        toastAt_ = SDL_GetTicks();
    }

    refreshProjects();
    if (projects_.empty()) {
        loadLegacyAutosave();
        refreshProjects();
    }
    seedExampleProjects();
    refreshProjects();
    screen_ = Screen::Home;
}

void App::seedExampleProjects()
{
    // Ship a couple of ready-to-run examples so a fresh install has something
    // to open and simulate immediately. A marker file makes this a one-time
    // action; the version suffix lets a new example reach existing installs.
    const std::string marker = baseDir_ + "examples-v2.marker";
    if (access(marker.c_str(), F_OK) == 0) {
        return;
    }
    struct SeededExample {
        const char* name;
        Circuit (*build)();
    };
    const SeededExample examples[] = {
        {kMosExampleName, &makeMosSwitchBoardDemo},
        {kTecExampleName, &makeThermoelectricDemo},
    };
    for (const SeededExample& example : examples) {
        bool alreadyThere = false;
        for (const auto& project : projects_) {
            if (project.name == example.name) {
                alreadyThere = true;
                break;
            }
        }
        if (alreadyThere) {
            continue;
        }
        std::string error;
        const std::string path = makeProjectPath(projectsDir_);
        if (saveProject(example.build(), path, example.name, &error)) {
            toast_ = std::string("已加入示例工程：") + example.name;
            toastAt_ = SDL_GetTicks();
        }
    }
    if (FILE* file = std::fopen(marker.c_str(), "w")) {
        std::fclose(file);
    }
}

void App::setSafeInsets(float left, float top, float right, float bottom)
{
    rawInsetLeft_ = std::max(0.0f, left);
    rawInsetTop_ = std::max(0.0f, top);
    rawInsetRight_ = std::max(0.0f, right);
    rawInsetBottom_ = std::max(0.0f, bottom);
}

void App::onAppBackground()
{
    // Writing an unchanged project would bump its mtime and reorder the home
    // list, so only flush actual edits.
    if (dirty_ && !currentProjectPath_.empty()) {
        saveCurrentProject();
    }
}

void App::loadLegacyAutosave()
{
    const std::string legacy = baseDir_ + "autosave.ckt";
    if (access(legacy.c_str(), F_OK) != 0) {
        return;
    }
    Circuit circuit;
    std::string error;
    if (!loadCircuit(legacy, &circuit, &error) || circuit.components.empty()) {
        return;
    }
    const std::string path = makeProjectPath(projectsDir_);
    if (saveProject(circuit, path, "之前的电路", &error)) {
        std::remove(legacy.c_str());
    }
}

std::string App::uniqueProjectName(const std::string& base) const
{
    const auto taken = [this](const std::string& candidate) {
        for (const auto& project : projects_) {
            if (project.name == candidate) {
                return true;
            }
        }
        return false;
    };
    if (!taken(base)) {
        return base;
    }
    for (int index = 2; index < 1000; ++index) {
        const std::string candidate = base + " " + std::to_string(index);
        if (!taken(candidate)) {
            return candidate;
        }
    }
    return base;
}

bool App::forkExampleProject()
{
    if (!currentProjectIsExample_ || circuit_.empty()) {
        return false;
    }
    refreshProjects();
    std::string base = withoutExampleWord(currentProjectName_);
    if (base.empty()) {
        base = "我的电路";
    }
    const std::string name = uniqueProjectName(base);
    const std::string path = makeProjectPath(projectsDir_);
    std::string error;
    if (!saveProject(circuit_, path, name, &error)) {
        toast_ = error;
        toastAt_ = SDL_GetTicks();
        return false;
    }
    // The example file itself is never touched: the edit lives in a new copy.
    currentProjectPath_ = path;
    currentProjectName_ = name;
    currentProjectIsExample_ = false;
    dirty_ = false;
    refreshProjects();
    toast_ = "已另存为你的工程「" + name + "」，示例工程保持原样";
    toastAt_ = SDL_GetTicks();
    status_ = "另存为：" + name;
    return true;
}

void App::refreshProjects()
{
    projects_ = listProjects(projectsDir_);
}

bool App::createProject(const std::string& rawName, int example)
{
    std::string name = rawName;
    if (name.empty()) {
        name = "未命名工程";
    }
    Circuit circuit;
    if (example == 0) {
        circuit = makeDividerDemo();
    } else if (example == 1) {
        circuit = makeRcDemo();
    } else if (example == 2) {
        circuit = makeTimer555Demo();
    } else if (example == 3) {
        circuit = makeMosSwitchBoardDemo();
    }
    const std::string path = makeProjectPath(projectsDir_);
    std::string error;
    if (!saveProject(circuit, path, name, &error)) {
        toast_ = error;
        toastAt_ = SDL_GetTicks();
        return false;
    }

    circuit_ = std::move(circuit);
    currentProjectPath_ = path;
    currentProjectName_ = name;
    currentProjectIsExample_ = example >= 0 || isExampleProjectName(name);
    undoStack_.clear();
    selectedComponent_ = -1;
    selectedWire_ = -1;
    wireStarted_ = false;
    live_ = false;
    ++simulationGeneration_;
    liveRun_ = false;
    hasResult_ = false;
    resultRevision_ = -1;
    visibleNodes_.clear();
    visibleProbes_.clear();
    probeStats_.clear();
    probeSeriesCache_.clear();
    resultsOpen_ = false;
    showSettings_ = false;
    dirty_ = false;
    updateCompiled();
    fitPending_ = true;
    setTool(Tool::Select);
    placement_ = Placement{};
    screen_ = Screen::Editor;
    refreshProjects();
    status_ = example >= 0 ? "示例工程已创建" : "新工程：放置元件开始绘制";
    if (example >= 0 && engineError_.empty()) {
        // Examples demonstrate themselves: a created example starts simulating
        // right away and the result panel opens on the most useful tab, so the
        // oscilloscope numbers show up without extra taps.
        toast_ = kExampleHint;
        toastAt_ = SDL_GetTicks();
        activeTab_ = bestResultTab();
        resultsOpen_ = true;
        startLive();
    }
    return true;
}

bool App::openProject(const std::string& path)
{
    Circuit loaded;
    std::string name;
    std::string error;
    if (!loadProject(path, &loaded, &name, &error)) {
        toast_ = error;
        toastAt_ = SDL_GetTicks();
        return false;
    }
    if (name.empty()) {
        const size_t slash = path.find_last_of('/');
        name = slash == std::string::npos ? path : path.substr(slash + 1);
        const size_t dot = name.find_last_of('.');
        if (dot != std::string::npos) {
            name.erase(dot);
        }
    }

    circuit_ = std::move(loaded);
    // Old projects are shrunk on load; when that would change the wiring the
    // project keeps its original (large) symbols, so say so instead of silently
    // looking unchanged.
    bool keptLarge = false;
    for (const auto& component : circuit_.components) {
        if (component.legacySymbol) {
            keptLarge = true;
            break;
        }
    }
    if (keptLarge) {
        toast_ = "旧工程保持原尺寸（自动缩小会改变接线），新建工程即可用缩小版元件";
        toastAt_ = SDL_GetTicks();
    }
    currentProjectPath_ = path;
    currentProjectName_ = name;
    currentProjectIsExample_ = isExampleProjectName(name);
    undoStack_.clear();
    selectedComponent_ = -1;
    selectedWire_ = -1;
    wireStarted_ = false;
    live_ = false;
    ++simulationGeneration_;
    liveRun_ = false;
    hasResult_ = false;
    resultRevision_ = -1;
    visibleNodes_.clear();
    visibleProbes_.clear();
    probeStats_.clear();
    probeSeriesCache_.clear();
    resultsOpen_ = false;
    showSettings_ = false;
    dirty_ = false;
    updateCompiled();
    fitPending_ = true;
    setTool(Tool::Select);
    placement_ = Placement{};
    screen_ = Screen::Editor;
    status_ = "已打开工程";
    if (isExampleProjectName(currentProjectName_) && engineError_.empty()) {
        toast_ = kExampleHint;
        toastAt_ = SDL_GetTicks();
        activeTab_ = bestResultTab();
        resultsOpen_ = true;
        startLive();
    }
    return true;
}

bool App::saveCurrentProject(std::string* errorOut)
{
    std::string error;
    if (currentProjectPath_.empty()) {
        if (errorOut) {
            *errorOut = "当前没有工程。";
        }
        return false;
    }
    if (currentProjectIsExample_ && !forkExampleProject()) {
        // Saving a copy failed: keep the example untouched rather than
        // overwriting it with the user's edits.
        if (errorOut) {
            *errorOut = "示例工程不能被修改，另存为新工程失败。";
        }
        return false;
    }
    if (!saveProject(circuit_, currentProjectPath_, currentProjectName_, &error)) {
        toast_ = error;
        toastAt_ = SDL_GetTicks();
        if (errorOut) {
            *errorOut = error;
        }
        return false;
    }
    dirty_ = false;
    if (errorOut) {
        errorOut->clear();
    }
    return true;
}

void App::goHome()
{
    if (dirty_) {
        saveCurrentProject();
    }
    live_ = false;
    ++simulationGeneration_;
    liveRun_ = false;
    liveRerunPending_ = false;
    resultsOpen_ = false;
    showSettings_ = false;
    pinching_ = false;
    dragging_ = false;
    dragComponent_ = -1;
    wireStarted_ = false;
    placement_ = Placement{};
    setTool(Tool::Select);
    screen_ = Screen::Home;
    status_ = "就绪";
    refreshProjects();
}

void App::deleteProjectAt(const std::string& path)
{
    std::string error;
    if (!deleteProjectFile(path, &error)) {
        toast_ = error;
        toastAt_ = SDL_GetTicks();
        return;
    }
    if (path == currentProjectPath_) {
        currentProjectPath_.clear();
        currentProjectName_.clear();
        dirty_ = false;
    }
    refreshProjects();
    status_ = "工程已删除";
}

void App::frame()
{
    pollSimulation();
    checkLoadDamage();
    autosaveIfNeeded();

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 display = io.DisplaySize;
    if (display.x != lastDisplaySize_.x || display.y != lastDisplaySize_.y) {
        lastDisplaySize_ = display;
        fitPending_ = true;
    }

    if (live_ && !simRunning_ && liveRerunPending_ &&
        SDL_GetTicks() >= liveRerunAt_) {
        liveRerunPending_ = false;
        requestLiveRun();
    }
    if (live_ && hasResult_ && result_.ok && !simRunning_ &&
        resultRevision_ == circuit_.revision) {
        liveClock_ += static_cast<double>(io.DeltaTime);
    }

    if (selectedComponent_ >= 0 && !circuit_.findComponent(selectedComponent_)) {
        selectedComponent_ = -1;
    }
    if (selectedWire_ >= 0 && !circuit_.findWire(selectedWire_)) {
        selectedWire_ = -1;
    }

    portrait_ = display.y > display.x;
    if (portrait_) {
        safeTop_ = std::min(rawInsetTop_, dp(48.0f));
        safeLeft_ = 0.0f;
        safeRight_ = 0.0f;
        safeBottom_ = 0.0f;
    } else {
        // In landscape only the display cutout side gets an inset. Reclaim
        // the status bar / navigation strips so the canvas uses the screen.
        safeTop_ = 0.0f;
        safeBottom_ = 0.0f;
        const float notch = dp(40.0f);
        if (rawInsetLeft_ > rawInsetRight_ + dp(6.0f)) {
            safeLeft_ = std::min(rawInsetLeft_, notch);
            safeRight_ = 0.0f;
        } else if (rawInsetRight_ > rawInsetLeft_ + dp(6.0f)) {
            safeRight_ = std::min(rawInsetRight_, notch);
            safeLeft_ = 0.0f;
        } else {
            safeLeft_ = 0.0f;
            safeRight_ = 0.0f;
        }
    }

    const float width = std::max(320.0f, display.x - safeLeft_ - safeRight_);
    const float height = std::max(240.0f, display.y - safeTop_ - safeBottom_);
    rootMin_ = ImVec2(safeLeft_, safeTop_);
    rootSize_ = ImVec2(width, height);
    topBarHeight_ = dp(52.0f);
    bottomBarHeight_ = dp(68.0f);

    ImGui::SetNextWindowPos(rootMin_);
    ImGui::SetNextWindowSize(rootSize_);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::toVec4(Theme::background()));
    const ImGuiWindowFlags rootFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##root", nullptr, rootFlags);

    if (screen_ == Screen::Home) {
        drawHome();
        drawCreateDialog();
        drawDeleteDialog();
    } else {
        drawEditor();
        drawEditorMenuPopup();
        drawSettingsPopup();
        drawRenameDialog();
        drawNumberEditor();
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    if (screen_ == Screen::Editor) {
        drawEditorOverlays();
        drawSelectionCard();
        drawResultsOverlay();
        drawComponentSheet();
    }

    if (!toast_.empty() && SDL_GetTicks() - toastAt_ < 3000) {
        const ImVec2 textSize = ImGui::CalcTextSize(toast_.c_str());
        const ImVec2 pos((display.x - textSize.x) * 0.5f, safeTop_ + dp(14.0f));
        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        drawList->AddRectFilled(
            ImVec2(pos.x - dp(14.0f), pos.y - dp(8.0f)),
            ImVec2(pos.x + textSize.x + dp(14.0f), pos.y + textSize.y + dp(8.0f)),
            IM_COL32(38, 48, 60, 245), dp(10.0f));
        drawList->AddText(pos, Theme::text(), toast_.c_str());
    }
}

// ---------------------------------------------------------------------------
// Home screen
// ---------------------------------------------------------------------------

void App::drawHome()
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilledMultiColor(
        rootMin_, ImVec2(rootMin_.x + rootSize_.x, rootMin_.y + rootSize_.y),
        IM_COL32(11, 15, 20, 255), IM_COL32(20, 32, 50, 255),
        IM_COL32(11, 15, 20, 255), IM_COL32(11, 15, 20, 255));

    const float pad = portrait_ ? dp(14.0f) : dp(20.0f);
    const float buttonHeight = dp(40.0f);
    const float newWidth = portrait_ ? dp(108.0f) : dp(120.0f);
    const float exampleWidth = portrait_ ? dp(70.0f) : dp(80.0f);
    // Portrait stacks the examples under the "new project" button so five
    // buttons never overflow a narrow phone.
    const float headerY = portrait_ ? dp(68.0f) : dp(16.0f);
    const float exampleY = portrait_ ? dp(116.0f) : headerY;
    const float dividerY = portrait_ ? dp(164.0f) : dp(80.0f);
    const float listLabelY = portrait_ ? dp(176.0f) : dp(94.0f);
    const float listTop = portrait_ ? dp(202.0f) : dp(120.0f);

    ImGui::SetCursorPos(ImVec2(pad, dp(10.0f)));
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.65f);
    ImGui::TextUnformatted("电路实验室");
    ImGui::PopFont();

    ImGui::SetCursorPos(ImVec2(pad, dp(42.0f)));
    ImGui::TextDisabled("离线电路仿真 · 新建工程，或从示例开始");

    const float examplesWidth = exampleWidth * 4.0f + dp(18.0f);
    float buttonX = portrait_
                        ? pad
                        : rootSize_.x - pad - newWidth - examplesWidth - dp(8.0f);
    ImGui::SetCursorPos(ImVec2(buttonX, headerY));
    if (accentButton("+ 新建", ImVec2(newWidth, buttonHeight))) {
        showCreateDialog_ = true;
        const std::string suggested = nextProjectName(projects_);
        std::snprintf(newProjectName_, sizeof(newProjectName_), "%s", suggested.c_str());
    }
    struct ExampleButton {
        const char* label;
        const char* projectName;
        int example;
    };
    static const ExampleButton kExamples[] = {
        {"分压", "分压电路示例", 0},
        {"RC", "RC 充电示例", 1},
        {"555", "555 无稳态示例", 2},
        {"MOS 板", kMosExampleName, 3},
    };
    if (portrait_) {
        ImGui::SetCursorPos(ImVec2(pad, exampleY));
    } else {
        ImGui::SameLine(0.0f, dp(6.0f));
    }
    for (const ExampleButton& example : kExamples) {
        if (ImGui::Button(example.label, ImVec2(exampleWidth, buttonHeight))) {
            createProject(example.projectName, example.example);
        }
        ImGui::SameLine(0.0f, dp(6.0f));
    }

    drawList->AddLine(ImVec2(rootMin_.x + pad, rootMin_.y + dividerY),
                      ImVec2(rootMin_.x + rootSize_.x - pad, rootMin_.y + dividerY),
                      Theme::stroke(), 1.0f);

    ImGui::SetCursorPos(ImVec2(pad, listLabelY));
    ImGui::TextDisabled("我的工程");
    ImGui::SameLine();
    ImGui::TextDisabled("%d", static_cast<int>(projects_.size()));

    const float listHeight = std::max(dp(80.0f), rootSize_.y - listTop - dp(16.0f));
    ImGui::SetCursorPos(ImVec2(pad, listTop));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    ImGui::BeginChild("##project_list", ImVec2(rootSize_.x - pad * 2.0f, listHeight), 0, 0);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    updateListDragScroll();

    if (projects_.empty()) {
        ImGui::SetCursorPos(ImVec2(dp(8.0f), dp(28.0f)));
        ImGui::TextDisabled("还没有工程。点击“新建工程”开始。");
        ImGui::SetCursorPos(ImVec2(dp(8.0f), dp(70.0f)));
        if (accentButton("创建第一个工程", ImVec2(dp(180.0f), buttonHeight))) {
            showCreateDialog_ = true;
            const std::string suggested = nextProjectName(projects_);
            std::snprintf(newProjectName_, sizeof(newProjectName_), "%s", suggested.c_str());
        }
    }

    for (size_t index = 0; index < projects_.size(); ++index) {
        const ProjectInfo& project = projects_[index];
        ImGui::PushID(static_cast<int>(index));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::toVec4(Theme::panel()));
        ImGui::BeginChild("##card", ImVec2(0.0f, dp(64.0f)), 0,
                          ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleColor();

        const float right = ImGui::GetWindowWidth() - dp(10.0f);
        ImGui::SetCursorPos(ImVec2(dp(14.0f), dp(8.0f)));
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.02f);
        ImGui::TextUnformatted(truncateUtf8(project.name, portrait_ ? 10 : 24).c_str());
        ImGui::PopFont();
        ImGui::SetCursorPos(ImVec2(dp(14.0f), dp(32.0f)));
        ImGui::TextDisabled("修改于 %s", projectTimestamp(project.modified).c_str());

        const float smallH = dp(34.0f);
        const float smallW = dp(64.0f);
        ImGui::SetCursorPos(ImVec2(right - smallW * 2.0f - dp(6.0f),
                                   (dp(64.0f) - smallH) * 0.5f));
        if (accentButton("编辑", ImVec2(smallW, smallH))) {
            openProject(project.path);
        }
        ImGui::SameLine(0.0f, dp(6.0f));
        if (dangerButton("删除", ImVec2(smallW, smallH))) {
            deletePath_ = project.path;
            deleteName_ = project.name;
            showDeleteDialog_ = true;
        }

        ImGui::EndChild();
        ImGui::PopID();
    }

    finishListDragScroll();
    ImGui::EndChild();
}

void App::drawCreateDialog()
{
    if (showCreateDialog_) {
        showCreateDialog_ = false;
        ImGui::OpenPopup("新建工程");
    }
    ImGui::SetNextWindowSize(ImVec2(dp(380.0f), 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("新建工程", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        return;
    }

    ImGui::TextUnformatted("新建工程");
    ImGui::Separator();
    ImGui::TextDisabled("给工程起一个名字");
    ImGui::SetNextItemWidth(-1.0f);
    const bool appearing = ImGui::IsWindowAppearing();
    if (appearing) {
        ImGui::SetKeyboardFocusHere();
    }
    const bool entered = ImGui::InputTextWithHint("##project_name", "工程名称",
                                                  newProjectName_, sizeof(newProjectName_),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::Spacing();

    const ImVec2 size(dp(140.0f), dp(46.0f));
    if (accentButton("创建", size) || entered) {
        if (createProject(newProjectName_, -1)) {
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("取消", size)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::drawDeleteDialog()
{
    if (showDeleteDialog_) {
        showDeleteDialog_ = false;
        ImGui::OpenPopup("删除工程");
    }
    ImGui::SetNextWindowSize(ImVec2(dp(400.0f), 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("删除工程", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        return;
    }

    ImGui::TextUnformatted("删除工程");
    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::Text("确定删除“%s”吗？", truncateUtf8(deleteName_, 24).c_str());
    ImGui::TextDisabled("删除后无法恢复。");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    const ImVec2 size(dp(130.0f), dp(46.0f));
    if (dangerButton("删除", size)) {
        deleteProjectAt(deletePath_);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("取消", size)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::drawRenameDialog()
{
    if (showRenameDialog_) {
        showRenameDialog_ = false;
        ImGui::OpenPopup("重命名工程");
    }
    ImGui::SetNextWindowSize(ImVec2(dp(380.0f), 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("重命名工程", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        return;
    }

    ImGui::TextUnformatted("重命名工程");
    ImGui::Separator();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    const bool entered = ImGui::InputTextWithHint("##rename", "工程名称", renameBuffer_,
                                                  sizeof(renameBuffer_),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::Spacing();
    const ImVec2 size(dp(130.0f), dp(46.0f));
    if (accentButton("保存", size) || entered) {
        if (renameBuffer_[0] != '\0') {
            currentProjectName_ = renameBuffer_;
            dirty_ = true;
            saveCurrentProject();
            refreshProjects();
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("取消", size)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// Editor layout
// ---------------------------------------------------------------------------

void App::drawEditor()
{
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    drawTopBar(topBarHeight_);
    const float contentHeight = std::max(
        dp(160.0f), rootSize_.y - topBarHeight_ - bottomBarHeight_);
    drawCanvas(contentHeight);
    drawBottomBar(bottomBarHeight_);
    ImGui::PopStyleVar();
}

void App::drawTopBar(float height)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::toVec4(Theme::panel()));
    ImGui::BeginChild("##topbar", ImVec2(0.0f, height), 0, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();

    const float pad = dp(10.0f);
    const float buttonHeight = dp(38.0f);
    const float y = (height - buttonHeight) * 0.5f;

    ImGui::SetCursorPos(ImVec2(pad, y));
    if (ImGui::Button("←", ImVec2(dp(46.0f), buttonHeight))) {
        goHome();
    }

    ImGui::SameLine(0.0f, dp(10.0f));
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.06f);
    ImGui::TextUnformatted(truncateUtf8(projectTitle(), portrait_ ? 10 : 18).c_str());
    ImGui::PopFont();

    const float gap = dp(6.0f);
    const float small = dp(52.0f);
    const float menuWidth = dp(46.0f);
    const float rightX = ImGui::GetWindowWidth() - pad -
                         (small * 2.0f + menuWidth + gap * 2.0f);
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + dp(8.0f), rightX));
    ImGui::BeginDisabled(undoStack_.empty());
    if (ImGui::Button("撤销", ImVec2(small, buttonHeight))) {
        undo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, gap);
    ImGui::BeginDisabled(redoStack_.empty());
    if (ImGui::Button("重做", ImVec2(small, buttonHeight))) {
        redo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, gap);
    if (ImGui::Button("⋯", ImVec2(menuWidth, buttonHeight))) {
        showEditorMenu_ = true;
    }

    const ImVec2 windowPos = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(windowPos.x, windowPos.y + ImGui::GetWindowHeight() - 1.0f),
        ImVec2(windowPos.x + ImGui::GetWindowWidth(),
               windowPos.y + ImGui::GetWindowHeight() - 1.0f),
        Theme::stroke(), 1.0f);
    ImGui::EndChild();
}

void App::drawResultsOverlay()
{
    if (!resultsOpen_ || sheetOpen()) {
        return;
    }
    const float height =
        portrait_ ? std::clamp(rootSize_.y * 0.55f, dp(240.0f), dp(420.0f))
                  : std::clamp(rootSize_.y * 0.60f, dp(170.0f), dp(215.0f));
    const float width = rootSize_.x - dp(16.0f);
    const float y = safeTop_ + rootSize_.y - bottomBarHeight_ - height - dp(8.0f);
    ImGui::SetNextWindowPos(ImVec2(safeLeft_ + dp(8.0f), y));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::toVec4(Theme::panelAlt()));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(dp(10.0f), dp(7.0f)));
    if (!ImGui::Begin("##results", nullptr,
                      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoScrollbar)) {
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        return;
    }

    const char* labels[] = {"电流", "电压", "波形", "示波", "网表", "日志"};
    const float tabWidth = portrait_ ? dp(46.0f) : dp(50.0f);
    const float tabGap = dp(4.0f);
    const float tabHeight = dp(32.0f);
    for (int index = 0; index < 6; ++index) {
        const bool active = activeTab_ == index;
        if (active) {
            accentButton(labels[index], ImVec2(tabWidth, tabHeight));
        } else {
            ImGui::Button(labels[index], ImVec2(tabWidth, tabHeight));
        }
        if (ImGui::IsItemClicked()) {
            activeTab_ = index;
        }
        ImGui::SameLine(0.0f, tabGap);
    }

    const float closeWidth = dp(34.0f);
    const float closeX = ImGui::GetWindowWidth() - closeWidth - dp(4.0f);
    ImGui::SameLine(closeX);
    if (ImGui::Button("×", ImVec2(closeWidth, tabHeight))) {
        resultsOpen_ = false;
    }

    ImGui::Separator();
    ImGui::BeginChild("##result_content", ImVec2(0.0f, 0.0f), 0, 0);
    updateListDragScroll();

    if (activeTab_ == 0) {
        drawCurrentsTab();
    } else if (activeTab_ == 1) {
        if (!hasResult_) {
            ImGui::TextDisabled("启动仿真后显示节点电压。");
        } else if (!result_.ok) {
            ImGui::TextColored(Theme::toVec4(Theme::danger()), "%s",
                               result_.error.c_str());
        } else if (resultRevision_ != circuit_.revision) {
            ImGui::TextColored(Theme::toVec4(Theme::warning()),
                               "电路已修改，等待重新仿真。");
        } else {
            const size_t index = liveSampleIndex();
            if (result_.kind == AnalysisKind::Transient) {
                ImGui::TextDisabled("t = %s", formatSI(liveSampleTime(), "s").c_str());
            }
            const ImGuiTableFlags flags = ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_BordersInnerH |
                                          ImGuiTableFlags_ScrollY;
            if (ImGui::BeginTable("##voltage_table", 2, flags, ImVec2(0.0f, 0.0f))) {
                ImGui::TableSetupColumn("节点", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("电压", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted("GND");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted("0 V");

                for (const auto& entry : result_.nodeVoltage) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("节点 %d", entry.first);
                    ImGui::TableSetColumnIndex(1);
                    if (entry.second.empty()) {
                        ImGui::TextDisabled("—");
                    } else {
                        const size_t sample = std::min(index, entry.second.size() - 1);
                        ImGui::TextUnformatted(
                            formatSI(entry.second[sample], "V").c_str());
                    }
                }
                ImGui::EndTable();
            }
        }
    } else if (activeTab_ == 2) {
        if (!hasResult_ || !result_.ok || result_.kind != AnalysisKind::Transient) {
            ImGui::TextDisabled(
                "含正弦或方波电源的电路会在启动后自动进行瞬态分析。");
        } else if (resultRevision_ != circuit_.revision) {
            ImGui::TextColored(Theme::toVec4(Theme::warning()),
                               "电路已修改，等待重新仿真。");
        } else {
            ImGui::BeginChild("##legend", ImVec2(0.0f, dp(36.0f)), 0,
                              ImGuiWindowFlags_HorizontalScrollbar);
            int series = 0;
            for (auto& entry : result_.nodeVoltage) {
                ImGui::PushID(entry.first);
                auto visibleIt = visibleNodes_.find(entry.first);
                if (visibleIt == visibleNodes_.end()) {
                    visibleIt = visibleNodes_.emplace(entry.first, true).first;
                }
                bool& visible = visibleIt->second;
                ImGui::ColorButton("##color", Theme::toVec4(Theme::seriesColor(series)),
                                   ImGuiColorEditFlags_NoTooltip |
                                       ImGuiColorEditFlags_NoDragDrop |
                                       ImGuiColorEditFlags_NoPicker,
                                   ImVec2(dp(14.0f), dp(14.0f)));
                ImGui::SameLine();
                char label[64];
                std::snprintf(label, sizeof(label), "节点 %d", entry.first);
                ImGui::Checkbox(label, &visible);
                ImGui::PopID();
                ImGui::SameLine();
                ++series;
            }
            ImGui::EndChild();
            drawTransientPlot(ImGui::GetContentRegionAvail());
        }
    } else if (activeTab_ == 3) {
        drawScopeTab();
    } else if (activeTab_ == 4) {
        if (ImGui::Button("复制", ImVec2(dp(70.0f), dp(34.0f)))) {
            if (compiled_.ok) {
                SDL_SetClipboardText(compiled_.netlist.c_str());
                setStatus("已复制网表");
            }
        }
        ImGui::SameLine();
        if (!compiled_.ok) {
            ImGui::TextColored(Theme::toVec4(Theme::danger()), "%s",
                               compiled_.error.c_str());
        } else {
            ImGui::TextDisabled("SPICE 网表（只读）");
        }
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::toVec4(Theme::canvas()));
        ImGui::BeginChild("##netlist", ImVec2(0.0f, 0.0f), 0, 0);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(compiled_.ok ? compiled_.netlist.c_str()
                                           : compiled_.error.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    } else {
        // Build the text once per result so it can live in an editable buffer:
        // that makes it selectable (long press + drag) and copyable.
        std::string text;
        if (!engineError_.empty()) {
            text += "引擎：" + engineError_ + "\n";
        }
        if (hasResult_) {
            if (!result_.error.empty()) {
                text += result_.error + "\n";
            }
            if (!result_.log.empty()) {
                text += result_.log;
                if (text.back() != '\n') {
                    text += '\n';
                }
            }
        }

        if (text.empty()) {
            ImGui::TextDisabled("启动仿真后，ngspice 的输出会显示在这里。");
        } else {
            logText_.assign(text.begin(), text.end());
            logText_.push_back('\0');
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::toVec4(Theme::canvas()));
            ImGui::BeginChild("##log_scroll", ImVec2(0.0f, -dp(40.0f)), 0, 0);
            ImGui::InputTextMultiline("##log_text", logText_.data(), logText_.size(),
                                      ImVec2(-FLT_MIN, -FLT_MIN),
                                      ImGuiInputTextFlags_ReadOnly);
            // Remember the selection: the state disappears as soon as another
            // widget takes focus, which is exactly what the copy button does.
            if (ImGuiInputTextState* state = ImGui::GetInputTextState(ImGui::GetItemID())) {
                if (state->HasSelection()) {
                    logSelectionStart_ = state->GetSelectionStart();
                    logSelectionEnd_ = state->GetSelectionEnd();
                } else {
                    logSelectionStart_ = -1;
                    logSelectionEnd_ = -1;
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();

            if (ImGui::Button("复制日志", ImVec2(dp(88.0f), dp(32.0f)))) {
                SDL_SetClipboardText(logText_.data());
                setStatus("已复制日志");
            }
            ImGui::SameLine(0.0f, dp(6.0f));
            const size_t length =
                logText_.empty() ? 0 : logText_.size() - 1;   // drop the NUL
            const size_t selStart = logSelectionStart_ < 0
                                        ? 0
                                        : static_cast<size_t>(logSelectionStart_);
            const size_t selEnd = logSelectionEnd_ < 0
                                      ? 0
                                      : static_cast<size_t>(logSelectionEnd_);
            const bool hasSelection =
                selEnd > selStart && selEnd <= length && selStart < length;
            if (hasSelection) {
                if (accentButton("复制选中", ImVec2(dp(96.0f), dp(32.0f)))) {
                    SDL_SetClipboardText(
                        std::string(logText_.data() + selStart, selEnd - selStart)
                            .c_str());
                    setStatus("已复制选中文本");
                }
            } else if (ImGui::Button("复制选中", ImVec2(dp(96.0f), dp(32.0f)))) {
                setStatus("先在上面的日志里长按拖动选中一段");
            }
            ImGui::SameLine(0.0f, dp(10.0f));
            ImGui::TextDisabled("长按拖动可选择文本");
        }
    }

    finishListDragScroll();
    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void App::drawCurrentsTab()
{
    if (!live_) {
        ImGui::TextDisabled("启动仿真后显示各元件电流。");
        return;
    }
    if (!hasResult_ || simRunning_) {
        ImGui::TextColored(Theme::toVec4(Theme::warning()), "仿真中…");
        return;
    }
    if (!result_.ok) {
        ImGui::TextColored(Theme::toVec4(Theme::danger()), "%s",
                           result_.error.c_str());
        return;
    }
    if (resultRevision_ != circuit_.revision) {
        ImGui::TextColored(Theme::toVec4(Theme::warning()),
                           "电路已修改，等待重新仿真。");
        return;
    }

    const size_t index = liveSampleIndex();
    if (result_.kind == AnalysisKind::Transient) {
        ImGui::TextDisabled("t = %s", formatSI(liveSampleTime(), "s").c_str());
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##current_table", 3, flags, ImVec2(0.0f, 0.0f))) {
        ImGui::TableSetupColumn("元件", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("电流", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& component : circuit_.components) {
            if (component.type == ComponentType::Ground) {
                continue;
            }
            const int pins = pinCount(component.type);
            if (pins <= 2) {
                double current = 0.0;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(component.name.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s", typeName(component.type));
                ImGui::TableSetColumnIndex(2);
                if (currentFor(component, index, &current)) {
                    if (std::fabs(current) < 1e-9) {
                        ImGui::TextUnformatted("0 A");
                    } else {
                        const std::string text =
                            std::string(currentArrow(component, current)) + " " +
                            formatSI(std::fabs(current), "A");
                        ImGui::TextUnformatted(text.c_str());
                    }
                } else {
                    ImGui::TextDisabled("—");
                }
                continue;
            }
            const PinLayout& layout = pinLayout(component.type);
            for (int pin = 0; pin < pins; ++pin) {
                double current = 0.0;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%s · %s", component.name.c_str(),
                            layout.pins[static_cast<size_t>(pin)].name);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s", typeName(component.type));
                ImGui::TableSetColumnIndex(2);
                if (currentForPin(component, pin, index, &current)) {
                    if (std::fabs(current) < 1e-9) {
                        ImGui::TextUnformatted("0 A");
                    } else {
                        ImGui::TextUnformatted(formatSI(current, "A").c_str());
                    }
                } else {
                    ImGui::TextDisabled("—");
                }
            }
        }
        ImGui::EndTable();
    }
}

void App::drawEditorMenuPopup()
{
    if (showEditorMenu_) {
        showEditorMenu_ = false;
        ImGui::SetNextWindowPos(
            ImVec2(safeLeft_ + rootSize_.x - dp(186.0f),
                   safeTop_ + topBarHeight_ + dp(4.0f)),
            ImGuiCond_Always);
        ImGui::OpenPopup("编辑菜单");
    }
    if (!ImGui::BeginPopup("编辑菜单")) {
        return;
    }
    const ImVec2 size(dp(150.0f), dp(38.0f));
    if (ImGui::Button("适应视图", size)) {
        fitPending_ = true;
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::Button("设置", size)) {
        showSettings_ = true;
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::Button("重命名工程", size)) {
        std::snprintf(renameBuffer_, sizeof(renameBuffer_), "%s",
                      currentProjectName_.c_str());
        showRenameDialog_ = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::drawSettingsPopup()
{
    if (showSettings_) {
        showSettings_ = false;
        ImGui::OpenPopup("分析设置");
    }
    if (!ImGui::BeginPopup("分析设置")) {
        return;
    }

    ImGui::TextUnformatted("分析设置");
    ImGui::Separator();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + dp(320.0f));
    ImGui::TextDisabled("含正弦/方波电源或 555 定时器的电路会自动进行瞬态分析。");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    // Steps down to 100 ns so a 50 kHz PWM keeps tens of samples per period.
    static const double steps[] = {100e-9, 1e-6, 10e-6, 100e-6, 1e-3};
    static const char* stepLabels[] = {"100 ns", "1 µs", "10 µs", "100 µs", "1 ms"};
    static const double stops[] = {100e-6, 1e-3, 5e-3, 20e-3, 50e-3};
    static const char* stopLabels[] = {"100 µs", "1 ms", "5 ms", "20 ms", "50 ms"};
    const int stepCount = static_cast<int>(sizeof(steps) / sizeof(steps[0]));
    const int stopCount = static_cast<int>(sizeof(stops) / sizeof(stops[0]));

    int stepIndex = 0;
    double best = std::fabs(transient_.step - steps[0]);
    for (int i = 1; i < stepCount; ++i) {
        const double distance = std::fabs(transient_.step - steps[i]);
        if (distance < best) {
            best = distance;
            stepIndex = i;
        }
    }
    int stopIndex = 0;
    best = std::fabs(transient_.stop - stops[0]);
    for (int i = 1; i < stopCount; ++i) {
        const double distance = std::fabs(transient_.stop - stops[i]);
        if (distance < best) {
            best = distance;
            stopIndex = i;
        }
    }

    ImGui::SetNextItemWidth(dp(190.0f));
    if (ImGui::Combo("步长", &stepIndex, stepLabels, stepCount)) {
        transient_.step = steps[stepIndex];
        if (live_) {
            scheduleLiveRerun(200);
        }
    }
    ImGui::SameLine(0.0f, dp(8.0f));
    if (ImGui::Button("键盘输入", ImVec2(dp(110.0f), dp(38.0f)))) {
        openNumberEditor(-1, NumberField::TransientStep);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SetNextItemWidth(dp(190.0f));
    if (ImGui::Combo("停止时间", &stopIndex, stopLabels, stopCount)) {
        transient_.stop = stops[stopIndex];
        if (live_) {
            scheduleLiveRerun(200);
        }
    }
    ImGui::SameLine(0.0f, dp(8.0f));
    if (ImGui::Button("键盘输入##stop", ImVec2(dp(110.0f), dp(38.0f)))) {
        openNumberEditor(-1, NumberField::TransientStop);
        ImGui::CloseCurrentPopup();
    }
    ImGui::TextDisabled("点数约 %.0f（上限 200000）",
                        transient_.stop / transient_.step + 1.0);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + dp(320.0f));
    if (flowAverageSamples_ > 1) {
        ImGui::TextDisabled("画布上的电子流动与读数按 1 个开关周期取平均"
                            "（约 %d 个采样点），否则 50 kHz 的脉动电流在 60 fps 下会看起来乱跳。",
                            static_cast<int>(flowAverageSamples_));
    } else {
        ImGui::TextDisabled("画布上的电子流动与读数显示当前采样时刻的瞬时值。");
    }
    ImGui::PopTextWrapPos();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + dp(320.0f));
    ImGui::TextDisabled("开关电源需要足够长的窗口才能进入稳态，"
                        "又需要足够小的步长才能看清纹波；这里只是下限，"
                        "含 PWM 信号源时会自动放宽。");
    ImGui::PopTextWrapPos();
    ImGui::TextDisabled("界面帧率 %.0f fps", ImGui::GetIO().Framerate);
    ImGui::SameLine(0.0f, dp(12.0f));
    // Build stamp: makes it obvious which APK is actually installed.
    ImGui::TextDisabled("版本 0.1.0 · 构建 %s %s", __DATE__, __TIME__);
    ImGui::Separator();

    const ImVec2 size(dp(140.0f), dp(44.0f));
    if (ImGui::Button("重命名工程", size)) {
        std::snprintf(renameBuffer_, sizeof(renameBuffer_), "%s",
                      currentProjectName_.c_str());
        showRenameDialog_ = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("关闭", size)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------

void App::startLive()
{
    if (!engineError_.empty()) {
        setStatus(engineError_);
        return;
    }
    // A fresh run repairs loads burned by the previous run, so parameters can
    // be adjusted and tried again without a manual repair step.
    restoreDamagedLoads();
    live_ = true;
    liveClock_ = 0.0;
    flowParticles_.clear();
    flowReady_ = false;
    requestLiveRun();
}

void App::pauseLive()
{
    live_ = false;
    liveRun_ = false;
    liveRerunPending_ = false;
    liveClock_ = 0.0;
    flowParticles_.clear();
    flowReady_ = false;
    setStatus("已暂停");
}

void App::scheduleLiveRerun(uint64_t delayMs)
{
    liveRerunAt_ = SDL_GetTicks() + delayMs;
    liveRerunPending_ = true;
}

void App::requestLiveRun()
{
    if (!live_ || screen_ != Screen::Editor) {
        return;
    }
    if (simRunning_) {
        liveRerunPending_ = true;
        liveRerunAt_ = SDL_GetTicks() + 150;
        return;
    }
    updateCompiled();
    const AnalysisKind kind =
        hasTimeVaryingSource() ? AnalysisKind::Transient : AnalysisKind::OperatingPoint;
    if (!compiled_.ok) {
        // Keep the reason around: the status bar alone cannot say which part is
        // wrong, and the log tab is where the user looks.
        result_ = SimulationResult{};
        result_.ok = false;
        result_.kind = kind;
        result_.revision = circuit_.revision;
        result_.error = compiled_.error;
        result_.log = compiled_.error;
        hasResult_ = true;
        resultRevision_ = circuit_.revision;
        probeStats_.clear();
        probeSeriesCache_.clear();
        status_ = "电路有错误 · 详见 结果 → 日志";
        if (resultsOpen_) {
            activeTab_ = 5;
        }
        return;
    }
    startSimulation(kind, true);
}

void App::updateFlowSmoothing()
{
    flowAverageSamples_ = 1;
    if (!result_.ok || result_.kind != AnalysisKind::Transient || result_.time.size() < 8) {
        return;
    }
    double period = 0.0;
    for (const auto& component : circuit_.components) {
        if (!isSourceType(component.type) || component.source.mode != Waveform::Pulse) {
            continue;
        }
        double candidate = component.source.period;
        if (!(candidate > 0.0)) {
            candidate = 1.0 / std::max(1e-9, component.source.frequency);
        }
        if (candidate > 0.0 && (period <= 0.0 || candidate < period)) {
            period = candidate;
        }
    }
    if (!(period > 0.0)) {
        return;
    }
    const double span = result_.time.back() - result_.time.front();
    if (!(span > 0.0)) {
        return;
    }
    // The playback squeezes the whole window into a few seconds. When one frame
    // advances more than half a switching period, consecutive frames land on
    // random phases and a switching waveform looks like it reverses at random.
    constexpr double kPlaybackSeconds = 3.0;
    constexpr double kFrameSeconds = 1.0 / 60.0;
    const double advance = span * (kFrameSeconds / kPlaybackSeconds);
    if (period > advance * 0.5) {
        return;
    }
    const double spacing = span / static_cast<double>(result_.time.size() - 1);
    const long long count =
        static_cast<long long>(std::llround(period / std::max(1e-12, spacing)));
    const long long cap = static_cast<long long>(result_.time.size() / 8);
    flowAverageSamples_ = static_cast<size_t>(std::clamp<long long>(count, 1, std::max<long long>(1, cap)));
}

double App::smoothedValue(const std::vector<double>& series, size_t index) const
{
    if (series.empty()) {
        return 0.0;
    }
    const size_t clamped = std::min(index, series.size() - 1);
    if (flowAverageSamples_ <= 1 || series.size() < 3) {
        return series[clamped];
    }
    const size_t half = flowAverageSamples_ / 2;
    const size_t begin = clamped > half ? clamped - half : 0;
    const size_t end = std::min(series.size() - 1, clamped + half);
    double sum = 0.0;
    size_t count = 0;
    for (size_t i = begin; i <= end; ++i) {
        if (!std::isfinite(series[i])) {
            continue;
        }
        sum += series[i];
        ++count;
    }
    return count > 0 ? sum / static_cast<double>(count) : series[clamped];
}

int App::bestResultTab() const
{
    for (const auto& component : circuit_.components) {
        if (isProbeType(component.type)) {
            return 3;
        }
    }
    return hasTimeVaryingSource() ? 2 : 1;
}

bool App::loopsLivePlayback() const
{
    for (const auto& component : circuit_.components) {
        if (isSourceType(component.type) && component.source.mode != Waveform::DC) {
            return true;
        }
    }
    return false;
}

bool App::hasTimeVaryingSource() const
{
    for (const auto& component : circuit_.components) {
        if (forcesTransient(component.type)) {
            return true;
        }
        if (isSourceType(component.type) && component.source.mode != Waveform::DC) {
            return true;
        }
    }
    return false;
}

void App::startSimulation(AnalysisKind kind, bool liveRun)
{
    if (simRunning_ || !engine_) {
        return;
    }

    updateCompiled();
    if (!compiled_.ok) {
        result_ = SimulationResult{};
        result_.ok = false;
        result_.kind = kind;
        result_.revision = circuit_.revision;
        result_.error = compiled_.error;
        result_.log = compiled_.error;
        hasResult_ = true;
        resultRevision_ = circuit_.revision;
        liveRun_ = false;
        status_ = "电路有错误";
        if (resultsOpen_) {
            activeTab_ = 5;
        }
        return;
    }

    TransientSpec spec = transient_;
    if (kind == AnalysisKind::Transient) {
        double shortestPeriod = 0.0;
        for (const auto& component : circuit_.components) {
            if (!isSourceType(component.type) || component.source.mode == Waveform::DC) {
                continue;
            }
            double period = 0.0;
            if (component.source.mode == Waveform::Sine) {
                period = 1.0 / std::max(1e-9, component.source.frequency);
            } else {
                period = component.source.period > 0.0
                             ? component.source.period
                             : 1.0 / std::max(1e-9, component.source.frequency);
            }
            if (period > 0.0 &&
                (shortestPeriod <= 0.0 || period < shortestPeriod)) {
                shortestPeriod = period;
            }
        }
        if (shortestPeriod > 0.0) {
            if (shortestPeriod <= 200e-6) {
                // Switching supply (>= 5 kHz). An open-loop LC filter rings on
                // startup at its own resonant frequency, so the window has to
                // cover many periods, and the ripple needs tens of samples per
                // period. The point count stays bounded for the phone.
                spec.stop = std::min(std::max(spec.stop, shortestPeriod * 1500.0), 50e-3);
                spec.step = std::min(spec.step, shortestPeriod / 20.0);
                const double maxPoints = 100000.0;
                if (spec.stop / spec.step > maxPoints) {
                    spec.step = spec.stop / maxPoints;
                }
            } else {
                spec.stop = std::max(spec.stop, shortestPeriod * 2.0);
                spec.step = std::min(spec.step, spec.stop / 800.0);
                if (spec.stop / spec.step > 10000.0) {
                    spec.step = spec.stop / 10000.0;
                }
            }
        }
        bool hasOscillator = false;
        for (const auto& component : circuit_.components) {
            if (forcesTransient(component.type)) {
                hasOscillator = true;
                break;
            }
        }
        if (hasOscillator) {
            spec.stop = std::max(spec.stop, 50e-3);
            spec.step = std::min(spec.step, 50e-6);
            // Same point budget as the switching rule: clamping to the old
            // 10 k limit would blur a fast ripple in a mixed circuit.
            if (spec.stop / spec.step > 100000.0) {
                spec.step = spec.stop / 100000.0;
            }
        }
    }

    SimulationRequest request;
    request.circuit = circuit_;
    request.kind = kind;
    request.transient = spec;
    request.revision = circuit_.revision;
    activeSimulationGeneration_ = ++simulationGeneration_;
    future_ = engine_->runAsync(std::move(request));
    simRunning_ = true;
    liveRun_ = liveRun;
    status_ = "仿真中…";
}

void App::pollSimulation()
{
    if (!simRunning_ || !future_.valid()) {
        return;
    }
    if (future_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        return;
    }

    SimulationResult incoming;
    try {
        incoming = future_.get();
    } catch (const std::exception& exception) {
        incoming = SimulationResult{};
        incoming.ok = false;
        incoming.error = std::string("仿真异常：") + exception.what();
        incoming.log = incoming.error;
    } catch (...) {
        incoming = SimulationResult{};
        incoming.ok = false;
        incoming.error = "仿真发生未知异常。";
        incoming.log = incoming.error;
    }

    simRunning_ = false;
    const bool wasLive = liveRun_;
    liveRun_ = false;
    if (activeSimulationGeneration_ != simulationGeneration_) {
        // The project changed while this run was in flight; never expose its
        // vectors against the new circuit.
        if (live_) {
            scheduleLiveRerun(120);
        }
        return;
    }

    result_ = std::move(incoming);
    hasResult_ = true;
    resultRevision_ = result_.revision;
    if (!loopsLivePlayback()) {
        // A one-off transient plays from the start every time a fresh result
        // arrives, so changing a parameter replays the whole cooldown instead
        // of leaving the readout pinned at its old end state.
        liveClock_ = 0.0;
    }
    refreshProbeCache();
    updateFlowSmoothing();

    if (!result_.ok) {
        activeTab_ = 5;
        status_ = "仿真失败";
        if (wasLive && live_) {
            scheduleLiveRerun(1200);
        }
        return;
    }

    // Keep the user's waveform legend choices across live reruns. If the node
    // set changed (topology edit), start over so stale numbers cannot hide a
    // different node.
    bool sameNodes = visibleNodes_.size() == result_.nodeVoltage.size();
    if (sameNodes) {
        for (const auto& entry : result_.nodeVoltage) {
            if (visibleNodes_.find(entry.first) == visibleNodes_.end()) {
                sameNodes = false;
                break;
            }
        }
    }
    if (!sameNodes) {
        visibleNodes_.clear();
        for (const auto& entry : result_.nodeVoltage) {
            visibleNodes_[entry.first] = true;
        }
    }

    if (wasLive && live_) {
        liveClock_ = 0.0;
        status_ = "运行中";
        if (resultRevision_ != circuit_.revision) {
            scheduleLiveRerun(250);
        }
        if (liveRerunPending_) {
            liveRerunPending_ = false;
            scheduleLiveRerun(180);
        }
    }
}

size_t App::liveSampleIndex() const
{
    if (result_.kind != AnalysisKind::Transient || result_.time.empty()) {
        return 0;
    }
    const double first = result_.time.front();
    const double last = result_.time.back();
    const double span = last - first;
    if (!(span > 0.0)) {
        return 0;
    }
    // Play the whole transient window over a few seconds, otherwise a 1 kHz
    // waveform would flicker far too fast to observe. Periodic circuits repeat
    // the window; a one-off transient (a Peltier plate cooling down, an RC
    // charging) plays once and then holds its final state instead of snapping
    // back to the start.
    const double playbackSeconds = 3.0;
    const double elapsed = std::max(0.0, liveClock_);
    const double phase =
        loopsLivePlayback()
            ? std::fmod(elapsed, playbackSeconds) / playbackSeconds
            : std::clamp(elapsed / playbackSeconds, 0.0, 1.0);
    double time = first + phase * span;
    const auto found = std::lower_bound(result_.time.begin(), result_.time.end(), time);
    size_t index = static_cast<size_t>(found - result_.time.begin());
    if (index >= result_.time.size()) {
        return result_.time.size() - 1;
    }
    if (index > 0) {
        const double before = result_.time[index - 1];
        const double after = result_.time[index];
        if (std::fabs(before - time) < std::fabs(after - time)) {
            --index;
        }
    }
    return index;
}

double App::liveSampleTime() const
{
    if (result_.time.empty()) {
        return 0.0;
    }
    const size_t index = std::min(liveSampleIndex(), result_.time.size() - 1);
    return result_.time[index];
}

bool App::currentFor(const Component& component, size_t index, double* current) const
{
    return currentForPin(component, 0, index, current);
}

bool App::currentForPin(const Component& component, int pin, size_t index,
                        double* current) const
{
    if (!current || !live_ || !hasResult_ || !result_.ok ||
        resultRevision_ != circuit_.revision) {
        return false;
    }
    if (component.type == ComponentType::Ground) {
        return false;
    }

    const auto pins = result_.pinCurrent.find(component.id);
    if (pins != result_.pinCurrent.end() && pin >= 0 &&
        pin < static_cast<int>(pins->second.size())) {
        const std::vector<double>& values = pins->second[static_cast<size_t>(pin)];
        if (!values.empty()) {
            *current = smoothedValue(values, index);
            return std::isfinite(*current);
        }
    }

    if (pin != 0 || pinCount(component.type) != 2 ||
        !isResistiveType(component.type)) {
        return false;
    }
    const auto nodes = compiled_.nodes.find(component.id);
    if (nodes == compiled_.nodes.end() || nodes->second.a() < 0 || nodes->second.b() < 0) {
        return false;
    }

    auto readVoltage = [&](int node, double* value) -> bool {
        if (node <= 0) {
            *value = 0.0;
            return true;
        }
        const auto entry = result_.nodeVoltage.find(node);
        if (entry == result_.nodeVoltage.end() || entry->second.empty()) {
            return false;
        }
        *value = smoothedValue(entry->second, index);
        return std::isfinite(*value);
    };

    double va = 0.0;
    double vb = 0.0;
    if (!readVoltage(nodes->second.a(), &va) || !readVoltage(nodes->second.b(), &vb)) {
        return false;
    }
    const double resistance = std::max(1e-9, componentResistance(component));
    *current = (va - vb) / resistance;
    return std::isfinite(*current);
}

bool App::voltageAcross(const Component& component, size_t index, double* voltage) const
{
    if (!voltage || !live_ || !hasResult_ || !result_.ok ||
        resultRevision_ != circuit_.revision) {
        return false;
    }
    const auto nodes = compiled_.nodes.find(component.id);
    if (nodes == compiled_.nodes.end() || nodes->second.a() < 0 || nodes->second.b() < 0) {
        return false;
    }
    const auto read = [&](int node, double* value) -> bool {
        if (node <= 0) {
            *value = 0.0;
            return true;
        }
        const auto entry = result_.nodeVoltage.find(node);
        if (entry == result_.nodeVoltage.end() || entry->second.empty()) {
            return false;
        }
        *value = smoothedValue(entry->second, index);
        return std::isfinite(*value);
    };
    double a = 0.0;
    double b = 0.0;
    if (!read(nodes->second.a(), &a) || !read(nodes->second.b(), &b)) {
        return false;
    }
    *voltage = a - b;
    return true;
}

double App::loadIntensity(const Component& component, size_t index) const
{
    if (!isLoadType(component.type) || component.damaged) {
        return 0.0;
    }
    double voltage = 0.0;
    if (!voltageAcross(component, index, &voltage)) {
        return 0.0;
    }
    return loadLevel(component, voltage);
}

void App::checkLoadDamage()
{
    if (!live_ || !hasResult_ || !result_.ok || simRunning_ ||
        resultRevision_ != circuit_.revision) {
        return;
    }
    const size_t index = liveSampleIndex();
    bool damagedAny = false;
    for (auto& component : circuit_.components) {
        if (!isBurnableType(component.type) || component.damaged) {
            continue;
        }
        double voltage = 0.0;
        if (!voltageAcross(component, index, &voltage)) {
            continue;
        }
        // The current is what the load actually draws at that voltage: use the
        // simulated branch current when there is one (a Peltier module is not a
        // plain resistor), otherwise fall back to V / R.
        double current = 0.0;
        if (!currentFor(component, index, &current) || !std::isfinite(current)) {
            const double resistance = std::max(1e-6, component.value);
            current = voltage / resistance;
        }
        const LoadFault fault = loadFault(component, voltage, current);
        if (fault != LoadFault::None) {
            component.damaged = true;
            component.damagedByCurrent =
                fault == LoadFault::OverCurrent || fault == LoadFault::OverPower;
            damagedAny = true;
        }
    }
    if (damagedAny) {
        circuit_.touch();
        // Damage is a transient, recoverable state: update the netlist but do
        // not auto-restore the loads here, or the damage effect would vanish
        // on the rerun that this change triggers.
        markChanged(false);
        setStatus("负载超出额定值，已损坏（可修复）");
    }
}

ImVec2 App::componentCurrentDirection(const Component& component) const
{
    Point pins[kMaxComponentPins];
    const int count = componentPins(component, pins);
    if (count < 2) {
        return ImVec2(0.0f, 1.0f);
    }
    const float dx = static_cast<float>(pins[1].x - pins[0].x);
    const float dy = static_cast<float>(pins[1].y - pins[0].y);
    if (std::fabs(dx) >= std::fabs(dy)) {
        return ImVec2(dx >= 0.0f ? 1.0f : -1.0f, 0.0f);
    }
    return ImVec2(0.0f, dy >= 0.0f ? 1.0f : -1.0f);
}

const char* App::currentArrow(const Component& component, double current) const
{
    const ImVec2 direction = componentCurrentDirection(component);
    const bool positive = current >= 0.0;
    if (std::fabs(direction.x) > std::fabs(direction.y)) {
        return (direction.x > 0.0f) == positive ? "→" : "←";
    }
    return (direction.y > 0.0f) == positive ? "↓" : "↑";
}

// ---------------------------------------------------------------------------
// State helpers and autosave
// ---------------------------------------------------------------------------

void App::setStatus(const std::string& text)
{
    status_ = text;
}

void App::markDirty()
{
    if (currentProjectIsExample_) {
        // Editing an example must not overwrite it.
        forkExampleProject();
    }
    dirty_ = true;
    dirtyAt_ = SDL_GetTicks();
}

void App::markChanged(bool restoreLoads)
{
    if (restoreLoads) {
        restoreDamagedLoads();
    }
    updateCompiled();
    markDirty();
    flowParticles_.clear();
    flowReady_ = false;
    if (live_) {
        scheduleLiveRerun(350);
    }
}

bool App::restoreDamagedLoads()
{
    if (clearLoadDamage(circuit_) == 0) {
        return false;
    }
    markDirty();
    if (status_ == "负载过压损坏") {
        status_ = "负载已恢复";
    }
    return true;
}

void App::updateCompiled()
{
    compiled_ = compileCircuit(circuit_);
    // Wires that cross without touching: the later one is drawn with a bridge.
    wireCrossings_ = findWireCrossings(circuit_);
}

void App::updateListDragScroll()
{
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 windowPos = ImGui::GetWindowPos();
    const float scrollbar = ImGui::GetStyle().ScrollbarSize;
    const bool overScrollbar =
        io.MousePos.x > windowPos.x + ImGui::GetWindowWidth() - scrollbar - dp(4.0f);
    const bool hovered = ImGui::IsWindowHovered(
        ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    if (!listDragHeld_ && hovered && !overScrollbar && ImGui::IsMouseClicked(0)) {
        listDragHeld_ = true;
        listDragging_ = false;
        listDragReleased_ = false;
        listDragStart_ = io.MousePos;
        listDragScroll_ = ImGui::GetScrollY();
    }
    if (!listDragHeld_) {
        return;
    }
    if (!ImGui::IsMouseDown(0)) {
        // Keep listDragging() set for this frame so a card under the finger does
        // not fire on the release that ended a swipe.
        listDragReleased_ = true;
        return;
    }
    const ImVec2 delta(io.MousePos.x - listDragStart_.x, io.MousePos.y - listDragStart_.y);
    if (!listDragging_ &&
        (std::fabs(delta.y) > dp(9.0f) || std::fabs(delta.x) > dp(9.0f))) {
        listDragging_ = true;
    }
    if (listDragging_) {
        const float maxScroll = std::max(0.0f, ImGui::GetScrollMaxY());
        ImGui::SetScrollY(std::clamp(listDragScroll_ - delta.y, 0.0f, maxScroll));
    }
}

void App::finishListDragScroll()
{
    if (listDragReleased_ || !ImGui::IsMouseDown(0)) {
        listDragHeld_ = false;
        listDragReleased_ = false;
        listDragging_ = false;
    }
}

void App::pushUndo()
{
    if (undoStack_.size() >= 20) {
        undoStack_.erase(undoStack_.begin());
    }
    undoStack_.push_back(circuit_);
    redoStack_.clear();
}

void App::undo()
{
    if (undoStack_.empty()) {
        return;
    }
    if (redoStack_.size() >= 20) {
        redoStack_.erase(redoStack_.begin());
    }
    redoStack_.push_back(circuit_);
    // Revisions must stay strictly increasing even though the circuit state
    // moves backwards, otherwise an old result could match the restored
    // revision and be shown for a different circuit.
    const int nextRevision = circuit_.revision + 1;
    circuit_ = undoStack_.back();
    circuit_.revision = nextRevision;
    undoStack_.pop_back();
    selectedComponent_ = -1;
    selectedWire_ = -1;
    wireStarted_ = false;
    updateCompiled();
    markDirty();
    if (live_) {
        scheduleLiveRerun(250);
    }
    setStatus("已撤销");
}

void App::redo()
{
    if (redoStack_.empty()) {
        return;
    }
    if (undoStack_.size() >= 20) {
        undoStack_.erase(undoStack_.begin());
    }
    undoStack_.push_back(circuit_);
    const int nextRevision = circuit_.revision + 1;
    circuit_ = redoStack_.back();
    circuit_.revision = nextRevision;
    redoStack_.pop_back();
    selectedComponent_ = -1;
    selectedWire_ = -1;
    wireStarted_ = false;
    updateCompiled();
    markDirty();
    if (live_) {
        scheduleLiveRerun(250);
    }
    setStatus("已重做");
}

void App::autosaveIfNeeded()
{
    if (!dirty_ || currentProjectPath_.empty()) {
        return;
    }
    if (SDL_GetTicks() - dirtyAt_ < 300) {
        return;
    }
    saveCurrentProject();
}

void App::setTool(Tool tool)
{
    tool_ = tool;
    placement_ = Placement{};
    if (tool != Tool::Wire) {
        wireStarted_ = false;
    }
    if (tool != Tool::Select) {
        dragging_ = false;
        dragComponent_ = -1;
    }
}

void App::beginPlacement(ComponentType type, Waveform waveform)
{
    setTool(Tool::Select);
    placement_.active = true;
    placement_.type = type;
    placement_.waveform = waveform;
    closeSheet();
    setStatus(std::string("正在放置 ") + componentInfo(type).label);
}

void App::endPlacement()
{
    placement_ = Placement{};
    setStatus("就绪");
}

void App::openSheet()
{
    resultsOpen_ = false;
    sheetLevel_ = SheetLevel::Categories;
    setStatus("选择元件分类");
}

void App::closeSheet()
{
    sheetLevel_ = SheetLevel::Closed;
}

const char* App::typeName(ComponentType type) const
{
    return componentInfo(type).label;
}

std::string App::projectTitle() const
{
    return currentProjectName_.empty() ? "未命名工程" : currentProjectName_;
}

}  // namespace circuitlab
