#pragma once

#include "circuit/Circuit.h"
#include "circuit/ComponentCatalog.h"
#include "circuit/Flow.h"
#include "circuit/Measure.h"
#include "circuit/Netlist.h"
#include "circuit/ProjectIO.h"
#include "circuit/Simulation.h"

#include "imgui.h"

#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace circuitlab {

class App {
public:
    enum class Tool {
        Select,
        Wire,
    };

    enum class Screen {
        Home,
        Editor,
    };

    // Two-level component picker: categories first, then a component grid.
    enum class SheetLevel {
        Closed,
        Categories,
        Components,
    };

    struct Placement {
        bool active = false;
        ComponentType type = ComponentType::Resistor;
        Waveform waveform = Waveform::DC;
    };

    App();
    ~App();

    void init(const std::string& baseDir, float uiScale);
    void frame();

    // Safe-area insets in physical pixels (display cutout / system bars).
    void setSafeInsets(float left, float top, float right, float bottom);

    // Two-finger pinch. `centerNormalized` uses the window's 0..1 coordinates.
    void pinchBegin();
    void pinchZoom(float scale, ImVec2 centerNormalized);
    void pinchEnd();
    bool pinching() const { return pinching_; }

    void onAppBackground();

private:
    // Screens
    void drawHome();
    void drawEditor();
    void drawCreateDialog();
    void drawDeleteDialog();
    void drawRenameDialog();
    void drawSettingsPopup();

    // Editor layout
    void drawTopBar(float height);
    void drawBottomBar(float height);
    void drawEditorOverlays();
    void drawFloatingTools();
    void drawZoomControls();
    void drawPlacementChip();
    void drawAddButton();
    void drawComponentSheet();
    void drawSheetCategories(const ImVec2& size);
    void drawSheetComponents(const ImVec2& size);
    void drawEditorMenuPopup();
    void drawSelectionCard();
    void drawResultsOverlay();
    void drawCurrentsTab();
    void drawTransientPlot(const ImVec2& size);
    // Oscilloscope: differential trace of every probe plus its steady-state
    // numbers (average, ripple, RMS, frequency).
    void drawScopeTab();
    void drawScopePlot(const ImVec2& size, const std::vector<const Component*>& probes);
    std::vector<double> probeSeries(const Component& component) const;
    void refreshProbeCache();
    // One switching period of sample averaging, used by the electron-flow
    // animation and the on-canvas readouts, so a 50 kHz waveform does not alias
    // into random directions at 60-120 fps.
    void updateFlowSmoothing();
    double smoothedValue(const std::vector<double>& series, size_t index) const;

    // Projects
    void refreshProjects();
    // Adds the bundled example project once per install.
    void seedExampleProjects();
    // Examples stay pristine: the first edit forks them into a new project.
    bool forkExampleProject();
    std::string uniqueProjectName(const std::string& base) const;
    // example: -1 = empty, 0 = divider, 1 = RC charging.
    bool createProject(const std::string& name, int example);
    bool openProject(const std::string& path);
    void goHome();
    bool saveCurrentProject(std::string* error = nullptr);
    void deleteProjectAt(const std::string& path);
    void loadLegacyAutosave();

    // Schematic rendering / hit testing
    ImVec2 worldToScreen(Point point) const;
    ImVec2 screenToWorld(ImVec2 screen) const;
    ImVec2 componentPoint(const Component& component, float localX, float localY) const;
    Point snapScreen(ImVec2 screen) const;
    float gridPixels() const;
    void fitView();
    int hitComponent(ImVec2 screen) const;
    int hitWire(ImVec2 screen) const;
    bool nearestConnection(ImVec2 screen, Point* point) const;
    void drawGrid(ImDrawList* drawList, const ImVec2& min, const ImVec2& max);
    void drawWire(ImDrawList* drawList, const Wire& wire, bool selected);
    void drawWireFlow(ImDrawList* drawList);
    void rebuildFlowNetwork();
    void updateFlowCurrents(size_t sample);
    float flowEdgePixels(int edgeIndex) const;
    ImVec2 flowEdgePoint(int edgeIndex, float t) const;
    // Radius of a jumper bridge in pixels, shared by the wire drawing and the
    // electron stream so the two always match.
    float bridgeRadiusPixels() const;
    void drawComponent(ImDrawList* drawList, const Component& component, bool selected);
    void drawGhost(ImDrawList* drawList, ImVec2 mouse);
    void drawCanvas(float height);
    void handleCanvasTap(const ImVec2& mouse);

