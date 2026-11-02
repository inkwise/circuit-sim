#include "circuit/ProjectIO.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace circuitlab {
namespace {

std::string cleanName(const std::string& name)
{
    std::string result;
    result.reserve(name.size());
    for (char c : name) {
        if (c == '\n' || c == '\r') {
            result += ' ';
        } else {
            result += c;
        }
    }
    // Trim surrounding whitespace.
    size_t begin = 0;
    while (begin < result.size() && (result[begin] == ' ' || result[begin] == '\t')) {
        ++begin;
    }
    size_t end = result.size();
    while (end > begin && (result[end - 1] == ' ' || result[end - 1] == '\t')) {
        --end;
    }
    return result.substr(begin, end - begin);
}

bool writeProject(const Circuit& circuit, const std::string& path, const std::string& name,
                  std::string* error)
{
    const std::string temporary = path + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        if (error) {
            *error = "无法写入工程文件：" + temporary;
        }
        return false;
    }

    output << "CIRCUITLAB 1\n";
    const std::string title = cleanName(name);
    if (!title.empty()) {
        output << "NAME " << title << '\n';
    }
    output << std::setprecision(17);
    for (const auto& component : circuit.components) {
        output << "COMP "
               << component.id << ' '
               << componentTypeKey(component.type) << ' '
               << component.name << ' '
               << component.gridX << ' '
               << component.gridY << ' '
               << component.rotation << ' '
               << component.value << ' '
               << waveformKey(component.source.mode) << ' '
               << component.source.dc << ' '
               << component.source.amplitude << ' '
               << component.source.frequency << ' '
               << component.source.rise << ' '
               << component.source.pulseWidth << ' '
               << component.source.period << ' '
               << (component.switchClosed ? 1 : 0) << ' '
               << component.ratedVoltage << ' '
               << (component.damaged ? 1 : 0) << ' '
               << component.source.duty << ' '
               << component.seriesR << ' '
               << component.minVoltage << ' '
               << component.maxVoltage << ' '
               << component.maxCurrent << ' '
               << (component.damagedByCurrent ? 1 : 0) << ' '
               << component.ratedCurrent << ' '
               << component.ratedPower << ' '
               << component.heatLoad << ' '
               << component.coldStart << '\n';
    }
    for (const auto& wire : circuit.wires) {
        output << "WIRE " << wire.id << ' ' << wire.points.size();
        for (const auto& point : wire.points) {
            output << ' ' << point.x << ' ' << point.y;
        }
        output << '\n';
    }
    output.close();
    if (!output) {
        if (error) {
            *error = "写入工程文件失败。";
        }
        std::remove(temporary.c_str());
        return false;
    }

    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        if (error) {
            *error = "无法替换工程文件：" + path;
        }
        std::remove(temporary.c_str());
        return false;
    }
    if (error) {
        error->clear();
    }
    return true;
}

bool readNameOnly(const std::string& path, std::string* name)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    std::string line;
    if (!std::getline(input, line)) {
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    if (line != "CIRCUITLAB 1") {
        return false;
    }
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::istringstream stream(line);
        std::string tag;
        stream >> tag;
        if (tag == "NAME") {
            std::string rest;
            std::getline(stream, rest);
            if (!rest.empty() && rest.front() == ' ') {
                rest.erase(rest.begin());
            }
            name->assign(rest);
            return true;
        }
        if (tag == "COMP" || tag == "WIRE") {
            return true;
        }
    }
    return true;
}

std::string stemOf(const std::string& filename)
{
    const size_t dot = filename.find_last_of('.');
    return dot == std::string::npos ? filename : filename.substr(0, dot);
}

}  // namespace

bool saveProject(const Circuit& circuit, const std::string& path, const std::string& name,
                 std::string* error)
{
    return writeProject(circuit, path, name, error);
}

