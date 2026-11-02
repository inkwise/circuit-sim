#pragma once

#include "circuit/Circuit.h"

#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace circuitlab {

enum class AnalysisKind {
    OperatingPoint,
    Transient,
};

struct TransientSpec {
    double step = 10e-6;
    double stop = 5e-3;
};

struct SimulationRequest {
    Circuit circuit;
    AnalysisKind kind = AnalysisKind::OperatingPoint;
    TransientSpec transient;
    int revision = 0;
};

struct SimulationResult {
    bool ok = false;
    AnalysisKind kind = AnalysisKind::OperatingPoint;
    int revision = 0;
    std::string error;
    std::string log;
    std::string plotName;
    std::vector<double> time;                        // transient only
    std::map<int, std::vector<double>> nodeVoltage;  // node -> 1 (OP) or N (TRAN) values
    // Component name (lowercase, e.g. "r1") -> branch current samples. The sign
    // follows SPICE convention: positive current flows from pin A to pin B.
    std::map<std::string, std::vector<double>> deviceCurrent;
    // Component id -> currents for every pin in netlist order. Two-pin devices
    // derive pin 1 as -pin 0; multi-pin devices use the per-pin sense sources.
    std::map<int, std::vector<std::vector<double>>> pinCurrent;
    int nodeCount = 0;
};

struct EngineImpl;

class NgspiceEngine {
public:
    NgspiceEngine();
    ~NgspiceEngine();

    NgspiceEngine(const NgspiceEngine&) = delete;
    NgspiceEngine& operator=(const NgspiceEngine&) = delete;

    // Initializes the ngspice shared library once. Safe to call multiple times.
    bool initialize(std::string& error);

    // Runs one analysis on a worker thread. The caller owns the future and must
    // wait for it before destroying the engine.
    std::future<SimulationResult> runAsync(SimulationRequest request);
    bool busy() const;

private:
    SimulationResult run(const SimulationRequest& request);

    std::unique_ptr<EngineImpl> impl_;
};

}  // namespace circuitlab
