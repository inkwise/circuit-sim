#include "App.h"

#include "Theme.h"

#include "circuit/Engineering.h"
#include "circuit/Measure.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace circuitlab {

void App::drawTransientPlot(const ImVec2& size)
{
    if (size.x < 120.0f || size.y < 80.0f) {
        return;
    }
    ImGui::InvisibleButton("##waveplot", size);
    const ImVec2 origin = ImGui::GetItemRectMin();
    const ImVec2 end(origin.x + size.x, origin.y + size.y);
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    drawList->AddRectFilled(origin, end, Theme::canvas(), 5.0f * uiScale_);

    const float left = 62.0f * uiScale_;
    const float right = 18.0f * uiScale_;
    const float top = 14.0f * uiScale_;
    const float bottom = 30.0f * uiScale_;
    const ImVec2 p0(origin.x + left, origin.y + top);
    const ImVec2 p1(end.x - right, end.y - bottom);
    if (p1.x - p0.x < 40.0f || p1.y - p0.y < 30.0f) {
        return;
    }

    std::vector<int> nodes;
    for (const auto& entry : result_.nodeVoltage) {
        const auto visible = visibleNodes_.find(entry.first);
        if (visible == visibleNodes_.end() || visible->second) {
            nodes.push_back(entry.first);
        }
    }
    if (result_.time.empty() || nodes.empty()) {
        drawList->AddText(ImVec2(origin.x + 16.0f * uiScale_, origin.y + 16.0f * uiScale_),
                          Theme::muted(), "没有可显示的节点电压");
        return;
    }

    const double t0 = result_.time.front();
    const double t1 = result_.time.back();
    const double timeSpan = (t1 > t0) ? (t1 - t0) : 1.0;

    double yMin = std::numeric_limits<double>::infinity();
    double yMax = -std::numeric_limits<double>::infinity();
    for (int node : nodes) {
        const auto found = result_.nodeVoltage.find(node);
        if (found == result_.nodeVoltage.end()) {
            continue;
        }
        for (double value : found->second) {
            if (!std::isfinite(value)) {
                continue;
            }
            yMin = std::min(yMin, value);
            yMax = std::max(yMax, value);
        }
    }
    if (!std::isfinite(yMin) || !std::isfinite(yMax)) {
        yMin = 0.0;
        yMax = 1.0;
    }
    if (std::fabs(yMax - yMin) < 1e-12) {
        const double center = (yMin + yMax) * 0.5;
        const double pad = std::max(0.5, std::fabs(center) * 0.1);
        yMin = center - pad;
        yMax = center + pad;
    } else {
        const double pad = (yMax - yMin) * 0.08;
        yMin -= pad;
        yMax += pad;
    }

    const auto mapX = [&](double t) {
        return p0.x + static_cast<float>((t - t0) / timeSpan) * (p1.x - p0.x);
    };
    const auto mapY = [&](double value) {
        return p1.y - static_cast<float>((value - yMin) / (yMax - yMin)) * (p1.y - p0.y);
    };

    // Recessive grid.
    for (int i = 0; i <= 4; ++i) {
        const float fraction = static_cast<float>(i) / 4.0f;
        const float x = p0.x + (p1.x - p0.x) * fraction;
        const float y = p1.y - (p1.y - p0.y) * fraction;
        drawList->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), Theme::grid(), 1.0f);
        drawList->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), Theme::grid(), 1.0f);
    }

    // Axis labels use a smaller size so a compact plot does not turn into a
    // pile of overlapping numbers.
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.72f);
    for (int i = 0; i <= 4; ++i) {
        const float fraction = static_cast<float>(i) / 4.0f;
        const float x = p0.x + (p1.x - p0.x) * fraction;
        const float y = p1.y - (p1.y - p0.y) * fraction;

        const double time = t0 + timeSpan * fraction;
        const std::string timeLabel = formatAxis(time, "s");
        const ImVec2 timeSize = ImGui::CalcTextSize(timeLabel.c_str());
        drawList->AddText(ImVec2(x - timeSize.x * 0.5f, p1.y + 5.0f * uiScale_),
                          Theme::muted(), timeLabel.c_str());

        if (i % 2 == 0) {
            const double voltage = yMin + (yMax - yMin) * fraction;
            const std::string voltageLabel = formatAxis(voltage, "V");
            const ImVec2 voltageSize = ImGui::CalcTextSize(voltageLabel.c_str());
            drawList->AddText(ImVec2(p0.x - voltageSize.x - 6.0f * uiScale_,
                                     y - voltageSize.y * 0.5f),
                              Theme::muted(), voltageLabel.c_str());
        }
    }
    ImGui::PopFont();

    drawList->AddLine(ImVec2(p0.x, p0.y), ImVec2(p0.x, p1.y), Theme::axis(), 1.0f);
    drawList->AddLine(ImVec2(p0.x, p1.y), ImVec2(p1.x, p1.y), Theme::axis(), 1.0f);

    const int maxPoints = std::max(1, static_cast<int>((p1.x - p0.x) * 2.0f));
    // Color follows the node entity, not its rank among the currently visible
    // traces, so hiding a trace never repaints the survivors.
    std::map<int, int> colorIndex;
    int colorCursor = 0;
    for (const auto& entry : result_.nodeVoltage) {
        colorIndex[entry.first] = colorCursor++;
    }
    struct Label {
        int node;
        float y;
        ImU32 color;
    };
    std::vector<Label> labels;

    for (int node : nodes) {
        const auto found = result_.nodeVoltage.find(node);
        if (found == result_.nodeVoltage.end() || found->second.empty()) {
            continue;
        }
        const std::vector<double>& values = found->second;
        const ImU32 color = Theme::seriesColor(colorIndex[node]);
        const int stride = std::max(1, static_cast<int>(values.size()) / maxPoints);
        ImVec2 previous(0.0f, 0.0f);
        bool havePrevious = false;
        for (int i = 0; i < static_cast<int>(values.size()); ++i) {
            if (i % stride != 0 && i != static_cast<int>(values.size()) - 1) {
                continue;
            }
            const size_t index = static_cast<size_t>(std::min(
                i, static_cast<int>(result_.time.size()) - 1));
            const double value = values[index];
            if (!std::isfinite(value)) {
                havePrevious = false;
                continue;
            }
            const ImVec2 point(mapX(result_.time[index]), mapY(value));
            if (havePrevious) {
                drawList->AddLine(previous, point, color, 2.0f * uiScale_);
            }
            previous = point;
            havePrevious = true;
        }
        if (!values.empty()) {
            const size_t last = std::min(values.size() - 1, result_.time.size() - 1);
            labels.push_back(Label{node, mapY(values[last]), color});
        }
    }

    // Direct labels for up to four visible series; identity also lives in the
    // checkbox legend above the plot.
    if (labels.size() <= 4 && !labels.empty()) {
        std::sort(labels.begin(), labels.end(), [](const Label& a, const Label& b) {
            return a.y < b.y;
        });
        const float spacing = ImGui::GetFontSize() + 4.0f * uiScale_;
        for (size_t i = 0; i < labels.size(); ++i) {
            if (i > 0 && labels[i].y < labels[i - 1].y + spacing) {
                labels[i].y = labels[i - 1].y + spacing;
            }
        }
        const float maxY = p1.y - spacing;
        for (size_t i = labels.size(); i-- > 0;) {
            if (labels[i].y > maxY) {
                labels[i].y = maxY;
            }
            if (i + 1 < labels.size() && labels[i].y > labels[i + 1].y - spacing) {
                labels[i].y = labels[i + 1].y - spacing;
            }
        }
        for (const auto& label : labels) {
            const ImVec2 anchor(p1.x - 5.0f * uiScale_, std::clamp(label.y, p0.y + 4.0f, p1.y - 4.0f));
            drawList->AddCircleFilled(anchor, 3.0f * uiScale_, label.color);
            char text[64];
            std::snprintf(text, sizeof(text), "节点 %d", label.node);
            const ImVec2 textSize = ImGui::CalcTextSize(text);
            drawList->AddText(ImVec2(anchor.x - textSize.x - 7.0f * uiScale_,
                                     anchor.y - textSize.y * 0.5f),
                              Theme::textDim(), text);
        }
    }

    // Touch/mouse crosshair with a compact value readout.
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        if (mouse.x >= p0.x && mouse.x <= p1.x && mouse.y >= p0.y && mouse.y <= p1.y) {
            const double time = t0 + (mouse.x - p0.x) / (p1.x - p0.x) * timeSpan;
            size_t nearest = 0;
            double bestDistance = std::fabs(result_.time[0] - time);
            for (size_t i = 1; i < result_.time.size(); ++i) {
                const double distance = std::fabs(result_.time[i] - time);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    nearest = i;
                }
            }
            const float crossX = mapX(result_.time[nearest]);
            drawList->AddLine(ImVec2(crossX, p0.y), ImVec2(crossX, p1.y),
                              IM_COL32(195, 194, 183, 110), 1.0f);

            std::vector<std::string> lines;
            lines.push_back("t = " + formatSI(result_.time[nearest], "s"));
            for (int node : nodes) {
                const auto found = result_.nodeVoltage.find(node);
                if (found == result_.nodeVoltage.end() || found->second.empty()) {
                    continue;
                }
                const ImU32 color = Theme::seriesColor(colorIndex[node]);
                const size_t index = std::min(nearest, found->second.size() - 1);
                const double value = found->second[index];
                if (std::isfinite(value)) {
                    const float markY = mapY(value);
                    const ImVec2 mark(crossX, markY);
                    drawList->AddCircleFilled(mark, 4.0f * uiScale_, color);
                    drawList->AddCircle(mark, 5.5f * uiScale_, Theme::canvas(), 0,
                                        1.5f * uiScale_);
                }
                char buffer[96];
                std::snprintf(buffer, sizeof(buffer), "节点 %d: %s", node,
                              formatSI(value, "V").c_str());
                lines.push_back(buffer);
            }

            float width = 0.0f;
            for (const auto& line : lines) {
                width = std::max(width, ImGui::CalcTextSize(line.c_str()).x);
            }
            const float lineHeight = ImGui::GetFontSize() + 2.0f * uiScale_;
            const ImVec2 boxSize(width + 18.0f * uiScale_,
                                 lineHeight * static_cast<float>(lines.size()) +
                                     10.0f * uiScale_);
            ImVec2 boxPos(mouse.x + 14.0f * uiScale_, mouse.y - boxSize.y - 10.0f * uiScale_);
            boxPos.x = std::clamp(boxPos.x, p0.x + 4.0f, p1.x - boxSize.x - 4.0f);
            boxPos.y = std::clamp(boxPos.y, p0.y + 4.0f, p1.y - boxSize.y - 4.0f);
            drawList->AddRectFilled(boxPos, ImVec2(boxPos.x + boxSize.x, boxPos.y + boxSize.y),
                                    IM_COL32(29, 37, 49, 238), 6.0f * uiScale_);
            drawList->AddRect(boxPos, ImVec2(boxPos.x + boxSize.x, boxPos.y + boxSize.y),
                              IM_COL32(255, 255, 255, 28), 6.0f * uiScale_, 0,
                              1.0f * uiScale_);
            for (size_t i = 0; i < lines.size(); ++i) {
                drawList->AddText(ImVec2(boxPos.x + 9.0f * uiScale_,
                                         boxPos.y + 5.0f * uiScale_ +
                                             lineHeight * static_cast<float>(i)),
                                  i == 0 ? Theme::text() : Theme::textDim(),
                                  lines[i].c_str());
            }
        }
    }

    drawList->AddRect(p0, p1, Theme::axis(), 4.0f * uiScale_, 0, 1.0f * uiScale_);
}