bool loadProject(const std::string& path, Circuit* circuit, std::string* name,
                 std::string* error)
{
    if (!circuit) {
        if (error) {
            *error = "内部错误：电路指针为空。";
        }
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error) {
            *error = "无法打开工程文件：" + path;
        }
        return false;
    }

    std::string header;
    std::getline(input, header);
    if (!header.empty() && header.back() == '\r') {
        header.pop_back();
    }
    if (header != "CIRCUITLAB 1") {
        if (error) {
            *error = "工程文件版本不受支持。";
        }
        return false;
    }

    Circuit loaded;
    std::string loadedName;
    std::string line;
    int maxComponentId = 0;
    int maxWireId = 0;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::istringstream stream(line);
        std::string tag;
        stream >> tag;
        if (tag.empty() || tag[0] == '#') {
            continue;
        }
        if (tag == "NAME") {
            std::string rest;
            std::getline(stream, rest);
            if (!rest.empty() && rest.front() == ' ') {
                rest.erase(rest.begin());
            }
            loadedName = rest;
        } else if (tag == "COMP") {
            Component component;
            std::string typeKey;
            std::string waveform;
            stream >> component.id >> typeKey >> component.name >> component.gridX >>
                component.gridY >> component.rotation >> component.value >> waveform >>
                component.source.dc >> component.source.amplitude >>
                component.source.frequency >> component.source.rise >>
                component.source.pulseWidth >> component.source.period;
            if (!stream || !componentTypeFromKey(typeKey, &component.type)) {
                if (error) {
                    *error = "工程文件中的元件记录无效。";
                }
                return false;
            }
            int closed = 1;
            if (stream >> closed) {
                component.switchClosed = closed != 0;
            }
            double rated = 0.0;
            int damaged = 0;
            if (stream >> rated) {
                component.ratedVoltage = rated;
                if (stream >> damaged) {
                    component.damaged = damaged != 0;
                }
            }
            // Duty cycle and series resistance were appended later; older files
            // simply do not have them.
            double duty = 0.0;
            if (stream >> duty && duty > 0.0 && duty <= 1.0) {
                component.source.duty = duty;
                double period = component.source.period;
                if (!(period > 0.0) && component.source.frequency > 0.0) {
                    period = 1.0 / component.source.frequency;
                }
                if (period > 0.0) {
                    component.source.period = period;
                    component.source.pulseWidth = period * duty;
                }
            } else if (component.source.period > 0.0 &&
                       component.source.pulseWidth > 0.0) {
                component.source.duty = std::clamp(
                    component.source.pulseWidth / component.source.period, 0.005, 0.995);
            }
            double seriesR = 0.0;
            if (stream >> seriesR && seriesR >= 0.0) {
                component.seriesR = seriesR;
            }
            // Load ratings were appended later; older files fall back to the
            // defaults for the part.
            double minVoltage = 0.0;
            double maxVoltage = 0.0;
            double maxCurrent = 0.0;
            if (stream >> minVoltage) {
                component.minVoltage = minVoltage;
                if (stream >> maxVoltage) {
                    component.maxVoltage = maxVoltage;
                    if (stream >> maxCurrent) {
                        component.maxCurrent = maxCurrent;
                    }
                }
            }
            int damagedByCurrent = 0;
            if (stream >> damagedByCurrent) {
                component.damagedByCurrent = damagedByCurrent != 0;
            }
            // The load spec (rated current / power) and the Peltier state were
            // appended later; older files stop here and get the defaults.
            double ratedCurrent = 0.0;
            if (stream >> ratedCurrent) {
                component.ratedCurrent = ratedCurrent;
                double ratedPower = 0.0;
                if (stream >> ratedPower) {
                    component.ratedPower = ratedPower;
                }
                double heatLoad = 0.0;
                if (stream >> heatLoad) {
                    component.heatLoad = heatLoad;
                }
                double coldStart = 0.0;
                if (stream >> coldStart) {
                    component.coldStart = coldStart;
                }
            }
            applyLoadDefaults(component);
            if (isLoadType(component.type) && component.ratedVoltage <= 0.0) {
                component.ratedVoltage =
                    component.type == ComponentType::Fan ? 12.0 : 12.0;
            }
            if (!waveformFromKey(waveform, &component.source.mode)) {
                component.source.mode = Waveform::DC;
            }
            component.rotation = ((component.rotation % 360) + 360) % 360;
            maxComponentId = std::max(maxComponentId, component.id);
            loaded.components.push_back(component);
        } else if (tag == "WIRE") {
            Wire wire;
            size_t count = 0;
            stream >> wire.id >> count;
            if (!stream || count < 2 || count > 64) {
                if (error) {
                    *error = "工程文件中的导线记录无效。";
                }
                return false;
            }
            for (size_t i = 0; i < count; ++i) {
                Point point;
                stream >> point.x >> point.y;
                if (!stream) {
                    if (error) {
                        *error = "工程文件中的导线坐标无效。";
                    }
                    return false;
                }
                wire.points.push_back(point);
            }
            maxWireId = std::max(maxWireId, wire.id);
            loaded.wires.push_back(std::move(wire));
        }
    }

    loaded.nextComponentId = maxComponentId + 1;
    loaded.nextWireId = maxWireId + 1;
    loaded.revision = 1;
    *circuit = std::move(loaded);
    if (name) {
        *name = loadedName;
    }
    if (error) {
        error->clear();
    }
    return true;
}

bool saveCircuit(const Circuit& circuit, const std::string& path, std::string* error)
{
    return saveProject(circuit, path, std::string(), error);
}

bool loadCircuit(const std::string& path, Circuit* circuit, std::string* error)
{
    return loadProject(path, circuit, nullptr, error);
}

bool ensureProjectDirectory(const std::string& directory, std::string* error)
{
    if (directory.empty()) {
        if (error) {
            *error = "工程目录为空。";
        }
        return false;
    }
    if (mkdir(directory.c_str(), 0700) == 0) {
        if (error) {
            error->clear();
        }
        return true;
    }
    if (errno == EEXIST) {
        struct stat info;
        if (stat(directory.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
            if (error) {
                error->clear();
            }
            return true;
        }
    }
    if (error) {
        *error = "无法创建工程目录：" + directory + "（" + std::strerror(errno) + "）";
    }
    return false;
}

std::vector<ProjectInfo> listProjects(const std::string& directory)
{
    std::vector<ProjectInfo> projects;
    DIR* dir = opendir(directory.c_str());
    if (!dir) {
        return projects;
    }
    while (dirent* entry = readdir(dir)) {
        const std::string filename = entry->d_name;
        if (filename.size() < 5 ||
            filename.compare(filename.size() - 4, 4, ".ckt") != 0) {
            continue;
        }
        const std::string path = directory + "/" + filename;
        struct stat info;
        if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
            continue;
        }
        ProjectInfo project;
        project.path = path;
        project.modified = static_cast<long long>(info.st_mtime);
        if (!readNameOnly(path, &project.name) || cleanName(project.name).empty()) {
            project.name = stemOf(filename);
        }
        project.name = cleanName(project.name);
        projects.push_back(std::move(project));
    }
    closedir(dir);
    std::sort(projects.begin(), projects.end(), [](const ProjectInfo& a, const ProjectInfo& b) {
        if (a.modified != b.modified) {
            return a.modified > b.modified;
        }
        return a.name < b.name;
    });
    return projects;
}

bool deleteProjectFile(const std::string& path, std::string* error)
{
    if (std::remove(path.c_str()) != 0) {
        if (error) {
            *error = "无法删除工程文件：" + path;
        }
        return false;
    }
    if (error) {
        error->clear();
    }
    return true;
}

}  // namespace circuitlab
