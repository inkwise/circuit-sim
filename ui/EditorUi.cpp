// Modern editor chrome: bottom bar, floating tool cluster, zoom controls,
// placement chip and the two-level component sheet.

#include "App.h"

#include "Symbols.h"
#include "Theme.h"

#include "circuit/ComponentCatalog.h"
#include "circuit/Engineering.h"

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

void App::drawSelectionCard()
{
    if (resultsOpen_ || sheetOpen()) {
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
            height = dp(200.0f);
            break;
        case ComponentType::Oscilloscope:
            height = dp(160.0f);
            break;
        case ComponentType::TerminalBlock:
            height = dp(96.0f);
            break;
        case ComponentType::Bulb:
        case ComponentType::Fan:
            height = dp(232.0f);
            break;
        case ComponentType::VoltageSource:
        case ComponentType::AcSource:
        case ComponentType::SignalSource:
            // A pulse source also shows the duty-cycle row; without the extra
            // height that row would be clipped and untouchable.
            height = component->source.mode == Waveform::Pulse ? dp(200.0f)
                                                               : dp(152.0f);
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

    ImGui::SetCursorPos(ImVec2(dp(2.0f), dp(9.0f)));
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.05f);
    ImGui::TextUnformatted(component->name.c_str());
    ImGui::PopFont();
    ImGui::SameLine(0.0f, dp(8.0f));
    ImGui::TextDisabled("%s", componentInfo(component->type).label);

    const float right = width - dp(12.0f) - buttonWidth * 2.0f - dp(7.0f);
    ImGui::SetCursorPos(ImVec2(std::max(dp(2.0f), right), dp(9.0f)));
    if (ImGui::Button("旋转", ImVec2(buttonWidth, buttonHeight))) {
        pushUndo();
        circuit_.rotateComponent(component->id);
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
    const auto valueRow = [&](double minimum, double maximum, float rowY) {
        const double value = std::clamp(component->value, minimum, maximum);
        float logValue = static_cast<float>(std::log10(value));
        const float minLog = static_cast<float>(std::log10(minimum));
        const float maxLog = static_cast<float>(std::log10(maximum));
        ImGui::SetCursorPos(ImVec2(dp(2.0f), rowY));
        ImGui::SetNextItemWidth(std::clamp(width - dp(250.0f), dp(140.0f), dp(360.0f)));
        const bool changed = ImGui::SliderFloat("##value", &logValue, minLog, maxLog, "");
        if (ImGui::IsItemActivated()) {
            pushUndo();
        }
        if (changed) {
            component->value = std::pow(10.0, static_cast<double>(logValue));
            circuit_.touch();
            markChanged();
        }
        ImGui::SameLine(0.0f, dp(8.0f));
        ImGui::TextUnformatted(componentValueLabel(*component).c_str());
        ImGui::SameLine(0.0f, dp(8.0f));
        if (ImGui::Button("÷10", ImVec2(dp(48.0f), dp(32.0f)))) {
            pushUndo();
            component->value = std::max(minimum, component->value / 10.0);
            circuit_.touch();
            markChanged();
        }
        ImGui::SameLine(0.0f, dp(6.0f));
        if (ImGui::Button("×10", ImVec2(dp(48.0f), dp(32.0f)))) {
            pushUndo();
            component->value = std::min(maximum, component->value * 10.0);
            circuit_.touch();
            markChanged();
        }
    };

    // Series resistance (winding DCR or capacitor ESR) of a real filter part.
    const auto seriesRow = [&](double minimum, double maximum, float rowY) {
        const double value = std::clamp(component->seriesR, minimum, maximum);
        float logValue = static_cast<float>(std::log10(std::max(minimum, value)));
        ImGui::SetCursorPos(ImVec2(dp(2.0f), rowY));
        ImGui::SetNextItemWidth(std::clamp(width - dp(250.0f), dp(140.0f), dp(360.0f)));
        const bool changed = ImGui::SliderFloat(
            "##seriesR", &logValue, static_cast<float>(std::log10(minimum)),
            static_cast<float>(std::log10(maximum)), "");
        if (ImGui::IsItemActivated()) {
            pushUndo();
        }
        if (changed) {
            component->seriesR = std::pow(10.0, static_cast<double>(logValue));
            circuit_.touch();
            markChanged();
        }
        ImGui::SameLine(0.0f, dp(8.0f));
        ImGui::Text("内阻 %s", formatSI(component->seriesR, "Ω").c_str());
        ImGui::SameLine(0.0f, dp(8.0f));
        ImGui::TextDisabled(component->type == ComponentType::PowerInductor ? "DCR"
                                                                           : "ESR");
    };

    if (component->type == ComponentType::Resistor) {
        valueRow(1.0, 1e7, contentY);
    } else if (component->type == ComponentType::Capacitor) {
        valueRow(1e-12, 1e-2, contentY);
    } else if (component->type == ComponentType::Inductor) {
        valueRow(1e-9, 1.0, contentY);
    } else if (component->type == ComponentType::PowerInductor) {
        valueRow(1e-9, 1.0, contentY);
        seriesRow(1e-3, 1e2, contentY + dp(40.0f));
    } else if (component->type == ComponentType::ElectrolyticCap) {
        valueRow(1e-7, 1e-1, contentY);
        seriesRow(1e-3, 1e2, contentY + dp(40.0f));
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
    } else if (isLoadType(component->type)) {
        valueRow(1.0, 1e7, contentY);

        // Realistic load ratings: nominal voltage, start-up voltage, maximum
        // voltage (e.g. a fan tolerates +10 %) and maximum current.
        const bool fan = component->type == ComponentType::Fan;
        const float row1 = contentY + dp(40.0f);
        const float row2 = row1 + dp(38.0f);
        const float row3 = row2 + dp(38.0f);

        ImGui::SetCursorPos(ImVec2(dp(2.0f), row1));
        ImGui::TextDisabled("额定");
        ImGui::SameLine(0.0f, dp(6.0f));
        ImGui::SetNextItemWidth(dp(118.0f));
        float rated = static_cast<float>(std::clamp(component->ratedVoltage, 1.0, 48.0));
        const bool ratedChanged = ImGui::SliderFloat("##rated", &rated, 1.0f, 48.0f, "%.1f V");
        if (ImGui::IsItemActivated()) {
            pushUndo();
        }
        if (ratedChanged) {
            // Keep the start-up / maximum limits in proportion to the rating.
            const double previous = std::max(0.1, component->ratedVoltage);
            const double scale = static_cast<double>(rated) / previous;
            component->ratedVoltage = rated;
            component->minVoltage *= scale;
            component->maxVoltage *= scale;
            applyLoadDefaults(*component);
            circuit_.touch();
            markChanged();
        }
        ImGui::SameLine(0.0f, dp(14.0f));
        ImGui::TextDisabled("启动");
        ImGui::SameLine(0.0f, dp(6.0f));
        ImGui::SetNextItemWidth(dp(118.0f));
        float start = static_cast<float>(
            std::clamp(component->minVoltage, 0.5, std::max(0.6, component->ratedVoltage)));
        const bool startChanged = ImGui::SliderFloat(
            "##minv", &start, 0.5f, static_cast<float>(std::max(1.0, component->ratedVoltage)),
            "%.1f V");
        if (ImGui::IsItemActivated()) {
            pushUndo();
        }
        if (startChanged) {
            component->minVoltage = start;
            circuit_.touch();
            markChanged();
        }

        ImGui::SetCursorPos(ImVec2(dp(2.0f), row2));
        ImGui::TextDisabled("上限");
        ImGui::SameLine(0.0f, dp(6.0f));
        ImGui::SetNextItemWidth(dp(118.0f));
        const float maxLimit = static_cast<float>(std::max(component->ratedVoltage * 2.0, 2.0));
        const float minLimit = static_cast<float>(std::max(component->ratedVoltage * 0.5, 1.0));
        float maxVolts = static_cast<float>(
            std::clamp(component->maxVoltage, static_cast<double>(minLimit),
                       static_cast<double>(maxLimit)));
        const bool maxChanged =
            ImGui::SliderFloat("##maxv", &maxVolts, minLimit, maxLimit, "%.1f V");
        if (ImGui::IsItemActivated()) {
            pushUndo();
        }
        if (maxChanged) {
            component->maxVoltage = maxVolts;
            component->maxCurrent = std::max(component->maxCurrent, 0.0);
            circuit_.touch();
            markChanged();
        }
        ImGui::SameLine(0.0f, dp(14.0f));
        ImGui::TextDisabled("电流上限");
        ImGui::SameLine(0.0f, dp(6.0f));
        ImGui::SetNextItemWidth(dp(118.0f));
        float maxAmps = static_cast<float>(std::max(0.01, component->maxCurrent));
        float logAmps = std::log10(maxAmps);
        const bool ampsChanged = ImGui::SliderFloat("##maxa", &logAmps, -2.0f, 1.3f, "");
        if (ImGui::IsItemActivated()) {
            pushUndo();
        }
        if (ampsChanged) {
            component->maxCurrent = std::pow(10.0, static_cast<double>(logAmps));
            circuit_.touch();
            markChanged();
        }
        ImGui::SameLine(0.0f, dp(8.0f));
        ImGui::TextUnformatted(formatSI(component->maxCurrent, "A").c_str());

        ImGui::SetCursorPos(ImVec2(dp(2.0f), row3));
        ImGui::TextDisabled("额定 %s · %s · %s", formatSI(component->ratedVoltage, "V").c_str(),
                            formatSI(loadRatedCurrent(*component), "A").c_str(),
                            formatSI(loadRatedPower(*component), "W").c_str());
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
        ImGui::SetNextItemWidth(dp(120.0f));
        float amplitude = static_cast<float>(component->source.amplitude);
        const bool amplitudeChanged =
            ImGui::SliderFloat("##amp", &amplitude, 0.0f, 20.0f, "%.2f V");
        if (ImGui::IsItemActivated()) {
            pushUndo();
        }
        if (amplitudeChanged) {
            component->source.amplitude = amplitude;
            if (component->source.mode == Waveform::DC) {
                component->source.dc = amplitude;
            }
            component->value = amplitude;
            circuit_.touch();
            markChanged();
        }
        if (component->source.mode != Waveform::DC) {
            ImGui::SetCursorPos(ImVec2(dp(2.0f), contentY + dp(38.0f)));
            ImGui::SetNextItemWidth(dp(180.0f));
            float frequency =
                static_cast<float>(std::max(1.0, component->source.frequency));
            float logFrequency = std::log10(frequency);
            const bool frequencyChanged =
                ImGui::SliderFloat("##freq", &logFrequency, 0.0f, 6.0f, "");
            if (ImGui::IsItemActivated()) {
                pushUndo();
            }
            if (frequencyChanged) {
                component->source.frequency =
                    std::pow(10.0, static_cast<double>(logFrequency));
                component->source.period =
                    1.0 / std::max(1e-9, component->source.frequency);
                component->source.pulseWidth =
                    pulseWidthFor(component->source.period, component->source.duty);
                component->source.rise = pulseRiseFor(component->source.period);
                circuit_.touch();
                markChanged();
            }
            ImGui::SameLine(0.0f, dp(8.0f));
            ImGui::TextUnformatted(formatSI(component->source.frequency, "Hz").c_str());

            if (component->source.mode == Waveform::Pulse) {
                // Duty cycle is edited as a whole percentage: a 0..1 float with
                // a "%.0f%%" format used to round 0.5 to "0%" and looked like a
                // dead control.
                const auto applyDuty = [&](double value) {
                    component->source.duty = std::clamp(value, 0.01, 0.99);
                    component->source.pulseWidth = pulseWidthFor(
                        component->source.period, component->source.duty);
                    circuit_.touch();
                    markChanged();
                };
                ImGui::SetCursorPos(ImVec2(dp(2.0f), contentY + dp(76.0f)));
                ImGui::SetNextItemWidth(dp(150.0f));
                int dutyPercent = static_cast<int>(
                    std::lround(std::clamp(component->source.duty, 0.01, 0.99) * 100.0));
                const bool dutyChanged =
                    ImGui::SliderInt("##duty", &dutyPercent, 1, 99, "占空比 %d%%");
                if (ImGui::IsItemActivated()) {
                    pushUndo();
                }
                if (dutyChanged) {
                    applyDuty(static_cast<double>(dutyPercent) / 100.0);
                }
                ImGui::SameLine(0.0f, dp(8.0f));
                if (ImGui::Button("-5%", ImVec2(dp(50.0f), dp(32.0f)))) {
                    pushUndo();
                    applyDuty(component->source.duty - 0.05);
                }
                ImGui::SameLine(0.0f, dp(6.0f));
                if (ImGui::Button("+5%", ImVec2(dp(50.0f), dp(32.0f)))) {
                    pushUndo();
                    applyDuty(component->source.duty + 0.05);
                }
                ImGui::SameLine(0.0f, dp(8.0f));
                ImGui::TextDisabled("平均 %s",
                                    formatSI(component->source.amplitude *
                                                 component->source.duty,
                                             "V")
                                        .c_str());
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
            float breakdown = static_cast<float>(std::clamp(component->value, 2.4, 24.0));
            ImGui::SetNextItemWidth(std::clamp(width - dp(250.0f), dp(140.0f), dp(360.0f)));
            const bool changed =
                ImGui::SliderFloat("##bv", &breakdown, 2.4f, 24.0f, "稳压 %.1f V");
            if (ImGui::IsItemActivated()) {
                pushUndo();
            }
            if (changed) {
                component->value = breakdown;
                circuit_.touch();
                markChanged();
            }
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