// ---------------------------------------------------------------------------
// Oscilloscope
// ---------------------------------------------------------------------------

namespace {

bool sampleNode(const SimulationResult& result, int node, size_t index, double* value)
{
    if (node <= 0) {
        *value = 0.0;
        return true;
    }
    const auto entry = result.nodeVoltage.find(node);
    if (entry == result.nodeVoltage.end() || entry->second.empty()) {
        return false;
    }
    const size_t sample = std::min(index, entry->second.size() - 1);
    *value = entry->second[sample];
    return std::isfinite(*value);
}

bool probeSample(const CompiledCircuit& compiled, const SimulationResult& result,
                 const Component& component, size_t index, double* value)
{
    if (!value) {
        return false;
    }
    const auto nodes = compiled.nodes.find(component.id);
    if (nodes == compiled.nodes.end() || nodes->second.a() < 0 ||
        nodes->second.b() < 0) {
        return false;
    }
    double a = 0.0;
    double b = 0.0;
    if (!sampleNode(result, nodes->second.a(), index, &a) ||
        !sampleNode(result, nodes->second.b(), index, &b)) {
        return false;
    }
    *value = a - b;
    return true;
}

}  // namespace

void App::refreshProbeCache()
{
    probeStats_.clear();
    probeSeriesCache_.clear();
    if (!result_.ok || result_.kind != AnalysisKind::Transient || result_.time.empty()) {
        return;
    }
    for (const auto& component : circuit_.components) {
        if (!isProbeType(component.type)) {
            continue;
        }
        std::vector<double> series = probeSeries(component);
        if (series.empty()) {
            continue;
        }
        probeStats_[component.id] = measureWave(result_.time, series);
        probeSeriesCache_[component.id] = std::move(series);
    }
}

