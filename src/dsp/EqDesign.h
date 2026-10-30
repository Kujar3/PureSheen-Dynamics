#pragma once

#include "dsp/Filters.h"
#include "dsp/Params.h"

#include <complex>
#include <vector>

// Internal to the Equalizer (owner: dsp-eq). Band designs, the state-variable realisation of the biquads, the
// Digital-mode prototypes and the linear-phase kernel designer. Implementation: EqDesign.cpp. See Equalizer.cpp for
// the behaviour of every shape.
namespace ps::eqdesign
{
constexpr int kMaxSections = 4;   // per band (Brickwall = 4 elliptic biquads, 48 dB/oct = 4 Butterworth biquads)

//==============================================================================
// Analog-mode realisation of one band: a cascade of biquads (double precision).
struct BandDesign
{
    int numSections = 0;
    BiquadCoeffs s[kMaxSections];

    bool isIdentity() const noexcept;

    // z1 = e^{-jw}
    std::complex<double> response (std::complex<double> z1) const noexcept;

    // Magnitude and continuous (unwrapped) phase. Every section is minimum phase (or has its zeros on the unit
    // circle), so summing the principal phases of each numerator and denominator gives a continuous phase.
    void magPhase (std::complex<double> z1, double& mag, double& phase) const noexcept;
};

// Shape actually realised in Analog mode (Surgical -> Bell).
EqShape analogShape (EqShape) noexcept;
bool isPassShape (EqShape) noexcept;      // HP / LP (Flat, Resonant, Brickwall): gain not used
bool usesSlope (EqShape) noexcept;        // HP / LP Flat and Resonant
int sectionCount (EqShape analog, EqSlope) noexcept;

// Band gain after Amount: gain * amount / 100, clamped to -30..+15 dB.
double scaledGainDb (double gainDb, double amountPercent) noexcept;

// Q actually used by Proportional Q for a given (scaled) gain.
double proportionalQ (double q, double gainDb) noexcept;

// Designs the Analog-mode cascade. gainDb is the scaled gain. Cost: ~1.5..20 us per section (matched designs).
void designAnalog (EqShape, EqSlope, double sampleRate, double hz, double gainDb, double q, BandDesign&) noexcept;

//==============================================================================
// Digital mode only: Surgical and Brickwall are not realised with biquads but with sharper analog prototypes,
// evaluated directly (s = j f / f0, no frequency warping). Magnitude + continuous phase of the prototype.
bool hasDigitalPrototype (EqShape) noexcept;
void digitalPrototype (EqShape, double hz, double gainDb, double q, double freqHz, double& mag,
                       double& phase, bool wantPhase = true) noexcept;

//==============================================================================
// Topology-preserving (trapezoidal) state-variable filter equivalent to a biquad: the poles are given by (g, k) and
// the zeros by the output mix (m0 input, m1 band pass, m2 low pass). Every stable biquad maps to g > 0, k > 0; the
// structure stays well behaved when (g, k) are modulated, unlike a direct form whose raw coefficients are
// interpolated.
struct Svf
{
    double g = 1.0, k = 2.0, m0 = 1.0, m1 = 0.0, m2 = 0.0;

    bool isIdentity() const noexcept { return m0 == 1.0 && m1 == 0.0 && m2 == 0.0; }
};

Svf toSvf (const BiquadCoeffs&) noexcept;   // equal numerator/denominator gives an exact unity mix and retains poles

//==============================================================================
// Digital mode sizes. FIR length N = (4096 << firSizeIndex) * (1, 2 or 4 by sample-rate family), kernel centre
// D = N / 2, convolver partition P = N / 16. Latency = D + P.
int firLength (int firSizeIndex, double sampleRate) noexcept;
int firPartition (int firLength) noexcept;
int firLatency (int firSizeIndex, double sampleRate) noexcept;

// Designs the residual kernel r = h - delta(D) of one channel set (worker thread). h is the windowed (Kaiser) inverse
// FFT of  prod_b |H_b(w)| * exp (j sum_b p_b phi_b(w)) * exp (-j w D)  on a grid of 2N bins. A set whose bands are
// all flat yields r = 0 exactly.
class KernelDesigner
{
public:
    void prepare (double sampleRate, int maxFirLength);   // allocates

    // bands: kEqBands bands of one set. out: firLength floats. Returns false when the set is flat (out zeroed).
    bool design (const EqBandParams* bands, float amountPercent, int firSizeIndex, float* out) noexcept;

private:
    void prepareSize (int firLength) noexcept;   // tables for one size (inside the preallocated buffers)

    double sr = 48000.0;
    int curN = 0;
    std::vector<std::complex<double>> spec, twiddle, zTable;
    std::vector<int> bitrev;
    std::vector<double> logMag, phase, window;
};

} // namespace ps::eqdesign