    // Simulation
    bool hasTimeVaryingSource() const;
    // Result tab that answers "did it work?" for the current circuit:
    // 3 = 示波 when a probe exists, 2 = 波形 for other transients, 1 = 电压.
    int bestResultTab() const;
    void startLive();
    void pauseLive();
    void requestLiveRun();
    void startSimulation(AnalysisKind kind, bool liveRun);
    void pollSimulation();
    size_t liveSampleIndex() const;
    double liveSampleTime() const;
    bool currentFor(const Component& component, size_t index, double* current) const;
    bool currentForPin(const Component& component, int pin, size_t index,
                       double* current) const;
    bool voltageAcross(const Component& component, size_t index, double* voltage) const;
    double loadIntensity(const Component& component, size_t index) const;
    void checkLoadDamage();
    ImVec2 componentCurrentDirection(const Component& component) const;
    const char* currentArrow(const Component& component, double current) const;
    void scheduleLiveRerun(uint64_t delayMs);

    // Touch drag scrolling for card lists and scrollable panels. A swipe
    // scrolls the list instead of activating the card under the finger.
    // Call updateListDragScroll() right after BeginChild and
    // finishListDragScroll() before EndChild; card activation must check
    // listDragging().
    void updateListDragScroll();
    void finishListDragScroll();
    bool listDragging() const { return listDragging_; }

    // Actions / state helpers
    void pushUndo();
    void undo();
    void redo();
    void markChanged(bool restoreLoads = true);
    bool restoreDamagedLoads();
    void markDirty();
    void updateCompiled();
    void autosaveIfNeeded();
    void setStatus(const std::string& text);
    void setTool(Tool tool);
    const char* typeName(ComponentType type) const;
    void beginPlacement(ComponentType type, Waveform waveform);
    void endPlacement();
    void openSheet();
    void closeSheet();
    bool sheetOpen() const { return sheetLevel_ != SheetLevel::Closed; }
    std::string projectTitle() const;
    float dp(float value) const { return value * uiScale_; }

    // State
    Circuit circuit_;
    CompiledCircuit compiled_;
    std::vector<WireCrossing> wireCrossings_;
    std::vector<Circuit> undoStack_;
    std::vector<Circuit> redoStack_;
    int selectedComponent_ = -1;
    int selectedWire_ = -1;

    Tool tool_ = Tool::Select;
    Placement placement_;

    SheetLevel sheetLevel_ = SheetLevel::Closed;
    ComponentCategory sheetCategory_ = ComponentCategory::Basic;
    float sheetAnim_ = 0.0f;
    bool panCandidate_ = false;
    bool panningCanvas_ = false;
    ImVec2 panStartOffset_{0, 0};

    bool wireStarted_ = false;
    Point wireStart_{};

    bool dragging_ = false;
    int dragComponent_ = -1;
    ImVec2 dragOffsetWorld_{0, 0};
    bool dragUndoPushed_ = false;

    bool canvasPressed_ = false;
    ImVec2 canvasPressPos_{0, 0};
    bool suppressTap_ = false;

    bool listDragHeld_ = false;
    bool listDragging_ = false;
    bool listDragReleased_ = false;
    ImVec2 listDragStart_{0, 0};
    float listDragScroll_ = 0.0f;

    float uiScale_ = 1.0f;
    float rawInsetLeft_ = 0.0f;
    float rawInsetTop_ = 0.0f;
    float rawInsetRight_ = 0.0f;
    float rawInsetBottom_ = 0.0f;
    float safeLeft_ = 0.0f;
    float safeTop_ = 0.0f;
    float safeRight_ = 0.0f;
    float safeBottom_ = 0.0f;
    bool portrait_ = false;

