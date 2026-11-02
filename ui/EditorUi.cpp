// Modern editor chrome: bottom bar, floating tool cluster, zoom controls,
// placement chip and the two-level component sheet.

#include "App.h"

#include "Symbols.h"
#include "Theme.h"

#include "circuit/ComponentCatalog.h"
#include "circuit/Engineering.h"
#include "circuit/Thermoelectric.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace circuitlab {
namespace {

struct PickerItem {
    ComponentType type = ComponentType::Resistor;
    Waveform waveform = Waveform::DC;
    const char* label = "";
    const char* description = "";
};

// Pulse width that matches the selected duty cycle, and an edge time that is
// fast but not so steep that the simulator has to crawl through every edge.
double pulseWidthFor(double period, double duty)
{
    if (!(period > 0.0)) {
        period = 1e-3;
    }
    return period * std::clamp(duty, 0.005, 0.995);
}

double pulseRiseFor(double period)
{
    if (!(period > 0.0)) {
        period = 1e-3;
    }
    return std::clamp(period * 2e-3, 5e-9, 1e-6);
}

bool accentButton(const char* label, const ImVec2& size)
{
    ImGui::PushStyleColor(ImGuiCol_Button, Theme::toVec4(Theme::accent()));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(0.36f, 0.60f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          ImVec4(0.44f, 0.68f, 1.0f, 1.0f));
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return clicked;
}

bool dangerButton(const char* label, const ImVec2& size)
{
    ImGui::PushStyleColor(ImGuiCol_Button, Theme::toVec4(Theme::danger()));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(0.92f, 0.46f, 0.46f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          ImVec4(0.98f, 0.55f, 0.55f, 1.0f));
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return clicked;
}

float easeOut(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - std::pow(1.0f - t, 3.0f);
}

std::string clipText(const std::string& text, float width)
{
    if (width <= 0.0f || text.empty()) {
        return {};
    }
    if (ImGui::CalcTextSize(text.c_str()).x <= width) {
        return text;
    }
    std::string result;
    size_t index = 0;
    while (index < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[index]);
        size_t length = 1;
        if ((c & 0xF8) == 0xF0) {
            length = 4;
        } else if ((c & 0xF0) == 0xE0) {
            length = 3;
        } else if ((c & 0xE0) == 0xC0) {
            length = 2;
        }
        length = std::min(length, text.size() - index);
        const std::string candidate = result + text.substr(index, length) + "…";
        if (ImGui::CalcTextSize(candidate.c_str()).x > width) {
            break;
        }
        result.append(text, index, length);
        index += length;
    }
    result += "…";
    return result;
}

void drawChevron(ImDrawList* drawList, ImVec2 center, float size, ImU32 color)
{
    drawList->AddLine(ImVec2(center.x - size * 0.3f, center.y - size * 0.5f),
                      ImVec2(center.x + size * 0.2f, center.y), color, 2.0f);
    drawList->AddLine(ImVec2(center.x + size * 0.2f, center.y),
                      ImVec2(center.x - size * 0.3f, center.y + size * 0.5f),
                      color, 2.0f);
}

}  // namespace

void App::drawBottomBar(float height)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::toVec4(Theme::panel()));
    ImGui::BeginChild("##bottombar", ImVec2(0.0f, height), 0,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();

    const float pad = dp(10.0f);
    const float buttonHeight = dp(42.0f);
    const float y = (height - buttonHeight) * 0.5f;

    ImGui::SetCursorPos(ImVec2(pad, y));
    ImGui::BeginDisabled(!engineError_.empty());
    if (live_) {
        if (dangerButton("暂停仿真", ImVec2(dp(124.0f), buttonHeight))) {
            pauseLive();
        }
    } else {
        if (accentButton("启动仿真", ImVec2(dp(124.0f), buttonHeight))) {
            startLive();
        }
    }
    ImGui::EndDisabled();

    const float fabSize = dp(52.0f);
    const float resultsWidth = dp(64.0f);
    const float resultsHeight = dp(38.0f);
    const float rightX = ImGui::GetWindowWidth() - pad - fabSize - dp(10.0f) - resultsWidth;

    std::string message;
    ImU32 messageColor = Theme::textDim();
    if (!engineError_.empty()) {
        message = "引擎错误";
        messageColor = Theme::danger();
    } else if (!live_) {
        if (!status_.empty() && status_ != "就绪" && status_ != "已暂停" &&
            status_ != "已撤销" && status_ != "已重做") {
            message = status_;
        } else {
            message = hasResult_ ? "已暂停" : "点击启动仿真，实时显示电流";
        }
    } else if (simRunning_) {
        message = "仿真中…";
        messageColor = Theme::warning();
    } else if (!compiled_.ok) {
        message = "电路有错误";
        messageColor = Theme::danger();
    } else if (hasResult_ && resultRevision_ != circuit_.revision) {
        message = "等待重新仿真…";
        messageColor = Theme::warning();
    } else if (hasResult_ && !result_.ok) {
        message = "仿真失败";
        messageColor = Theme::danger();
    } else if (hasResult_ && result_.ok) {
        message = "运行中";
        if (result_.kind == AnalysisKind::Transient) {
            message += " · t = " + formatSI(liveSampleTime(), "s");
        }
        messageColor = Theme::good();
        if (selectedComponent_ >= 0) {
            if (const Component* component = circuit_.findComponent(selectedComponent_)) {
                double current = 0.0;
                if (currentFor(*component, liveSampleIndex(), &current)) {
                    message += " · " + component->name + " ";
                    message += std::fabs(current) < 1e-9
                                   ? "0 A"
                                   : formatSI(current, "A");
                }
            }
        }
    } else {
        message = "等待仿真结果…";
        messageColor = Theme::warning();
    }

    const float statusX = pad + dp(124.0f) + dp(14.0f);
    const float statusWidth = std::max(0.0f, rightX - statusX - dp(10.0f));
    ImGui::SetCursorPos(ImVec2(statusX, y + dp(9.0f)));
    const ImVec2 clipMin = ImGui::GetCursorScreenPos();
    ImGui::PushClipRect(clipMin,
                        ImVec2(ImGui::GetWindowPos().x + statusX + statusWidth,
                               ImGui::GetWindowPos().y + height),
                        true);
    ImGui::TextColored(Theme::toVec4(messageColor), "%s",
                       clipText(message, statusWidth).c_str());
    ImGui::PopClipRect();

    ImGui::SetCursorPos(ImVec2(rightX, (height - resultsHeight) * 0.5f));
    if (resultsOpen_) {
        if (accentButton("结果", ImVec2(resultsWidth, resultsHeight))) {
            resultsOpen_ = false;
        }
    } else {
        if (ImGui::Button("结果", ImVec2(resultsWidth, resultsHeight))) {
            resultsOpen_ = true;
        }
    }
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - pad - fabSize,
                               (height - fabSize) * 0.5f));
    drawAddButton();

    const ImVec2 windowPos = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(windowPos.x, windowPos.y + 1.0f),
        ImVec2(windowPos.x + ImGui::GetWindowWidth(), windowPos.y + 1.0f),
        Theme::stroke(), 1.0f);
    ImGui::EndChild();
}