std::vector<double> App::probeSeries(const Component& component) const
{
    std::vector<double> series;
    if (result_.time.empty()) {
        return series;
    }
    series.assign(result_.time.size(), 0.0);
    for (size_t i = 0; i < series.size(); ++i) {
        if (!probeSample(compiled_, result_, component, i, &series[i])) {
            series.clear();
            return series;
        }
    }
    return series;
}

void App::drawScopeTab()
{
    if (!hasResult_ || !result_.ok || result_.kind != AnalysisKind::Transient) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("示波器显示瞬态波形。给电路加上 PWM 或交流信号源后启动仿真。");
        ImGui::PopTextWrapPos();
        return;
    }
    if (resultRevision_ != circuit_.revision) {
        ImGui::TextColored(Theme::toVec4(Theme::warning()), "电路已修改，等待重新仿真。");
        return;
    }

    std::vector<const Component*> probes;
    for (const auto& component : circuit_.components) {
        if (isProbeType(component.type)) {
            probes.push_back(&component);
        }
    }
    if (probes.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("电路里还没有示波器探头。点 + → 仪器 → 示波器，"
                            "把 CH1 接到要观察的节点，GND 接到参考点。");
        ImGui::PopTextWrapPos();
        return;
    }

    // Time window selector: whole run, settled tail, or a couple of periods.
    const char* spans[] = {"全程", "稳态", "纹波"};
    for (int i = 0; i < 3; ++i) {
        if (i > 0) {
            ImGui::SameLine(0.0f, dp(6.0f));
        }
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Button,
                              Theme::toVec4(scopeSpan_ == i ? Theme::accent()
                                                            : Theme::surface()));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              Theme::toVec4(scopeSpan_ == i ? Theme::canvas()
                                                            : Theme::text()));
        if (ImGui::Button(spans[i], ImVec2(dp(58.0f), dp(30.0f)))) {
            scopeSpan_ = i;
        }
        ImGui::PopStyleColor(2);
        ImGui::PopID();
    }
    ImGui::SameLine(0.0f, dp(12.0f));
    ImGui::TextDisabled("%s", scopeSpan_ == 0 ? "完整瞬态窗口"
                                              : (scopeSpan_ == 1 ? "最后 5%（稳态）"
                                                                 : "约 2 个开关周期"));

    // Trace selection and the steady-state numbers shown above the plot.
    if (ImGui::BeginChild("##scope_legend", ImVec2(0.0f, dp(54.0f)), 0,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        for (size_t i = 0; i < probes.size(); ++i) {
            const Component& probe = *probes[i];
            ImGui::PushID(probe.id);
            auto visible = visibleProbes_.find(probe.id);
            if (visible == visibleProbes_.end()) {
                visible = visibleProbes_.emplace(probe.id, true).first;
            }
            bool& shown = visible->second;
            ImGui::ColorButton("##color", Theme::toVec4(Theme::seriesColor(
                                               static_cast<int>(i))),
                               ImGuiColorEditFlags_NoTooltip |
                                   ImGuiColorEditFlags_NoDragDrop |
                                   ImGuiColorEditFlags_NoPicker,
                               ImVec2(dp(14.0f), dp(14.0f)));
            ImGui::SameLine();
            ImGui::Checkbox(probe.name.c_str(), &shown);
            ImGui::SameLine();
            const auto cachedStats = probeStats_.find(probe.id);
            if (cachedStats != probeStats_.end() && cachedStats->second.ok) {
                const WaveStats& stats = cachedStats->second;
                char text[160];
                std::snprintf(text, sizeof(text), "%s  ·  纹波 %s（%s）  ·  %s",
                              formatSI(stats.mean, "V").c_str(),
                              formatSI(stats.ripple, "V").c_str(),
                              formatSI(stats.ripple / std::max(1e-9, std::fabs(stats.mean)) *
                                           100.0, "%")
                                  .c_str(),
                              stats.frequency > 0.0
                                  ? formatSI(stats.frequency, "Hz").c_str()
                                  : "非周期");
                ImGui::TextDisabled("%s", text);
            } else {
                ImGui::TextDisabled("无数据");
            }
            ImGui::PopID();
            ImGui::SameLine(0.0f, dp(16.0f));
        }
    }
    ImGui::EndChild();

    drawScopePlot(ImGui::GetContentRegionAvail(), probes);
}

