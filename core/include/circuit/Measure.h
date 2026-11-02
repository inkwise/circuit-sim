#pragma once

#include <vector>

namespace circuitlab {

// Steady-state measurement of one sampled waveform. It is what the
// oscilloscope probe reports: the DC average, how big the switching ripple is
// and how fast the waveform repeats.
struct WaveStats {
    bool ok = false;
    int samples = 0;
    // Statistics window: the tail of the transient, so the startup transient
    // does not hide the steady state.
    double timeStart = 0.0;
    double timeEnd = 0.0;
    double mean = 0.0;   // DC average over the statistics window
    double min = 0.0;
    double max = 0.0;
    double rms = 0.0;    // true RMS (DC + AC) over the statistics window
    double acRms = 0.0;  // RMS of the AC part only
    // Ripple window: the very end of the statistics window, one or more
    // switching periods wide.
    double ripple = 0.0;  // peak-to-peak voltage inside the ripple window
    double rippleTimeStart = 0.0;
    double rippleTimeEnd = 0.0;
    double frequency = 0.0;  // estimated from crossings of `mean`; 0 = unknown
};

// `time` and `values` must share a time base (transient result).
// settleFraction: fraction of the time axis skipped at the start
//                 (0.5 analyses the second half of the transient).
// rippleFraction: last fraction of the analysed window used for the ripple
//                 peak-to-peak and the frequency estimate (0.25 = last quarter).
WaveStats measureWave(const std::vector<double>& time,
                      const std::vector<double>& values,
                      double settleFraction = 0.5,
                      double rippleFraction = 0.25);

// Component-wise difference of two sampled waveforms, truncated to the shorter
// one. Used for a differential probe reading: v(CH1) - v(GND).
std::vector<double> subtractSamples(const std::vector<double>& a,
                                    const std::vector<double>& b);

}  // namespace circuitlab