void App::drawAddButton()
{
    const float size = dp(52.0f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##add_component", ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 center(pos.x + size * 0.5f, pos.y + size * 0.5f);
    addButtonCenter_ = center;
    addButtonRadius_ = size * 0.5f;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (hovered) {
        drawList->AddCircleFilled(center, size * 0.5f + dp(4.0f),
                                  Theme::accentSoft(), 40);
    }
    drawList->AddCircleFilled(center, size * 0.5f, Theme::accent(), 40);
    const float arm = dp(9.0f);
    drawList->AddLine(ImVec2(center.x - arm, center.y),
                      ImVec2(center.x + arm, center.y), Theme::text(),
                      dp(2.4f));
    drawList->AddLine(ImVec2(center.x, center.y - arm),
                      ImVec2(center.x, center.y + arm), Theme::text(),
                      dp(2.4f));
    if (ImGui::IsItemClicked()) {
        openSheet();
    }
}

void App::drawEditorOverlays()
{
    drawFloatingTools();
    drawZoomControls();
    drawPlacementChip();
}

void App::drawFloatingTools()
{
    if (sheetOpen() || placement_.active || canvasSize_.x < dp(120.0f) ||
        canvasSize_.y < dp(80.0f)) {
        return;
    }
    ImGui::SetNextWindowPos(ImVec2(canvasOrigin_.x + dp(12.0f),
                                   canvasOrigin_.y + dp(12.0f)));
    ImGui::SetNextWindowBgAlpha(0.95f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::toVec4(Theme::panelAlt()));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, dp(14.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(dp(6.0f), dp(6.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(dp(4.0f), dp(4.0f)));
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##floating_tools", nullptr, flags)) {
        const ImVec2 buttonSize(dp(66.0f), dp(38.0f));
        if (tool_ == Tool::Select) {
            if (accentButton("选择", buttonSize)) {
                setTool(Tool::Select);
            }
        } else {
            if (ImGui::Button("选择", buttonSize)) {
                setTool(Tool::Select);
            }
        }
        ImGui::SameLine(0.0f, dp(4.0f));
        if (tool_ == Tool::Wire) {
            if (accentButton("导线", buttonSize)) {
                setTool(Tool::Wire);
            }
        } else {
            if (ImGui::Button("导线", buttonSize)) {
                setTool(Tool::Wire);
                setStatus("点击两点绘制导线");
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

void App::drawZoomControls()
{
    if (sheetOpen() || resultsOpen_ || selectedComponent_ >= 0 || selectedWire_ >= 0 ||
        canvasSize_.x < dp(160.0f) || canvasSize_.y < dp(100.0f)) {
        return;
    }
    const float button = dp(38.0f);
    const float fitWidth = dp(56.0f);
    const float width = button * 2.0f + fitWidth + dp(12.0f);
    const float height = button + dp(10.0f);
    ImGui::SetNextWindowPos(ImVec2(canvasOrigin_.x + canvasSize_.x - width - dp(12.0f),
                                   canvasOrigin_.y + canvasSize_.y - height - dp(12.0f)));
    ImGui::SetNextWindowBgAlpha(0.95f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::toVec4(Theme::panelAlt()));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, dp(14.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(dp(5.0f), dp(5.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(dp(4.0f), dp(4.0f)));
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##zoom_controls", nullptr, flags)) {
        const auto zoomBy = [&](float factor) {
            if (canvasSize_.x < 10.0f) {
                return;
            }
            const ImVec2 center(canvasOrigin_.x + canvasSize_.x * 0.5f,
                                canvasOrigin_.y + canvasSize_.y * 0.5f);
            const ImVec2 before = screenToWorld(center);
            zoom_ = std::clamp(zoom_ * factor, 0.3f, 3.5f);
            viewOffset_ = ImVec2(center.x - canvasOrigin_.x - before.x * gridPixels(),
                                 center.y - canvasOrigin_.y - before.y * gridPixels());
        };
        if (ImGui::Button("−", ImVec2(button, button))) {
            zoomBy(1.0f / 1.2f);
        }
        ImGui::SameLine(0.0f, dp(4.0f));
        if (ImGui::Button("+", ImVec2(button, button))) {
            zoomBy(1.2f);
        }
        ImGui::SameLine(0.0f, dp(4.0f));
        if (ImGui::Button("适应", ImVec2(fitWidth, button))) {
            fitPending_ = true;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

void App::drawPlacementChip()
{
    if (!placement_.active || sheetOpen()) {
        return;
    }
    const float width = std::min(dp(330.0f), std::max(dp(190.0f), canvasSize_.x - dp(24.0f)));
    if (canvasSize_.x < dp(120.0f)) {
        return;
    }
    const float x = canvasOrigin_.x + (canvasSize_.x - width) * 0.5f;
    const float y = canvasOrigin_.y + dp(12.0f);
    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(width, dp(54.0f)));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::toVec4(Theme::panelAlt()));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, dp(16.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(dp(12.0f), dp(7.0f)));
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar;
    if (ImGui::Begin("##placement_chip", nullptr, flags)) {
        const float buttonWidth = dp(66.0f);
        const float textWidth = width - dp(24.0f) - buttonWidth - dp(8.0f);
        ImGui::SetCursorPos(ImVec2(dp(4.0f), dp(8.0f)));
        const std::string text =
            std::string("正在放置：") + componentInfo(placement_.type).label;
        ImGui::TextUnformatted(clipText(text, textWidth).c_str());
        ImGui::SetCursorPos(ImVec2(width - buttonWidth - dp(10.0f), dp(8.0f)));
        if (accentButton("完成", ImVec2(buttonWidth, dp(34.0f)))) {
            endPlacement();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void App::drawComponentSheet()
{
    if (!sheetOpen() && sheetAnim_ <= 0.001f) {
        sheetAnim_ = 0.0f;
        return;
    }
    const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.05f);
    const float target = sheetOpen() ? 1.0f : 0.0f;
    const float speed = sheetOpen() ? 7.0f : 9.0f;
    sheetAnim_ += (target - sheetAnim_) * std::min(1.0f, dt * speed);
    if (sheetOpen() && sheetAnim_ < 0.02f) {
        sheetAnim_ = 0.02f;
    }
    if (!sheetOpen() && sheetAnim_ < 0.005f) {
        sheetAnim_ = 0.0f;
        return;
    }
    const float anim = std::clamp(sheetAnim_, 0.0f, 1.0f);
    const float eased = easeOut(anim);

    // Dim scrim. It is created before the sheet so the sheet stays on top.
    ImGui::SetNextWindowPos(rootMin_);
    ImGui::SetNextWindowSize(rootSize_);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.48f * anim));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const ImGuiWindowFlags scrimFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::Begin("##sheet_scrim", nullptr, scrimFlags)) {
        ImGui::SetCursorScreenPos(rootMin_);
        ImGui::InvisibleButton("##sheet_scrim_hit", rootSize_);
        if (ImGui::IsItemClicked()) {
            closeSheet();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    const float sheetWidth =
        portrait_ ? rootSize_.x : std::min(dp(640.0f), rootSize_.x - dp(24.0f));
    const float sheetHeight =
        portrait_ ? std::min(rootSize_.y * 0.72f, dp(620.0f))
                  : std::min(rootSize_.y * 0.86f, dp(560.0f));
    const float x = rootMin_.x + (rootSize_.x - sheetWidth) * 0.5f;
    const float y = rootMin_.y + rootSize_.y - sheetHeight * eased;

    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(sheetWidth, sheetHeight));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::toVec4(Theme::panelAlt()));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, dp(20.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(dp(14.0f), dp(8.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(dp(8.0f), dp(6.0f)));
    const ImGuiWindowFlags sheetFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar;
    if (ImGui::Begin("##component_sheet", nullptr, sheetFlags)) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 windowPos = ImGui::GetWindowPos();
        const float handleY = windowPos.y + dp(7.0f);
        drawList->AddRectFilled(ImVec2(windowPos.x + sheetWidth * 0.5f - dp(22.0f), handleY),
                                ImVec2(windowPos.x + sheetWidth * 0.5f + dp(22.0f),
                                       handleY + dp(4.0f)),
                                Theme::stroke(), dp(2.0f));

        const float headerY = dp(18.0f);
        if (sheetLevel_ == SheetLevel::Components) {
            ImGui::SetCursorPos(ImVec2(dp(8.0f), headerY));
            if (ImGui::Button("‹", ImVec2(dp(40.0f), dp(36.0f)))) {
                sheetLevel_ = SheetLevel::Categories;
            }
            ImGui::SameLine(0.0f, dp(8.0f));
        } else {
            ImGui::SetCursorPos(ImVec2(dp(8.0f), headerY));
        }
        ImGui::AlignTextToFramePadding();
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.12f);
        ImGui::TextUnformatted(
            sheetLevel_ == SheetLevel::Components ? categoryLabel(sheetCategory_)
                                                  : "添加元件");
        ImGui::PopFont();

        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + dp(8.0f),
                                 ImGui::GetWindowWidth() - dp(54.0f)));
        if (ImGui::Button("✕", ImVec2(dp(40.0f), dp(36.0f)))) {
            closeSheet();
        }

        ImGui::SetCursorPos(ImVec2(dp(10.0f), dp(62.0f)));
        ImGui::BeginChild("##sheet_content", ImVec2(-1.0f, -1.0f), 0, 0);
        // Swiping the list scrolls it; only a real tap (press and release on the
        // same card without movement) picks a component.
        updateListDragScroll();
        if (sheetLevel_ == SheetLevel::Categories) {
            drawSheetCategories(ImGui::GetContentRegionAvail());
        } else if (sheetLevel_ == SheetLevel::Components) {
            drawSheetComponents(ImGui::GetContentRegionAvail());
        }
        finishListDragScroll();
        ImGui::EndChild();
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

void App::drawSheetCategories(const ImVec2& size)
{
    const ImVec2 start = ImGui::GetCursorScreenPos();
    float y = start.y;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (ComponentCategory category : componentCategories()) {
        const std::vector<ComponentType> types = componentsIn(category);
        if (types.empty()) {
            continue;
        }
        ImGui::PushID(static_cast<int>(category));
        const float cardHeight = dp(58.0f);
        ImGui::SetCursorScreenPos(ImVec2(start.x, y));
        ImGui::InvisibleButton("##category", ImVec2(size.x, cardHeight));
        const bool hovered = ImGui::IsItemHovered();
        // A tap counts on release over the same card, so a swipe that scrolls
        // the list never picks a category by accident.
        const bool activated = ImGui::IsItemDeactivated() && hovered;
        const ImVec2 cardMin = ImGui::GetItemRectMin();
        const ImVec2 cardMax = ImGui::GetItemRectMax();
        drawList->AddRectFilled(cardMin, cardMax,
                                hovered ? Theme::surfaceHigh() : Theme::surface(),
                                dp(13.0f));
        drawList->AddRect(cardMin, cardMax, Theme::stroke(), dp(13.0f), 0, 1.0f);

        const ImVec2 iconMin(cardMin.x + dp(10.0f), cardMin.y + dp(9.0f));
        const ImVec2 iconMax(iconMin.x + dp(40.0f), iconMin.y + dp(40.0f));
        drawList->AddRectFilled(iconMin, iconMax, Theme::accentSoft(), dp(11.0f));
        const ImVec2 iconCenter((iconMin.x + iconMax.x) * 0.5f,
                                (iconMin.y + iconMax.y) * 0.5f);
        for (int dot = 0; dot < 4; ++dot) {
            const float dx = (dot % 2) == 0 ? -dp(5.0f) : dp(5.0f);
            const float dy = dot < 2 ? -dp(5.0f) : dp(5.0f);
            drawList->AddCircleFilled(ImVec2(iconCenter.x + dx, iconCenter.y + dy),
                                      dp(2.2f), Theme::accent());
        }

        drawList->AddText(ImVec2(cardMin.x + dp(62.0f), cardMin.y + dp(9.0f)),
                          Theme::text(), categoryLabel(category));
        char count[32];
        std::snprintf(count, sizeof(count), "%d 种元件",
                      static_cast<int>(types.size()));
        drawList->AddText(ImVec2(cardMin.x + dp(62.0f), cardMin.y + dp(31.0f)),
                          Theme::muted(), count);
        drawChevron(drawList,
                    ImVec2(cardMax.x - dp(22.0f), (cardMin.y + cardMax.y) * 0.5f),
                    dp(16.0f), Theme::textDim());

        if (activated && !listDragging()) {
            sheetCategory_ = category;
            sheetLevel_ = SheetLevel::Components;
        }
        ImGui::PopID();
        y += cardHeight + dp(9.0f);
    }
    // ImGui 1.92 asserts if SetCursorPos extends the content boundary as the
    // last call in a window; submit a dummy item to grow it explicitly.
    ImGui::SetCursorScreenPos(ImVec2(start.x, y));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

void App::drawSheetComponents(const ImVec2& size)
{
    std::vector<PickerItem> items;
    for (ComponentType type : componentsIn(sheetCategory_)) {
        if (type == ComponentType::SignalSource) {
            items.push_back({type, Waveform::Sine, "正弦信号", "5 V · 1 kHz"});
            items.push_back({type, Waveform::Pulse, "方波信号", "5 V · 1 kHz"});
        } else if (type == ComponentType::AcSource) {
            items.push_back({type, Waveform::Sine, componentInfo(type).label,
                             componentInfo(type).description});
        } else if (type == ComponentType::VoltageSource) {
            items.push_back({type, Waveform::DC, componentInfo(type).label,
                             componentInfo(type).description});
        } else {
            items.push_back({type, Waveform::DC, componentInfo(type).label,
                             componentInfo(type).description});
        }
    }

    if (items.empty()) {
        ImGui::TextDisabled("这个分类暂时没有可用元件。");
        return;
    }

    const float gap = dp(10.0f);
    const int columns = size.x < dp(400.0f) ? 2 : 3;
    const float cardWidth = (size.x - gap * static_cast<float>(columns - 1)) /
                            static_cast<float>(columns);
    const float cardHeight = dp(108.0f);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    for (size_t index = 0; index < items.size(); ++index) {
        const int column = static_cast<int>(index) % columns;
        const int row = static_cast<int>(index) / columns;
        const ImVec2 pos(start.x + static_cast<float>(column) * (cardWidth + gap),
                         start.y + static_cast<float>(row) * (cardHeight + gap));
        ImGui::PushID(static_cast<int>(index));
        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton("##component", ImVec2(cardWidth, cardHeight));
        const bool hovered = ImGui::IsItemHovered();
        const bool activated = ImGui::IsItemDeactivated() && hovered;
        const ImVec2 cardMin = ImGui::GetItemRectMin();
        const ImVec2 cardMax = ImGui::GetItemRectMax();
        drawList->AddRectFilled(cardMin, cardMax,
                                hovered ? Theme::surfaceHigh() : Theme::surface(),
                                dp(14.0f));
        drawList->AddRect(cardMin, cardMax,
                          hovered ? Theme::accent() : Theme::stroke(), dp(14.0f), 0,
                          1.0f);

        const ImVec2 glyphCenter(cardMin.x + cardWidth * 0.5f,
                                 cardMin.y + dp(32.0f));
        drawComponentGlyph(drawList, glyphCenter, dp(42.0f), items[index].type,
                           hovered ? Theme::accent() : Theme::textDim());

        const std::string label = clipText(items[index].label, cardWidth - dp(12.0f));
        const ImVec2 labelSize = ImGui::CalcTextSize(label.c_str());
        drawList->AddText(ImVec2(cardMin.x + (cardWidth - labelSize.x) * 0.5f,
                                 cardMin.y + dp(61.0f)),
                          Theme::text(), label.c_str());

        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.86f);
        const std::string description =
            clipText(items[index].description, cardWidth - dp(12.0f));
        const ImVec2 descSize = ImGui::CalcTextSize(description.c_str());
        drawList->AddText(ImVec2(cardMin.x + (cardWidth - descSize.x) * 0.5f,
                                 cardMin.y + dp(83.0f)),
                          Theme::muted(), description.c_str());
        ImGui::PopFont();

        if (activated && !listDragging()) {
            beginPlacement(items[index].type, items[index].waveform);
        }
        ImGui::PopID();
    }

    const int rows = (static_cast<int>(items.size()) + columns - 1) / columns;
    ImGui::SetCursorScreenPos(
        ImVec2(start.x, start.y + static_cast<float>(rows) * (cardHeight + gap)));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

// ---------------------------------------------------------------------------
// Numeric parameters: sliders for coarse work, the keyboard for exact values
// ---------------------------------------------------------------------------

namespace {

struct NumberRange {
    double minimum;
    double maximum;
};

// Unit, range and title of a part's main value, shared by the slider row and
// the keyboard dialog so the two can never disagree.
NumberRange valueRange(ComponentType type)
{
    switch (type) {
    case ComponentType::Capacitor:
        return {1e-12, 1e-2};
    case ComponentType::Inductor:
    case ComponentType::PowerInductor:
        return {1e-9, 1.0};
    case ComponentType::ElectrolyticCap:
        return {1e-7, 1e-1};
    case ComponentType::Zener:
        return {2.4, 24.0};
    case ComponentType::Oscilloscope:
        return {1e6, 1e6};
    default:
        return {1.0, 1e7};
    }
}

const char* valueUnit(ComponentType type)
{
    switch (type) {
    case ComponentType::Capacitor:
    case ComponentType::ElectrolyticCap:
        return "F";
    case ComponentType::Inductor:
    case ComponentType::PowerInductor:
        return "H";
    case ComponentType::Zener:
        return "V";
    default:
        return "Ω";
    }
}

const char* valueTitle(ComponentType type)
{
    switch (type) {
    case ComponentType::Capacitor:
    case ComponentType::ElectrolyticCap:
        return "容值";
    case ComponentType::Inductor:
    case ComponentType::PowerInductor:
        return "电感值";
    case ComponentType::Zener:
        return "稳压值";
    default:
        return "电阻值";
    }
}

// SI prefixes that bracket the allowed range: a 1 Ω .. 10 MΩ field offers
// none / k / M, a 1 pF .. 10 mF field offers p / n / µ / m.
void prefixWindowFor(double minimum, double maximum, int* prefixMinimum,
                     int* prefixMaximum)
{
    const auto stepFor = [](double value, int fallback) {
        if (!(value > 0.0) || !std::isfinite(value)) {
            return fallback;
        }
        const int exponent =
            static_cast<int>(std::floor(std::log10(value) / 3.0) * 3.0);
        return std::clamp(exponent, -12, 9);
    };
    *prefixMinimum = stepFor(minimum, 0);
    *prefixMaximum = std::max(*prefixMinimum, stepFor(maximum, 0));
}

}  // namespace

bool App::numberFieldInfo(const Component* component, NumberField field,
                          NumberFieldInfo* info) const
{
    if (!info) {
        return false;
    }
    NumberFieldInfo result;

    const auto finish = [&](NumberFieldInfo value) {
        prefixWindowFor(value.minimum, value.maximum, &value.prefixMinimum,
                        &value.prefixMaximum);
        *info = value;
        return true;
    };
    const auto finishIntegral = [&](NumberFieldInfo value) {
        value.prefixMinimum = 0;
        value.prefixMaximum = 0;
        *info = value;
        return true;
    };

    // Analysis settings exist without a component.
    if (field == NumberField::TransientStep) {
        result.title = "瞬态步长";
        result.unit = "s";
        result.value = transient_.step;
        result.minimum = 100e-9;
        result.maximum = 1e-3;
        result.sliderFormat = "%.3g s";
        return finish(result);
    }
    if (field == NumberField::TransientStop) {
        result.title = "停止时间";
        result.unit = "s";
        result.value = transient_.stop;
        result.minimum = 100e-6;
        result.maximum = 50e-3;
        result.sliderFormat = "%.3g s";
        return finish(result);
    }
    if (!component) {
        return false;
    }

    switch (field) {
    case NumberField::Value: {
        const NumberRange range = valueRange(component->type);
        result.title = valueTitle(component->type);
        result.unit = valueUnit(component->type);
        result.value = component->value;
        result.minimum = range.minimum;
        result.maximum = range.maximum;
        // A slider over several decades reads better on a log scale; a narrow
        // range such as a zener voltage stays linear.
        result.logarithmic = range.minimum > 0.0 && range.maximum / range.minimum >= 1000.0;
        result.sliderFormat = "";
        return finish(result);
    }
    case NumberField::SeriesR:
        result.title = component->type == ComponentType::PowerInductor ? "绕组电阻 DCR"
                                                                      : "等效串联电阻 ESR";
        result.unit = "Ω";
        result.value = component->seriesR;
        result.minimum = 1e-3;
        result.maximum = 1e2;
        result.logarithmic = true;
        result.sliderFormat = "";
        return finish(result);
    case NumberField::RatedVoltage:
        result.title = "额定电压";
        result.unit = "V";
        result.value = component->ratedVoltage;
        result.minimum = 1.0;
        result.maximum = 48.0;
        result.sliderFormat = "%.1f V";
        return finishIntegral(result);
    case NumberField::MinVoltage:
        result.title = "启动电压";
        result.unit = "V";
        result.value = component->minVoltage;
        result.minimum = 0.5;
        result.maximum = std::max(1.0, component->ratedVoltage);
        result.sliderFormat = "%.1f V";
        return finishIntegral(result);
    case NumberField::MaxVoltage:
        result.title = "电压上限";
        result.unit = "V";
        result.value = component->maxVoltage;
        result.minimum = std::max(component->ratedVoltage * 0.5, 1.0);
        result.maximum = std::max(component->ratedVoltage * 2.0, 2.0);
        result.sliderFormat = "%.1f V";
        return finishIntegral(result);
    case NumberField::MaxCurrent:
        result.title = "电流上限";
        result.unit = "A";
        result.value = component->maxCurrent;
        result.minimum = 0.01;
        result.maximum = 20.0;
        result.logarithmic = true;
        result.sliderFormat = "";
        return finish(result);
    case NumberField::RatedCurrent:
        result.title = "额定电流";
        result.unit = "A";
        result.value = loadRatedCurrent(*component);
        result.minimum = 0.001;
        result.maximum = 50.0;
        result.sliderFormat = "%.2f A";
        return finishIntegral(result);
    case NumberField::RatedPower:
        result.title = "额定功率";
        result.unit = "W";
        result.value = loadRatedPower(*component);
        result.minimum = 0.01;
        result.maximum = 500.0;
        result.sliderFormat = "%.1f W";
        return finishIntegral(result);
    case NumberField::HeatLoad:
        result.title = "冷面热负载";
        result.unit = "W";
        result.value = component->heatLoad;
        result.minimum = 0.0;
        result.maximum = 100.0;
        result.sliderFormat = "%.1f W";
        return finishIntegral(result);
    case NumberField::ColdStart:
        result.title = "初始冷面温度";
        result.unit = "°C";
        result.value = component->coldStart;
        result.minimum = -40.0;
        result.maximum = 60.0;
        result.sliderFormat = "%.1f °C";
        return finishIntegral(result);
    case NumberField::Amplitude:
        result.title = component->source.mode == Waveform::DC ? "电压" : "幅值";
        result.unit = "V";
        result.value = component->source.amplitude;
        result.minimum = 0.0;
        result.maximum = 20.0;
        result.sliderFormat = "%.2f V";
        return finishIntegral(result);
    case NumberField::Frequency:
        result.title = "频率";
        result.unit = "Hz";
        result.value = component->source.frequency;
        result.minimum = 1.0;
        result.maximum = 1e6;
        result.logarithmic = true;
        result.sliderFormat = "";
        return finish(result);
    case NumberField::Duty:
        result.title = "占空比";
        result.unit = "%";
        result.value = std::clamp(component->source.duty * 100.0, 1.0, 99.0);
        result.minimum = 1.0;
        result.maximum = 99.0;
        result.sliderFormat = "%.1f %%";
        return finishIntegral(result);
    case NumberField::None:
    case NumberField::TransientStep:
    case NumberField::TransientStop:
        // Handled before the component lookup: they belong to the analysis.
        break;
    }
    return false;
}

void App::applyNumberField(int componentId, NumberField field, double value)
{
    switch (field) {
    case NumberField::TransientStep:
        transient_.step = value;
        if (live_) {
            scheduleLiveRerun(200);
        }
        return;
    case NumberField::TransientStop:
        transient_.stop = value;
        if (live_) {
            scheduleLiveRerun(200);
        }
        return;
    default:
        break;
    }

    Component* component = circuit_.findComponent(componentId);
    if (!component) {
        return;
    }
    NumberFieldInfo info;
    if (!numberFieldInfo(component, field, &info)) {
        return;
    }
    value = std::clamp(value, info.minimum, info.maximum);

    switch (field) {
    case NumberField::Value:
        if (isLoadType(component->type) && !isThermoelectricType(component->type)) {
            // The rated current/power follow the resistance, so the card can
            // never show a spec that contradicts the part.
            setLoadResistance(*component, value);
        } else {
            component->value = value;
        }
        break;
    case NumberField::SeriesR:
        component->seriesR = value;
        break;
    case NumberField::RatedVoltage: {
        // Keep the start-up / maximum limits in proportion to the rating.
        const double previous = std::max(0.1, component->ratedVoltage);
        const double ratio = value / previous;
        component->ratedVoltage = value;
        component->minVoltage *= ratio;
        component->maxVoltage *= ratio;
        applyLoadDefaults(*component);
        break;
    }
    case NumberField::RatedCurrent:
        setLoadRatedCurrent(*component, value);
        break;
    case NumberField::RatedPower:
        setLoadRatedPower(*component, value);
        break;
    case NumberField::HeatLoad:
        component->heatLoad = std::max(0.0, value);
        break;
    case NumberField::ColdStart:
        component->coldStart = value;
        break;
    case NumberField::MinVoltage:
        component->minVoltage = value;
        break;
    case NumberField::MaxVoltage:
        component->maxVoltage = value;
        component->maxCurrent = std::max(component->maxCurrent, 0.0);
        break;
    case NumberField::MaxCurrent:
        component->maxCurrent = value;
        break;
    case NumberField::Amplitude:
        component->source.amplitude = value;
        if (component->source.mode == Waveform::DC) {
            component->source.dc = value;
        }
        component->value = value;
        break;
    case NumberField::Frequency:
        component->source.frequency = std::max(1e-6, value);
        component->source.period = 1.0 / component->source.frequency;
        component->source.pulseWidth =
            pulseWidthFor(component->source.period, component->source.duty);
        component->source.rise = pulseRiseFor(component->source.period);
        break;
    case NumberField::Duty:
        component->source.duty = std::clamp(value / 100.0, 0.01, 0.99);
        component->source.pulseWidth =
            pulseWidthFor(component->source.period, component->source.duty);
        break;
    default:
        return;
    }

    circuit_.touch();
    markChanged();
}

bool App::drawFieldSlider(int componentId, NumberField field, float width)
{
    NumberFieldInfo info;
    const Component* component =
        componentId >= 0 ? circuit_.findComponent(componentId) : nullptr;
    if (!numberFieldInfo(component, field, &info)) {
        return false;
    }

    ImGui::PushID(static_cast<int>(field));
    ImGui::SetNextItemWidth(width);
    float value = static_cast<float>(std::clamp(info.value, info.minimum, info.maximum));
    bool changed = false;
    if (info.logarithmic) {
        float logValue = static_cast<float>(
            std::log10(std::max(info.minimum, static_cast<double>(value))));
        changed = ImGui::SliderFloat("##value", &logValue,
                                     static_cast<float>(std::log10(info.minimum)),
                                     static_cast<float>(std::log10(info.maximum)), "");
        if (changed) {
            value = static_cast<float>(std::pow(10.0, static_cast<double>(logValue)));
        }
    } else {
        changed = ImGui::SliderFloat("##value", &value, static_cast<float>(info.minimum),
                                     static_cast<float>(info.maximum),
                                     info.sliderFormat);
    }
    if (ImGui::IsItemActivated()) {
        pushUndo();
    }
    ImGui::PopID();

    if (changed) {
        applyNumberField(componentId, field, value);
    }
    return changed;
}

bool App::drawValueChip(const char* id, int componentId, NumberField field,
                        const std::string& text, const ImVec2& size)
{
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Button, Theme::toVec4(Theme::panelAlt()));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::toVec4(Theme::surface()));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::toVec4(Theme::accentSoft()));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.30f, 0.55f, 1.0f, 0.55f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, dp(1.0f));
    const bool clicked =
        ImGui::Button(clipText(text, size.x - dp(12.0f)).c_str(), size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    ImGui::PopID();
    if (clicked) {
        openNumberEditor(componentId, field);
    }
    return clicked;
}

void App::openNumberEditor(int componentId, NumberField field)
{
    const Component* component =
        componentId >= 0 ? circuit_.findComponent(componentId) : nullptr;
    NumberFieldInfo info;
    if (!numberFieldInfo(component, field, &info)) {
        return;
    }

    numberEditor_.open = true;
    numberEditor_.needsOpen = true;
    numberEditor_.requestFocus = true;
    numberEditor_.selectAll = true;
    numberEditor_.componentId = componentId;
    numberEditor_.field = field;
    numberEditor_.title = info.title;
    numberEditor_.unit = info.unit;
    numberEditor_.minimum = info.minimum;
    numberEditor_.maximum = info.maximum;
    numberEditor_.error.clear();
    // Open the field the way an engineer writes the value: "4.7" with the kilo
    // prefix selected, not "4700".
    const SiValue split = splitSI(info.value, info.prefixMinimum, info.prefixMaximum);
    numberEditor_.prefixExponent = split.exponent;
    std::snprintf(numberEditor_.buffer, sizeof(numberEditor_.buffer), "%s",
                  formatEditable(split.scaled).c_str());
}

bool App::wantsNumericKeyboard() const
{
    return numberEditor_.open;
}

void App::drawNumberEditor()
{
    if (!numberEditor_.open) {
        return;
    }
    Component* component = numberEditor_.componentId >= 0
                               ? circuit_.findComponent(numberEditor_.componentId)
                               : nullptr;
    NumberFieldInfo info;
    if (!numberFieldInfo(component, numberEditor_.field, &info)) {
        // The part was deleted, or undo replaced the circuit, while typing.
        numberEditor_.open = false;
        return;
    }
    if (numberEditor_.needsOpen) {
        numberEditor_.needsOpen = false;
        ImGui::OpenPopup("##number_editor");
    }

    const float width = std::clamp(rootSize_.x - dp(28.0f), dp(280.0f), dp(560.0f));
    const float contentWidth = width - dp(28.0f);
    const bool hasPrefixes = (info.prefixMaximum - info.prefixMinimum) >= 3;
    // A fixed, compact height: with the number pad up the window is roughly
    // halved, so the dialog must never need to scroll or shrink.
    const float rowTitle = dp(34.0f);
    const float rowInput = dp(48.0f);
    const float rowHint = dp(24.0f);
    const float rowPrefix = hasPrefixes ? dp(40.0f) : 0.0f;
    // The size below is the window size, so the vertical padding (12 dp top and
    // bottom) is part of it.
    const float height = dp(24.0f) + rowTitle + rowInput + rowHint + rowPrefix;

    // Anchored to the top: Android shrinks the window when the soft keyboard
    // appears, so a bottom-anchored dialog would be buried under the keys.
    ImGui::SetNextWindowPos(
        ImVec2(rootMin_.x + (rootSize_.x - width) * 0.5f, safeTop_ + dp(10.0f)),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::toVec4(Theme::panelAlt()));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(dp(14.0f), dp(12.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(dp(8.0f), dp(6.0f)));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                   ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse;
    if (!ImGui::BeginPopupModal("##number_editor", nullptr, flags)) {
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        // Dismissed with Escape or by the platform: forget the pending edit.
        numberEditor_.open = false;
        return;
    }

    // Live interpretation of what has been typed so far.
    const ParsedNumber parsed = parseEngineering(numberEditor_.buffer);
    const double prefixScale =
        std::pow(10.0, static_cast<double>(numberEditor_.prefixExponent));
    const double preview =
        parsed.ok ? (parsed.hasPrefix ? parsed.value : parsed.value * prefixScale)
                  : info.minimum;

    // 确定 / 取消 live in the title row: they stay reachable even if the soft
    // keyboard leaves very little height.
    const auto close = [&]() {
        numberEditor_.open = false;
        ImGui::CloseCurrentPopup();
    };
    const auto commit = [&]() {
        const ParsedNumber typed = parseEngineering(numberEditor_.buffer);
        if (!typed.ok) {
            numberEditor_.error = "无法识别这个数值：请输入 4700、4.7k 或 100n";
            numberEditor_.requestFocus = true;
            numberEditor_.selectAll = true;
            return;
        }
        const double value = typed.hasPrefix ? typed.value : typed.value * prefixScale;
        const double tolerance = std::fabs(info.maximum) * 1e-9 + 1e-12;
        if (value < info.minimum - tolerance || value > info.maximum + tolerance) {
            numberEditor_.error = "超出范围：" + formatSI(info.minimum, info.unit) +
                                  " ~ " + formatSI(info.maximum, info.unit);
            numberEditor_.requestFocus = true;
            numberEditor_.selectAll = true;
            return;
        }
        // Keep the transient inside the engine's sample budget.
        const double points =
            numberEditor_.field == NumberField::TransientStep
                ? transient_.stop / std::max(1e-12, value)
                : (numberEditor_.field == NumberField::TransientStop
                       ? value / std::max(1e-12, transient_.step)
                       : 0.0);
        if (points > 200000.0) {
            numberEditor_.error = "点数超过 200000，请放大步长或缩短时间";
            return;
        }
        if (numberEditor_.componentId >= 0) {
            pushUndo();
        }
        applyNumberField(numberEditor_.componentId, numberEditor_.field, value);
        close();
    };

    const float cancelWidth = dp(76.0f);
    const float commitWidth = dp(92.0f);
    ImGui::SetCursorPos(ImVec2(contentWidth - cancelWidth - dp(6.0f) - commitWidth,
                               dp(1.0f)));
    if (accentButton("确定", ImVec2(commitWidth, dp(32.0f)))) {
        commit();
    }
    ImGui::SameLine(0.0f, dp(6.0f));
    if (ImGui::Button("取消", ImVec2(cancelWidth, dp(32.0f)))) {
        close();
    }

    ImGui::SetCursorPos(ImVec2(0.0f, dp(2.0f)));
    const float titleRoom =
        std::max(dp(48.0f), contentWidth - cancelWidth - commitWidth - dp(22.0f));
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.1f);
    ImGui::TextUnformatted(clipText(numberEditor_.title, titleRoom * 0.5f).c_str());
    ImGui::PopFont();
    ImGui::SameLine(0.0f, dp(10.0f));
    const std::string subject =
        component ? component->name + " · " + componentInfo(component->type).label
                  : std::string("分析设置");
    ImGui::TextDisabled("%s", clipText(subject, titleRoom * 0.5f).c_str());

    // The number itself: a plain text field, focused when the dialog opens so
    // the keyboard appears immediately. AutoSelectAll means typing replaces the
    // value that is already there.
    ImGui::SetCursorPos(ImVec2(0.0f, rowTitle));
    const float unitWidth = ImGui::CalcTextSize(numberEditor_.unit.c_str()).x;
    ImGui::SetNextItemWidth(contentWidth - unitWidth - dp(12.0f));
    if (numberEditor_.requestFocus) {
        numberEditor_.requestFocus = false;
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2f);
    ImGuiInputTextFlags inputFlags = ImGuiInputTextFlags_EnterReturnsTrue;
    if (numberEditor_.selectAll) {
        // Typing replaces the value that is already there, but only the first
        // time: coming back from a prefix button keeps the text intact.
        inputFlags |= ImGuiInputTextFlags_AutoSelectAll;
    }
    const bool entered =
        ImGui::InputText("##number_value", numberEditor_.buffer,
                         sizeof(numberEditor_.buffer), inputFlags);
    const bool edited = ImGui::IsItemEdited();
    if (ImGui::IsItemActive()) {
        numberEditor_.selectAll = false;
    }
    ImGui::PopFont();
    ImGui::SameLine(0.0f, dp(8.0f));
    ImGui::TextUnformatted(numberEditor_.unit.c_str());
    if (edited) {
        numberEditor_.error.clear();
    }

    // One status line: the interpretation of the text, or the reason it cannot
    // be used yet.
    ImGui::SetCursorPos(ImVec2(0.0f, rowTitle + rowInput));
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.92f);
    if (!numberEditor_.error.empty()) {
        ImGui::TextColored(Theme::toVec4(Theme::danger()), "%s",
                           numberEditor_.error.c_str());
    } else if (!parsed.ok) {
        ImGui::TextColored(Theme::toVec4(Theme::warning()),
                           "请输入数值，例如 4700、4.7k、100n、1e-3");
    } else {
        ImGui::Text("= %s", formatSI(preview, info.unit).c_str());
        ImGui::SameLine(0.0f, dp(10.0f));
        ImGui::TextDisabled("范围 %s ~ %s", formatSI(info.minimum, info.unit).c_str(),
                            formatSI(info.maximum, info.unit).c_str());
    }
    ImGui::PopFont();

    // SI prefix buttons: on a phone this is how "4.7 kΩ" is entered without
    // hunting for the letter k on an alphabet keyboard. A prefix typed inside
    // the field wins over the selected button (see parseEngineering()).
    if (hasPrefixes) {
        const int prefixCount = (info.prefixMaximum - info.prefixMinimum) / 3 + 1;
        const float chipWidth =
            std::min(dp(58.0f), (contentWidth - dp(6.0f) * static_cast<float>(prefixCount - 1)) /
                                    static_cast<float>(prefixCount));
        ImGui::SetCursorPos(ImVec2(0.0f, rowTitle + rowInput + rowHint));
        for (int exponent = info.prefixMinimum; exponent <= info.prefixMaximum;
             exponent += 3) {
            const std::string prefix = siPrefix(exponent);
            const std::string label = prefix.empty() ? std::string("—") : prefix;
            const ImVec2 size(chipWidth, dp(34.0f));
            ImGui::PushID(exponent);
            const bool clicked = exponent == numberEditor_.prefixExponent
                                     ? accentButton(label.c_str(), size)
                                     : ImGui::Button(label.c_str(), size);
            ImGui::PopID();
            if (clicked) {
                numberEditor_.prefixExponent = exponent;
                numberEditor_.error.clear();
                // Tapping the button took the focus away from the field, which
                // also hides the keyboard: take it back so typing can continue.
                numberEditor_.requestFocus = true;
            }
            if (exponent + 3 <= info.prefixMaximum) {
                ImGui::SameLine(0.0f, dp(6.0f));
            }
        }
    }

    if (entered) {
        commit();
    }

    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void App::drawSelectionCard()
{
    if (resultsOpen_ || sheetOpen() || numberEditor_.open) {
        return;
    }
    Component* component =
        selectedWire_ < 0 ? circuit_.findComponent(selectedComponent_) : nullptr;
    if (selectedWire_ < 0 && !component) {
        return;
    }

    const bool wire = selectedWire_ >= 0;
    float height = dp(64.0f);
    if (wire) {
        height = dp(58.0f);
    } else {
        switch (component->type) {
        case ComponentType::Resistor:
        case ComponentType::Capacitor:
        case ComponentType::Inductor:
            height = dp(108.0f);
            break;
        case ComponentType::PowerInductor:
        case ComponentType::ElectrolyticCap:
            height = dp(150.0f);
            break;
        case ComponentType::PwmSource:
            height = dp(204.0f);
            break;
        case ComponentType::Oscilloscope:
            height = dp(160.0f);
            break;
        case ComponentType::TerminalBlock:
            height = dp(96.0f);
            break;
        case ComponentType::Tec12706:
        case ComponentType::Tec12710:
            // Spec line, temperature, electrical readout, the two model
            // parameters and the cooldown curve.
            height = dp(248.0f);
            break;
        case ComponentType::Bulb:
        case ComponentType::Fan: {
            // Two parameters per row on a wide card, one per row on a phone
            // held upright, so every slider and value chip stays finger sized.
            const bool wide = (rootSize_.x - dp(16.0f)) >= dp(560.0f);
            height = dp(wide ? 240.0f : 300.0f);
            break;
        }
        case ComponentType::VoltageSource:
        case ComponentType::AcSource:
        case ComponentType::SignalSource:
            // A pulse source also shows the duty-cycle row; without the extra
            // height that row would be clipped and untouchable.
            height = component->source.mode == Waveform::Pulse ? dp(204.0f)
                                                               : dp(156.0f);
            break;
        case ComponentType::Switch:
            height = dp(98.0f);
            break;
        case ComponentType::Ground:
            height = dp(78.0f);
            break;
        case ComponentType::Diode:
        case ComponentType::Led:
        case ComponentType::Zener:
        case ComponentType::SchottkyDiode:
            height = dp(146.0f);
            break;
        case ComponentType::Npn:
        case ComponentType::Pnp:
        case ComponentType::Nmos:
        case ComponentType::PowerNmos:
        case ComponentType::PowerPmos:
            height = dp(144.0f);
            break;
        case ComponentType::OpAmp:
        case ComponentType::Timer555:
        case ComponentType::GateAnd:
        case ComponentType::GateOr:
        case ComponentType::GateNot:
        case ComponentType::Regulator7805:
            height = dp(164.0f);
            break;
        }
        if (portrait_) {
            height += dp(14.0f);
        }
    }

    const float width = rootSize_.x - dp(16.0f);
    const float x = safeLeft_ + dp(8.0f);
    const float y = safeTop_ + rootSize_.y - bottomBarHeight_ - height - dp(6.0f);
    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::toVec4(Theme::panelAlt()));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, dp(18.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(dp(12.0f), dp(7.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(dp(7.0f), dp(6.0f)));
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar;
    if (!ImGui::Begin("##selection_card", nullptr, flags)) {
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor();
        return;
    }

    const float buttonWidth = dp(56.0f);
    const float buttonHeight = dp(34.0f);
    if (wire) {
        ImGui::SetCursorPos(ImVec2(dp(4.0f), dp(10.0f)));
        ImGui::TextUnformatted("导线");
        ImGui::SetCursorPos(ImVec2(width - dp(12.0f) - buttonWidth, dp(10.0f)));
        if (dangerButton("删除", ImVec2(buttonWidth, buttonHeight))) {
            pushUndo();
            circuit_.removeWire(selectedWire_);
            selectedWire_ = -1;
            markChanged();
            setStatus("已删除导线");
            ImGui::End();
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor();
            return;
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor();
        return;
    }

    const float right = width - dp(12.0f) - buttonWidth * 3.0f - dp(14.0f);

    ImGui::SetCursorPos(ImVec2(dp(2.0f), dp(9.0f)));
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.05f);
    ImGui::TextUnformatted(component->name.c_str());
    ImGui::PopFont();
    ImGui::SameLine(0.0f, dp(8.0f));
    // Clip the type label so it never runs under the rotate / mirror / delete
    // buttons.
    const float labelRoom = std::max(dp(20.0f), right - ImGui::GetCursorPosX() - dp(6.0f));
    ImGui::TextDisabled("%s",
                        clipText(componentInfo(component->type).label, labelRoom).c_str());

    ImGui::SetCursorPos(ImVec2(std::max(dp(2.0f), right), dp(9.0f)));
    if (ImGui::Button("旋转", ImVec2(buttonWidth, buttonHeight))) {
        pushUndo();
        circuit_.rotateComponent(component->id);
        markChanged();
    }
    ImGui::SameLine(0.0f, dp(7.0f));
    if (ImGui::Button("翻转", ImVec2(buttonWidth, buttonHeight))) {
        pushUndo();
        circuit_.mirrorComponent(component->id);
        markChanged();
    }
    ImGui::SameLine(0.0f, dp(7.0f));
    if (dangerButton("删除", ImVec2(buttonWidth, buttonHeight))) {
        pushUndo();
        circuit_.removeComponent(component->id);
        selectedComponent_ = -1;
        markChanged();
        setStatus("已删除元件");
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor();
        return;
    }

    const float contentY = dp(54.0f);
    const float chipHeight = dp(32.0f);
    const float stepButtonWidth = dp(46.0f);
    // The slider leaves room for the value chip and the two decade buttons, so
    // the row reads "drag | type | ÷10 | ×10".
    const auto rowSliderWidth = [&]() {
        return std::clamp(width - dp(300.0f), dp(120.0f), dp(340.0f));
    };

    // Main value of the part: drag the slider, or tap the chip and type the
    // exact number on the keyboard.
    const auto valueRow = [&](float rowY) {
        const NumberRange range = valueRange(component->type);
        ImGui::SetCursorPos(ImVec2(dp(2.0f), rowY));
        drawFieldSlider(component->id, NumberField::Value, rowSliderWidth());
        ImGui::SameLine(0.0f, dp(8.0f));
        drawValueChip("##value_chip", component->id, NumberField::Value,
                      formatSI(component->value, valueUnit(component->type)),
                      ImVec2(dp(88.0f), chipHeight));
        ImGui::SameLine(0.0f, dp(8.0f));
        if (ImGui::Button("÷10", ImVec2(stepButtonWidth, chipHeight))) {
            pushUndo();
            applyNumberField(component->id, NumberField::Value,
                             std::max(range.minimum, component->value / 10.0));
        }
        ImGui::SameLine(0.0f, dp(6.0f));
        if (ImGui::Button("×10", ImVec2(stepButtonWidth, chipHeight))) {
            pushUndo();
            applyNumberField(component->id, NumberField::Value,
                             std::min(range.maximum, component->value * 10.0));
        }
    };

    // Series resistance (winding DCR or capacitor ESR) of a real filter part.
    const auto seriesRow = [&](float rowY) {
        ImGui::SetCursorPos(ImVec2(dp(2.0f), rowY));
        drawFieldSlider(component->id, NumberField::SeriesR, rowSliderWidth());
        ImGui::SameLine(0.0f, dp(8.0f));
        drawValueChip("##seriesr_chip", component->id, NumberField::SeriesR,
                      formatSI(component->seriesR, "Ω"), ImVec2(dp(88.0f), chipHeight));
        ImGui::SameLine(0.0f, dp(8.0f));
        ImGui::TextDisabled(component->type == ComponentType::PowerInductor ? "内阻 DCR"
                                                                           : "内阻 ESR");
    };

    // One labelled parameter row: slider plus its exact-entry chip.
    const auto fieldRow = [&](NumberField field, const char* label, float x, float rowY,
                              float rowWidth) {
        NumberFieldInfo info;
        if (!numberFieldInfo(component, field, &info)) {
            return;
        }
        ImGui::SetCursorPos(ImVec2(x, rowY));
        ImGui::TextDisabled("%s", label);
        ImGui::SameLine(0.0f, dp(6.0f));
        const float labelWidth = ImGui::CalcTextSize(label).x;
        const float chipWidth = dp(88.0f);
        const float sliderWidth =
            std::clamp(rowWidth - labelWidth - chipWidth - dp(26.0f), dp(80.0f), dp(300.0f));
        drawFieldSlider(component->id, field, sliderWidth);
        ImGui::SameLine(0.0f, dp(6.0f));
        char chipId[32];
        std::snprintf(chipId, sizeof(chipId), "##chip%d", static_cast<int>(field));
        drawValueChip(chipId, component->id, field,
                      formatSI(info.value, info.unit), ImVec2(chipWidth, chipHeight));
    };

    if (component->type == ComponentType::Resistor) {
        valueRow(contentY);
    } else if (component->type == ComponentType::Capacitor) {
        valueRow(contentY);
    } else if (component->type == ComponentType::Inductor) {
        valueRow(contentY);
    } else if (component->type == ComponentType::PowerInductor) {
        valueRow(contentY);
        seriesRow(contentY + dp(40.0f));
    } else if (component->type == ComponentType::ElectrolyticCap) {
        valueRow(contentY);
        seriesRow(contentY + dp(40.0f));
    } else if (component->type == ComponentType::Oscilloscope) {
        ImGui::SetCursorPos(ImVec2(dp(2.0f), contentY));
        ImGui::TextDisabled("探头输入 1 MΩ，与地构成差分测量");
        const auto stats = probeStats_.find(component->id);
        if (stats == probeStats_.end() || !stats->second.ok) {
            ImGui::SetCursorPos(ImVec2(dp(2.0f), contentY + dp(34.0f)));
            ImGui::TextDisabled("启动仿真后显示平均值与纹波");
        } else {
            const WaveStats& value = stats->second;
            ImGui::SetCursorPos(ImVec2(dp(2.0f), contentY + dp(30.0f)));
            ImGui::Text("平均 %s   峰峰 %s", formatSI(value.mean, "V").c_str(),
                        formatSI(value.ripple, "V").c_str());
            ImGui::SetCursorPos(ImVec2(dp(2.0f), contentY + dp(56.0f)));
            ImGui::TextDisabled("最大 %s   最小 %s   有效值 %s",
                                formatSI(value.max, "V").c_str(),
                                formatSI(value.min, "V").c_str(),
                                formatSI(value.rms, "V").c_str());
            ImGui::SetCursorPos(ImVec2(dp(2.0f), contentY + dp(82.0f)));
            ImGui::TextDisabled(value.frequency > 0.0 ? "频率 %s" : "频率 —",
                                formatSI(value.frequency, "Hz").c_str());
        }
    } else if (component->type == ComponentType::TerminalBlock) {
        ImGui::SetCursorPos(ImVec2(dp(2.0f), contentY));
        ImGui::TextDisabled("两只螺丝互不相连，仅用来标记板级接口");
    } else if (isThermoelectricType(component->type)) {
        // Peltier cooler: a fixed datasheet module, so the card shows what the
        // simulation makes of it - cold face temperature, cooling power and
        // efficiency - plus the two model parameters of the thermal side.
        const ThermoelectricSpec* spec = thermoelectricSpec(component->type);
        const size_t sample = liveSampleIndex();
        const auto temperatureText = [](double celsius) {
            char text[32];
            std::snprintf(text, sizeof(text), "%.1f °C", celsius);
            return std::string(text);
        };
        const auto cold = result_.coldTemperature.find(component->id);
        const bool haveSimulation =
            live_ && hasResult_ && result_.ok && resultRevision_ == circuit_.revision &&
            cold != result_.coldTemperature.end() && !cold->second.empty();
        const auto sampleAt = [&](size_t index) {
            return cold->second[std::min(index, cold->second.size() - 1)];
        };

        ImGui::SetCursorPos(ImVec2(dp(2.0f), contentY));
        if (spec) {
            ImGui::TextDisabled("%s · 额定 %s / %s / %s", spec->model,
                                formatSI(spec->ratedVoltage, "V").c_str(),
                                formatSI(spec->ratedCurrent, "A").c_str(),
                                formatSI(spec->maxPumping, "W").c_str());
        }

        const float row1 = contentY + dp(25.0f);
        const float row2 = row1 + dp(31.0f);
        const float row3 = row2 + dp(27.0f);
        ImGui::SetCursorPos(ImVec2(dp(2.0f), row1));
        if (component->damaged) {
            ImGui::TextColored(Theme::toVec4(Theme::danger()),
                               component->damagedByCurrent ? "过流损坏" : "过压损坏");
            ImGui::SameLine(0.0f, dp(10.0f));
            if (accentButton("修复", ImVec2(dp(66.0f), dp(32.0f)))) {
                pushUndo();
                component->damaged = false;
                component->damagedByCurrent = false;
                circuit_.touch();
                markChanged();
            }
        } else if (haveSimulation) {
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.05f);
            ImGui::Text("冷面 %s", temperatureText(sampleAt(sample)).c_str());
            ImGui::PopFont();
            ImGui::SameLine(0.0f, dp(10.0f));
            ImGui::TextDisabled("稳定 %s · 温差 %s",
                                temperatureText(cold->second.back()).c_str(),
                                formatSI(thermoelectricHotSide() - cold->second.back(), "K")
                                    .c_str());
        } else {
            ImGui::TextDisabled("冷面 %s · 启动仿真后显示降温过程",
                                temperatureText(component->coldStart).c_str());
        }

        ImGui::SetCursorPos(ImVec2(dp(2.0f), row2));
        if (haveSimulation && spec) {
            double voltage = 0.0;
            double current = 0.0;
            if (voltageAcross(*component, sample, &voltage) &&
                currentFor(*component, sample, &current)) {
                ThermoelectricPoint point;
                thermoelectricFromMeasurement(*spec, voltage, current,
                                              sampleAt(sample), &point);
                ImGui::Text("电流 %s · 制冷 %s · 输入 %s · COP %.2f",
                            formatSI(current, "A").c_str(),
                            formatSI(point.heatPumped, "W").c_str(),
                            formatSI(point.inputPower, "W").c_str(), point.cop);
            } else {
                ImGui::TextDisabled("等待电流与电压读数");
            }
        } else if (spec) {
            ImGui::TextDisabled("上限 %s / %s / %s · 温差上限 %s",
                                formatSI(spec->maxVoltage, "V").c_str(),
                                formatSI(spec->maxCurrent, "A").c_str(),
                                formatSI(spec->maxPumping, "W").c_str(),
                                formatSI(spec->maxDeltaT, "K").c_str());
        }

        // Editable model parameters: where the plate starts and how much heat
        // leaks back into its cold face.
        ImGui::SetCursorPos(ImVec2(dp(2.0f), row3));
        ImGui::TextDisabled("初始");
        ImGui::SameLine(0.0f, dp(6.0f));
        drawValueChip("##coldstart_chip", component->id, NumberField::ColdStart,
                      temperatureText(component->coldStart), ImVec2(dp(84.0f), chipHeight));
        ImGui::SameLine(0.0f, dp(12.0f));
        ImGui::TextDisabled("热负载");
        ImGui::SameLine(0.0f, dp(6.0f));
        drawValueChip("##heatload_chip", component->id, NumberField::HeatLoad,
                      formatSI(component->heatLoad, "W"), ImVec2(dp(84.0f), chipHeight));
        ImGui::SameLine(0.0f, dp(10.0f));
        ImGui::TextDisabled("热面 %.0f °C", thermoelectricHotSide());

        // Cooldown curve of the cold face over the simulated window, with the
        // heat sink temperature as a reference line.
        if (haveSimulation && cold->second.size() > 3 && !result_.time.empty()) {
            const float curveHeight = dp(46.0f);
            const float row4 = std::max(row3 + dp(38.0f), height - curveHeight - dp(18.0f));
            ImGui::SetCursorPos(ImVec2(dp(2.0f), row4));
            const ImVec2 topLeft = ImGui::GetCursorScreenPos();
            const float curveWidth = std::max(dp(120.0f), width - dp(26.0f));
            ImGui::Dummy(ImVec2(curveWidth, curveHeight));
            const ImVec2 bottomRight(topLeft.x + curveWidth, topLeft.y + curveHeight);
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(topLeft, bottomRight, IM_COL32(0, 0, 0, 70), dp(8.0f));

            double low = cold->second[0];
            double high = cold->second[0];
            for (double value : cold->second) {
                low = std::min(low, value);
                high = std::max(high, value);
            }
            low = std::min(low, thermoelectricHotSide() - 1.0);
            high = std::max(high, thermoelectricHotSide() + 1.0);
            const double span = std::max(1e-6, high - low);
            const float inset = dp(5.0f);
            const auto plotPoint = [&](size_t i) {
                const float u = cold->second.size() > 1
                                    ? static_cast<float>(i) /
                                          static_cast<float>(cold->second.size() - 1)
                                    : 1.0f;
                const float v = static_cast<float>((cold->second[i] - low) / span);
                return ImVec2(topLeft.x + inset + u * (curveWidth - inset * 2.0f),
                              bottomRight.y - inset - v * (curveHeight - inset * 2.0f));
            };
            const float ambientV =
                static_cast<float>((thermoelectricHotSide() - low) / span);
            const float ambientY =
                bottomRight.y - inset - ambientV * (curveHeight - inset * 2.0f);
            drawList->AddLine(ImVec2(topLeft.x + inset, ambientY),
                              ImVec2(bottomRight.x - inset, ambientY),
                              IM_COL32(255, 255, 255, 40), 1.0f);
            std::vector<ImVec2> points;
            points.reserve(cold->second.size());
            for (size_t i = 0; i < cold->second.size(); ++i) {
                points.push_back(plotPoint(i));
            }
            // Cold blue for the cold face: the curve falls away from ambient.
            drawList->AddPolyline(points.data(), static_cast<int>(points.size()),
                                  IM_COL32(120, 200, 255, 255), 0, 2.0f * uiScale_);
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.8f);
            drawList->AddText(ImVec2(topLeft.x + dp(6.0f), topLeft.y + dp(1.0f)),
                              Theme::muted(), temperatureText(high).c_str());
            drawList->AddText(ImVec2(topLeft.x + dp(6.0f), bottomRight.y - dp(15.0f)),
                              Theme::muted(), temperatureText(low).c_str());
            ImGui::PopFont();
        }
    } else if (isLoadType(component->type)) {
        valueRow(contentY);

        // Realistic load ratings: nominal voltage, start-up voltage, maximum
        // voltage (e.g. a fan tolerates +10 %) and maximum current. Each one is
        // a slider plus its exact-entry chip; a narrow (portrait) card gives
        // every parameter its own row, a wide one puts two side by side.
        const bool fan = component->type == ComponentType::Fan;
        struct LoadParameter {
            NumberField field;
            const char* label;
        };
        static const LoadParameter kLoadParameters[] = {
            {NumberField::RatedVoltage, "额定"},
            {NumberField::MinVoltage, "启动"},
            {NumberField::MaxVoltage, "上限"},
            {NumberField::MaxCurrent, "电流上限"},
        };
        const bool twoColumns = width >= dp(560.0f);
        const float rowHeight = dp(40.0f);
        const float gridTop = contentY + dp(40.0f);
        const float columnWidth = (width - dp(24.0f)) * 0.5f;
        int index = 0;
        for (const LoadParameter& parameter : kLoadParameters) {
            const int column = twoColumns ? index % 2 : 0;
            const int line = twoColumns ? index / 2 : index;
            fieldRow(parameter.field, parameter.label,
                     dp(2.0f) + columnWidth * static_cast<float>(column),
                     gridTop + rowHeight * static_cast<float>(line),
                     twoColumns ? columnWidth : width - dp(24.0f));
            ++index;
        }
        const int gridLines = twoColumns ? 2 : 4;
        const float row3 = gridTop + rowHeight * static_cast<float>(gridLines) + dp(4.0f);

        // The nominal spec of the part: the voltage is the row above, and the
        // current and power are editable here. They are linked (P = V * I), so
        // typing either one re-derives the resistance.
        ImGui::SetCursorPos(ImVec2(dp(2.0f), row3));
        drawValueChip("##ratedcurrent_chip", component->id, NumberField::RatedCurrent,
                      formatSI(loadRatedCurrent(*component), "A"),
                      ImVec2(dp(78.0f), chipHeight));
        ImGui::SameLine(0.0f, dp(6.0f));
        drawValueChip("##ratedpower_chip", component->id, NumberField::RatedPower,
                      formatSI(loadRatedPower(*component), "W"),
                      ImVec2(dp(78.0f), chipHeight));
        ImGui::SameLine(0.0f, dp(10.0f));
        if (component->damaged) {
            ImGui::TextColored(Theme::toVec4(Theme::danger()),
                               component->damagedByCurrent ? "过流损坏" : "过压损坏");
            ImGui::SameLine(0.0f, dp(8.0f));
            if (accentButton("修复", ImVec2(dp(66.0f), dp(32.0f)))) {
                pushUndo();
                component->damaged = false;
                component->damagedByCurrent = false;
                circuit_.touch();
                markChanged();
            }
        } else if (live_) {
            double voltage = 0.0;
            if (voltageAcross(*component, liveSampleIndex(), &voltage)) {
                const double intensity = loadIntensity(*component, liveSampleIndex());
                const bool belowStart =
                    std::fabs(voltage) < component->minVoltage - 1e-9;
                if (belowStart) {
                    ImGui::TextColored(Theme::toVec4(Theme::warning()), "欠压未启动");
                } else {
                    ImGui::TextDisabled("%s %.0f%%（%s）",
                                        fan ? "转速" : "亮度", intensity * 100.0,
                                        formatSI(std::fabs(voltage), "V").c_str());
                }
            }
        }
    } else if (component->type == ComponentType::Switch) {
        ImGui::SetCursorPos(ImVec2(dp(2.0f), contentY));
        if (component->switchClosed) {
            if (accentButton("闭合", ImVec2(dp(84.0f), dp(36.0f)))) {
                pushUndo();
                component->switchClosed = false;
                circuit_.touch();
                markChanged();
            }
        } else {
            if (ImGui::Button("断开", ImVec2(dp(84.0f), dp(36.0f)))) {
                pushUndo();
                component->switchClosed = true;
                circuit_.touch();
                markChanged();
            }
        }
        ImGui::SameLine(0.0f, dp(8.0f));
        ImGui::TextDisabled("点击切换");
    } else if (isSourceType(component->type)) {
        const bool pwmGenerator = component->type == ComponentType::PwmSource;
        ImGui::SetCursorPos(ImVec2(dp(2.0f), contentY));
        int mode = static_cast<int>(component->source.mode);
        const char* modes[] = {"直流", "正弦", "方波"};
        ImGui::SetNextItemWidth(dp(78.0f));
        if (pwmGenerator) {
            ImGui::TextDisabled("PWM 发生器");
            ImGui::SameLine(0.0f, dp(8.0f));
        } else if (ImGui::Combo("##wave", &mode, modes, 3)) {
            pushUndo();
            component->source.mode = static_cast<Waveform>(mode);
            if (component->source.mode == Waveform::DC) {
                component->source.dc = component->source.amplitude;
                component->value = component->source.amplitude;
            } else {
                component->source.dc = 0.0;
                if (component->source.amplitude <= 0.0) {
                    component->source.amplitude = 5.0;
                }
                if (component->source.frequency <= 0.0) {
                    component->source.frequency = 1000.0;
                }
                component->source.period =
                    1.0 / std::max(1e-9, component->source.frequency);
                component->source.pulseWidth =
                    pulseWidthFor(component->source.period, component->source.duty);
                component->source.rise = pulseRiseFor(component->source.period);
                component->value = component->source.amplitude;
            }
            circuit_.touch();
            markChanged();
        }
        ImGui::SameLine(0.0f, dp(8.0f));
        drawFieldSlider(component->id, NumberField::Amplitude, dp(120.0f));
        ImGui::SameLine(0.0f, dp(6.0f));
        drawValueChip("##amplitude_chip", component->id, NumberField::Amplitude,
                      formatSI(component->source.amplitude, "V"),
                      ImVec2(dp(76.0f), chipHeight));
        if (component->source.mode != Waveform::DC) {
            ImGui::SetCursorPos(ImVec2(dp(2.0f), contentY + dp(38.0f)));
            drawFieldSlider(component->id, NumberField::Frequency, dp(150.0f));
            ImGui::SameLine(0.0f, dp(6.0f));
            drawValueChip("##frequency_chip", component->id, NumberField::Frequency,
                          formatSI(component->source.frequency, "Hz"),
                          ImVec2(dp(88.0f), chipHeight));

            if (component->source.mode == Waveform::Pulse) {
                // Duty cycle is edited as a percentage: a 0..1 float with a
                // "%.0f%%" format used to round 0.5 to "0%" and looked like a
                // dead control. Fractional values are allowed here, so 33.3 %
                // is exactly 33.3 %.
                const auto applyDuty = [&](double percent) {
                    applyNumberField(component->id, NumberField::Duty, percent);
                };
                ImGui::SetCursorPos(ImVec2(dp(2.0f), contentY + dp(76.0f)));
                drawFieldSlider(component->id, NumberField::Duty, dp(120.0f));
                ImGui::SameLine(0.0f, dp(6.0f));
                drawValueChip("##duty_chip", component->id, NumberField::Duty,
                              formatSI(component->source.duty * 100.0, "%"),
                              ImVec2(dp(76.0f), chipHeight));
                ImGui::SameLine(0.0f, dp(6.0f));
                if (ImGui::Button("-5%", ImVec2(dp(50.0f), chipHeight))) {
                    pushUndo();
                    applyDuty(component->source.duty * 100.0 - 5.0);
                }
                ImGui::SameLine(0.0f, dp(6.0f));
                if (ImGui::Button("+5%", ImVec2(dp(50.0f), chipHeight))) {
                    pushUndo();
                    applyDuty(component->source.duty * 100.0 + 5.0);
                }
                if (width >= dp(600.0f)) {
                    ImGui::SameLine(0.0f, dp(8.0f));
                    ImGui::TextDisabled("平均 %s",
                                        formatSI(component->source.amplitude *
                                                     component->source.duty,
                                                 "V")
                                            .c_str());
                }
            }
        }
    } else if (component->type == ComponentType::Diode ||
               component->type == ComponentType::SchottkyDiode ||
               component->type == ComponentType::Led ||
               component->type == ComponentType::Zener) {
        ImGui::SetCursorPos(ImVec2(dp(2.0f), contentY));
        ImGui::TextDisabled("型号");
        ImGui::SameLine(0.0f, dp(8.0f));
        ImGui::TextUnformatted(componentValueLabel(*component).c_str());
        if (component->type == ComponentType::Zener) {
            ImGui::SetCursorPos(ImVec2(dp(2.0f), contentY + dp(32.0f)));
            drawFieldSlider(component->id, NumberField::Value, rowSliderWidth());
            ImGui::SameLine(0.0f, dp(8.0f));
            drawValueChip("##zener_chip", component->id, NumberField::Value,
                          formatSI(component->value, "V"), ImVec2(dp(88.0f), chipHeight));
        }
        if (live_) {
            double current = 0.0;
            if (currentForPin(*component, 0, liveSampleIndex(), &current)) {
                ImGui::SetCursorPos(ImVec2(dp(2.0f), contentY + dp(32.0f) +
                                                     (component->type == ComponentType::Zener
                                                          ? dp(34.0f)
                                                          : dp(0.0f))));
                ImGui::TextDisabled("电流 %s",
                                    formatSI(std::fabs(current), "A").c_str());
            }
        }
    } else if (component->type == ComponentType::Ground) {
        ImGui::SetCursorPos(ImVec2(dp(2.0f), contentY));
        ImGui::TextDisabled("电路参考点");
    } else {
        // Discrete transistors and integrated circuits: model name and one
        // current readout per pin.
        ImGui::SetCursorPos(ImVec2(dp(2.0f), contentY));
        ImGui::TextDisabled("型号");
        ImGui::SameLine(0.0f, dp(8.0f));
        ImGui::TextUnformatted(componentValueLabel(*component).c_str());

        const PinLayout& layout = pinLayout(component->type);
        const int maxColumns = width < dp(520.0f) ? 2 : 4;
        const int columns = layout.count > 4 ? maxColumns : layout.count;
        const float cellWidth = (width - dp(20.0f)) / static_cast<float>(columns);
        bool any = false;
        for (int pin = 0; pin < layout.count; ++pin) {
            double current = 0.0;
            if (!currentForPin(*component, pin, liveSampleIndex(), &current)) {
                continue;
            }
            const int row = columns > 0 ? pin / columns : 0;
            const int column = columns > 0 ? pin % columns : 0;
            ImGui::SetCursorPos(
                ImVec2(dp(2.0f) + cellWidth * static_cast<float>(column),
                       contentY + dp(30.0f) + dp(22.0f) * static_cast<float>(row)));
            char rowText[96];
            std::snprintf(rowText, sizeof(rowText), "%s %s",
                          layout.pins[static_cast<size_t>(pin)].name,
                          formatSI(current, "A").c_str());
            ImGui::TextDisabled("%s", rowText);
            any = true;
        }
        if (!any) {
            ImGui::SetCursorPos(ImVec2(dp(2.0f), contentY + dp(30.0f)));
            ImGui::TextDisabled("启动仿真后显示各脚电流");
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

}  // namespace circuitlab
