#pragma once

#include "circuit/Circuit.h"

#include <string>
#include <vector>

namespace circuitlab {

// Legacy single-file helpers (used by the core tests and older builds).
bool saveCircuit(const Circuit& circuit, const std::string& path, std::string* error = nullptr);
bool loadCircuit(const std::string& path, Circuit* circuit, std::string* error = nullptr);

// Project files add a "NAME <title>" header line and persist switch states.
bool saveProject(const Circuit& circuit, const std::string& path, const std::string& name,
                 std::string* error = nullptr);
// A damaged record (a corrupt wire or component line) no longer makes the whole
// project unopenable: the bad record is skipped, the rest loads, and `warning`
// says how many were dropped.
bool loadProject(const std::string& path, Circuit* circuit, std::string* name,
                 std::string* error = nullptr, std::string* warning = nullptr);

struct ProjectInfo {
    std::string path;
    std::string name;
    long long modified = 0;  // Unix time in seconds
};

// Creates `directory` if needed (single level; the parent must exist).
bool ensureProjectDirectory(const std::string& directory, std::string* error = nullptr);

// Lists *.ckt files in `directory`, newest first. Unreadable files are skipped.
std::vector<ProjectInfo> listProjects(const std::string& directory);

bool deleteProjectFile(const std::string& path, std::string* error = nullptr);

}  // namespace circuitlab