    float zoom_ = 1.0f;
    ImVec2 viewOffset_{0, 0};
    bool fitPending_ = true;
    ImVec2 canvasOrigin_{0, 0};
    ImVec2 canvasSize_{0, 0};
    ImVec2 lastDisplaySize_{0, 0};

    bool pinching_ = false;

    std::unique_ptr<NgspiceEngine> engine_;
    std::future<SimulationResult> future_;
    bool simRunning_ = false;
    uint64_t simulationGeneration_ = 0;
    uint64_t activeSimulationGeneration_ = 0;
    bool live_ = false;
    bool liveRun_ = false;
    bool hasResult_ = false;
    SimulationResult result_;
    int resultRevision_ = -1;
    std::map<int, bool> visibleNodes_;
    std::map<int, bool> visibleProbes_;
    // Oscilloscope results, refreshed once per successful simulation run so the
    // UI never recomputes a full transient window per frame.
    std::map<int, WaveStats> probeStats_;
    std::map<int, std::vector<double>> probeSeriesCache_;
    size_t flowAverageSamples_ = 1;
    // Oscilloscope time window: 0 = whole transient, 1 = settled tail,
    // 2 = about two switching periods.
    int scopeSpan_ = 2;
    TransientSpec transient_;
    double liveClock_ = 0.0;
    uint64_t liveRerunAt_ = 0;
    bool liveRerunPending_ = false;

    // Electron stream state. The network rebuilds when the schematic
    // revision changes; particles survive simulation updates so AC current
    // changes only reverse their motion.
    struct FlowParticle {
        int edge = 0;
        float t = 0.0f;
    };
    // A jumper bridge on one flow edge: where it sits (0..1 along the edge) and
    // which way the electrons bulge, so the stream follows the arc.
    // Glow sprite emitted by a lamp or LED, drawn after every symbol so the
    // light is not hidden behind wires or neighbouring parts.
    struct Glow {
        ImVec2 center{0.0f, 0.0f};
        float radius = 0.0f;
        ImU32 color = 0;
        bool core = false;
    };
    struct FlowHop {
        float t = 0.0f;
        ImVec2 outward{0.0f, -1.0f};
    };
    FlowNetwork flowNetwork_;
    std::map<int, std::vector<FlowHop>> flowEdgeHops_;
    std::vector<Glow> pendingGlows_;
    std::vector<FlowParticle> flowParticles_;
    bool flowReady_ = false;
    int flowRevision_ = -1;
    uint32_t flowRandom_ = 0x9e3779b9u;

    // Editable copy of the log text so it can be selected and copied.
    std::vector<char> logText_;
    int logSelectionStart_ = -1;
    int logSelectionEnd_ = -1;

    std::string engineError_;
    std::string status_;
    int activeTab_ = 0;
    bool showSettings_ = false;
    bool showEditorMenu_ = false;
    bool resultsOpen_ = false;

    // Project state
    Screen screen_ = Screen::Home;
    std::string baseDir_;
    std::string projectsDir_;
    std::vector<ProjectInfo> projects_;
    std::string currentProjectPath_;
    std::string currentProjectName_;
    // True while the open project is a bundled example; editing it saves a copy.
    bool currentProjectIsExample_ = false;
    bool dirty_ = false;
    uint64_t dirtyAt_ = 0;
    std::string toast_;
    uint64_t toastAt_ = 0;

    bool showCreateDialog_ = false;
    bool showDeleteDialog_ = false;
    bool showRenameDialog_ = false;
    std::string deletePath_;
    std::string deleteName_;
    char newProjectName_[96] = {};
    char renameBuffer_[96] = {};

    // Layout positions captured each frame for the overlay panels.
    ImVec2 rootMin_{0, 0};
    ImVec2 rootSize_{0, 0};
    float topBarHeight_ = 0.0f;
    float bottomBarHeight_ = 0.0f;
    ImVec2 addButtonCenter_{0, 0};
    float addButtonRadius_ = 0.0f;
};

}  // namespace circuitlab