void App::drawScopePlot(const ImVec2& size, const std::vector<const Component*>& probes)
{
    if (size.x < 140.0f || size.y < 90.0f) {
        return;
    }
    ImGui::InvisibleButton("##scopeplot", size);
    const ImVec2 origin = ImGui::GetItemRectMin();
    const ImVec2 end(origin.x + size.x, origin.y + size.y);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(origin, end, Theme::canvas(), 5.0f * uiScale_);

    const float left = dp(62.0f);
    const float right = dp(16.0f);
    const float top = dp(12.0f);
    const float bottom = dp(28.0f);
    const ImVec2 p0(origin.x + left, origin.y + top);
    const ImVec2 p1(end.x - right, end.y - bottom);
    if (p1.x - p0.x < 50.0f || p1.y - p0.y < 40.0f) {
        return;
    }

    // Trace series, after the visibility filter.
    struct Trace {
        const Component* probe = nullptr;
        std::vector<double> values;
        ImU32 color = 0;
        WaveStats stats;
    };
    std::vector<Trace> traces;
    for (size_t i = 0; i < probes.size(); ++i) {
        const Component& probe = *probes[i];
        const auto visible = visibleProbes_.find(probe.id);
        if (visible != visibleProbes_.end() && !visible->second) {
            continue;
        }
        const auto cachedSeries = probeSeriesCache_.find(probe.id);
        const auto cachedStats = probeStats_.find(probe.id);
        if (cachedSeries == probeSeriesCache_.end() || cachedSeries->second.empty()) {
            continue;
        }
        Trace trace;
        trace.probe = &probe;
        trace.values = cachedSeries->second;
        trace.color = Theme::seriesColor(static_cast<int>(i));
        if (cachedStats != probeStats_.end()) {
            trace.stats = cachedStats->second;
        }
        traces.push_back(std::move(trace));
    }
    if (traces.empty() || result_.time.empty()) {
        drawList->AddText(ImVec2(origin.x + dp(16.0f), origin.y + dp(16.0f)),
                          Theme::muted(), "没有可显示的探头");
        return;
    }

    // Time window: the whole transient, the settled tail, or about two
    // switching periods so the ripple fills the screen.
    const double tLast = result_.time.back();
    const double tFirst = result_.time.front();
    const double span = std::max(1e-15, tLast - tFirst);
    double period = 0.0;
    for (const auto& trace : traces) {
        if (trace.stats.ok && trace.stats.frequency > 0.0) {
            period = 1.0 / trace.stats.frequency;
            break;
        }
    }
    double t0 = tFirst;
    if (scopeSpan_ == 1) {
        t0 = tLast - span * 0.05;
    } else if (scopeSpan_ == 2) {
        const double window = period > 0.0 ? period * 2.5 : span * 0.02;
        t0 = std::max(tFirst, tLast - window);
    }
    const double t1 = tLast;
    const double window = std::max(1e-15, t1 - t0);

    // Vertical range from the samples inside the window.
    double yMin = std::numeric_limits<double>::infinity();
    double yMax = -std::numeric_limits<double>::infinity();
    for (const auto& trace : traces) {
        for (size_t i = 0; i < trace.values.size() && i < result_.time.size(); ++i) {
            if (result_.time[i] < t0) {
                continue;
            }
            const double value = trace.values[i];
            if (!std::isfinite(value)) {
                continue;
            }
            yMin = std::min(yMin, value);
            yMax = std::max(yMax, value);
        }
    }
    if (!std::isfinite(yMin) || !std::isfinite(yMax)) {
        yMin = 0.0;
        yMax = 1.0;
    }
    if (std::fabs(yMax - yMin) < 1e-9) {
        const double center = (yMin + yMax) * 0.5;
        const double pad = std::max(0.5, std::fabs(center) * 0.1);
        yMin = center - pad;
        yMax = center + pad;
    } else {
        const double pad = (yMax - yMin) * 0.12;
        yMin -= pad;
        yMax += pad;
    }

    const auto mapX = [&](double t) {
        return p0.x + static_cast<float>((t - t0) / window) * (p1.x - p0.x);
    };
    const auto mapY = [&](double value) {
        return p1.y - static_cast<float>((value - yMin) / (yMax - yMin)) * (p1.y - p0.y);
    };

    for (int i = 0; i <= 8; ++i) {
        const float fraction = static_cast<float>(i) / 8.0f;
        const float x = p0.x + (p1.x - p0.x) * fraction;
        const float y = p1.y - (p1.y - p0.y) * fraction;
        drawList->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), Theme::grid(), 1.0f);
        if (i % 2 == 0) {
            drawList->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), Theme::grid(), 1.0f);
        }
    }

    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.72f);
    for (int i = 0; i <= 4; ++i) {
        const float fraction = static_cast<float>(i) / 4.0f;
        const float x = p0.x + (p1.x - p0.x) * fraction;
        const double time = t0 + window * static_cast<double>(fraction);
        const std::string label = formatAxis(time, "s");
        const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
        drawList->AddText(ImVec2(x - textSize.x * 0.5f, p1.y + dp(5.0f)), Theme::muted(),
                          label.c_str());
    }
    for (int i = 0; i <= 4; ++i) {
        const float fraction = static_cast<float>(i) / 4.0f;
        const float y = p1.y - (p1.y - p0.y) * fraction;
        const double voltage = yMin + (yMax - yMin) * static_cast<double>(fraction);
        const std::string label = formatAxis(voltage, "V");
        const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
        drawList->AddText(ImVec2(p0.x - textSize.x - dp(6.0f), y - textSize.y * 0.5f),
                          Theme::muted(), label.c_str());
    }
    ImGui::PopFont();

    drawList->AddLine(ImVec2(p0.x, p0.y), ImVec2(p0.x, p1.y), Theme::axis(), 1.0f);
    drawList->AddLine(ImVec2(p0.x, p1.y), ImVec2(p1.x, p1.y), Theme::axis(), 1.0f);

    const int maxPoints = std::max(2, static_cast<int>((p1.x - p0.x) * 2.0f));
    for (const auto& trace : traces) {
        const size_t count = std::min(trace.values.size(), result_.time.size());
        size_t begin = 0;
        while (begin < count && result_.time[begin] < t0) {
            ++begin;
        }
        if (begin > 0) {
            --begin;
        }
        const size_t available = count > begin ? count - begin : 0;
        const size_t stride = std::max<size_t>(1, available / static_cast<size_t>(maxPoints));
        ImVec2 previous(0.0f, 0.0f);
        bool havePrevious = false;
        for (size_t i = begin; i < count; ++i) {
            if ((i - begin) % stride != 0 && i + 1 != count) {
                continue;
            }
            const double value = trace.values[i];
            if (!std::isfinite(value)) {
                havePrevious = false;
                continue;
            }
            const ImVec2 point(mapX(result_.time[i]), mapY(value));
            if (havePrevious) {
                drawList->AddLine(previous, point, trace.color, 2.0f * uiScale_);
            }
            previous = point;
            havePrevious = true;
        }

        // Mean level and the ripple band, so "is it really DC?" is obvious.
        if (trace.stats.ok) {
            const float meanY = mapY(trace.stats.mean);
            if (meanY > p0.y && meanY < p1.y) {
                drawList->AddLine(ImVec2(p0.x, meanY), ImVec2(p1.x, meanY),
                                  IM_COL32(236, 235, 226, 90), 1.0f);
            }
        }
    }

    // Compact readout in the corner: the numbers the user came for.
    if (traces.front().stats.ok) {
        const WaveStats& stats = traces.front().stats;
        char text[192];
        std::snprintf(text, sizeof(text),
                      "%s  平均 %s   峰峰 %s   有效值 %s   %s", traces.front().probe->name.c_str(),
                      formatSI(stats.mean, "V").c_str(), formatSI(stats.ripple, "V").c_str(),
                      formatSI(stats.rms, "V").c_str(),
                      stats.frequency > 0.0 ? formatSI(stats.frequency, "Hz").c_str()
                                            : "非周期");
        const ImVec2 textSize = ImGui::CalcTextSize(text);
        const ImVec2 boxMin(p0.x + dp(6.0f), p0.y + dp(4.0f));
        drawList->AddRectFilled(boxMin,
                                ImVec2(boxMin.x + textSize.x + dp(14.0f),
                                       boxMin.y + textSize.y + dp(8.0f)),
                                IM_COL32(29, 37, 49, 215), dp(5.0f));
        drawList->AddText(ImVec2(boxMin.x + dp(7.0f), boxMin.y + dp(4.0f)), Theme::textDim(),
                          text);
    }

    drawList->AddRect(p0, p1, Theme::axis(), dp(4.0f), 0, 1.0f);
}

}  // namespace circuitlab
