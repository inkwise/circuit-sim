#include "circuit/SpiceModels.h"

#include <cstdio>

namespace circuitlab {
namespace {

const char* kNpnModel =
    ".model QNPN NPN(IS=1E-14 BF=200 VAF=100 RB=10 RC=1 RE=0.1 "
    "CJE=4P CJC=2P TF=0.3N)";

const char* kPnpModel =
    ".model QPNP PNP(IS=1E-14 BF=150 VAF=80 RB=10 RC=1 RE=0.1 "
    "CJE=6P CJC=3P TF=0.5N)";

const char* kDiodeModel =
    ".model D1N4148 D(IS=2.682N N=1.836 RS=0.5664 CJO=4P M=0.3333 VJ=0.5 "
    "IBV=100U BV=100)";

const char* kLedModel =
    ".model DLED D(IS=1E-20 N=1.9 RS=10 CJO=50P EG=2.4)";

const char* kNmosModel =
    ".model M2N7000 NMOS(LEVEL=1 VTO=2.1 KP=0.15 RD=1 RS=1 CGS=50P CGD=10P)";

// Power switches used by the switching-regulator parts. Low threshold (logic
// level), low channel resistance and small but non-zero gate charge so a real
// gate resistor and PWM frequency produce a visible switching time.
// Gate charge is intentionally left out: LEVEL=1 has no writable cgs/cgd at
// model level (ngspice warns about it), and the app keeps the deck clean.
const char* kPowerNmosModel =
    ".model MPWR_N NMOS(LEVEL=1 VTO=1.6 KP=8 RD=0.02 RS=0.02 IS=1E-12)";

const char* kPowerPmosModel =
    ".model MPWR_P PMOS(LEVEL=1 VTO=-1.6 KP=5 RD=0.03 RS=0.03 IS=1E-12)";

// IRL540N: logic-level N-channel MOSFET, 100 V / 28 A, Rds(on) 44 mΩ at
// Vgs = 5 V. The LEVEL=1 model has no breakdown, capacitance or thermal data,
// so the parameters are fitted to the on-resistance and the gate threshold:
// Rds ~ 1 / (KP * (Vgs - VTO)) + RD + RS = 34 + 10 = 44 mΩ at 5 V.
const char* kIrl540NModel =
    ".model MIRL540N NMOS(LEVEL=1 VTO=1.8 KP=9.2 RD=0.005 RS=0.005 IS=1E-12)";

// SS34-class Schottky freewheel diode: about 0.4 V at 1 A.
const char* kSchottkyModel =
    ".model DSCH D(IS=1E-6 N=1.05 RS=0.02 CJO=200P BV=40 IBV=100U)";

const char* kOpAmpSubckt = R"SPICE(.subckt CL_OPAMP INP INN OUT VCC VEE
RINP INP 0 1G
RINN INN 0 1G
RVCC VCC 0 1G
RVEE VEE 0 1G
E1 MID 0 INP INN 100K
B1 OUTP 0 V = max(min(V(MID), V(VCC)-1.5), V(VEE)+1.5)
ROUT OUTP OUT 75
.ends CL_OPAMP)SPICE";

const char* kAndSubckt = R"SPICE(.subckt CL_GATE_AND A B Y
RA A 0 1G
RB B 0 1G
B1 YD 0 V = (V(A) > 2.5) ? ((V(B) > 2.5) ? 5 : 0) : 0
RO YD Y 100
.ends CL_GATE_AND)SPICE";

const char* kOrSubckt = R"SPICE(.subckt CL_GATE_OR A B Y
RA A 0 1G
RB B 0 1G
B1 YD 0 V = (V(A) > 2.5) ? 5 : ((V(B) > 2.5) ? 5 : 0)
RO YD Y 100
.ends CL_GATE_OR)SPICE";

const char* kNotSubckt = R"SPICE(.subckt CL_GATE_NOT A Y
RA A 0 1G
B1 YD 0 V = (V(A) > 2.5) ? 0 : 5
RO YD Y 100
.ends CL_GATE_NOT)SPICE";

const char* kRegulator7805Subckt = R"SPICE(.subckt CL_7805 IN GND OUT
RQ IN GND 1K
RG GND 0 1G
B1 OUTP IN V = min(5, max(0, V(IN)-2)) - V(IN)
RO OUTP OUT 0.1
ROB OUT GND 1G
.ends CL_7805)SPICE";

// Functional 555 timer. The latch state is held on QST; the soft uramp
// currents make the DC operating point well-defined without .ic/uic.
const char* kTimer555Subckt = R"SPICE(.subckt CL_555 GND TRIG OUT RESET CTRL THRES DISCH VCC
R1 VCC CTRL 5K
R2 CTRL NREF 5K
R3 NREF GND 5K
RTRIG TRIG 0 1G
RTHRES THRES 0 1G
RRESET RESET VCC 1G
RDIS DISCH 0 1G
RGND GND 0 1G
RQ VCC 0 2K
CQ QST 0 1N
RQST QST 0 1T
BSET SET 0 V = (V(TRIG) < V(NREF)) ? 5 : 0
BRST RST 0 V = (V(THRES) > V(CTRL)) ? 5 : ((V(RESET) < 2.5) ? 5 : 0)
BQ QST 0 I = (V(RST) > 2.5) ? 1M*uramp(V(QST)-1) : ((V(SET) > 2.5) ? -1M*uramp(4-V(QST)) : 0)
BQB QBAR 0 V = (V(QST) > 2.5) ? 0 : 5
SDIS DISCH 0 QBAR 0 SW555
BOUT NOUT VCC V = (V(QST) > 2.5) ? -1.7 : -V(VCC)
ROUT NOUT OUT 10
.ends CL_555
.model SW555 SW(VT=2.5 VH=0.2 RON=10 ROFF=1G))SPICE";

}  // namespace

std::string spiceModelCard(const std::string& modelName)
{
    if (modelName == "QNPN") {
        return kNpnModel;
    }
    if (modelName == "QPNP") {
        return kPnpModel;
    }
    if (modelName == "D1N4148") {
        return kDiodeModel;
    }
    if (modelName == "DLED") {
        return kLedModel;
    }
    if (modelName == "NMOS" || modelName == "M2N7000") {
        return kNmosModel;
    }
    if (modelName == "MPWR_N") {
        return kPowerNmosModel;
    }
    if (modelName == "MPWR_P") {
        return kPowerPmosModel;
    }
    if (modelName == "DSCH") {
        return kSchottkyModel;
    }
    if (modelName == "MIRL540N") {
        return kIrl540NModel;
    }
    return {};
}

std::string zenerModelCard(const std::string& instanceName, double breakdownVoltage)
{
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  ".model %s_MOD D(IS=1E-14 N=1.5 RS=5 BV=%.6g IBV=20M)",
                  instanceName.c_str(), breakdownVoltage);
    return buffer;
}

std::string spiceSubcircuitCard(const std::string& subcircuitName)
{
    if (subcircuitName == "CL_OPAMP") {
        return kOpAmpSubckt;
    }
    if (subcircuitName == "CL_GATE_AND") {
        return kAndSubckt;
    }
    if (subcircuitName == "CL_GATE_OR") {
        return kOrSubckt;
    }
    if (subcircuitName == "CL_GATE_NOT") {
        return kNotSubckt;
    }
    if (subcircuitName == "CL_7805") {
        return kRegulator7805Subckt;
    }
    if (subcircuitName == "CL_555") {
        return kTimer555Subckt;
    }
    return {};
}

}  // namespace circuitlab
