#pragma once

#include <memory>

// Shared residual STFT engine of the adaptive spectral-gain modules (Tone Balance, Definition, Smoother; pure C++17).
// Owner: dsp-spectral (implemented with the contracts so the three modules can start at once; the owner may extend
// it, e.g. skip the forward FFT while idle). Implementation: SpectralEngine.cpp. Tests: tests/SpectralEngineTests.cpp,
// prefix "spec.".
//
//   output = input delayed by N  +  overlap-add of IFFT ((G - 1) X)          ("residual form")
// Frames of N samples every hop H = N / 4 (75 % overlap), sine (= sqrt periodic Hann) analysis and synthesis windows
// (window products sum to 2 over the hops: OLA scale 0.5). Both channels in one complex FFT (Fft::splitPacked /
// mergePacked). With every gain exactly 1 the correction is exactly 0, so a neutral module nulls bit-exactly against
// the input delayed by N; for G != 1 the result equals classic STFT filtering, and only the correction passes through
// the windows.
// Sizes: N = baseSize x k, k = 1 at <= 50 kHz, 2 at <= 100 kHz, 4 above (Tone Balance base 2048, Definition and
// Smoother base 1024). Latency = N, fixed per sample rate (never changes with parameters).
// Per hop the engine computes per-bin powers of both channels and asks the client for per-bin gains (linear, > 0);
// the client owns everything adaptive (band grid, detectors, ballistics, smoothing; at least 2 bins per band, release
// >= 2 hops toward unity, boosts slew-limited: the rules that keep musical noise away).
// Idle skip: when the client reports every gain exactly 1 for a hop the IFFT / OLA are skipped (the residual is 0).
// Silence: frames whose total power is below -150 dBFS are passed with Frame::silent = true (clients hold their
// state and return unity).
// Realtime: prepare() allocates; reset() / process() do not. Works across any block size (1 sample included).
namespace ps
{
class SpectralEngine
{
public:
    struct Frame
    {
        int numBins = 0;                  // N / 2 + 1
        double binHz = 0.0;               // fs / N
        int hop = 0;                      // H in samples
        // |X_c (k)|^2 of the windowed frame, scaled so a full-scale sine reads 1.0 at its bin (summed over the
        // window's main lobe), for c = 0, 1.
        const float* power[2] { nullptr, nullptr };
        // Bin gains to fill (linear), preset to 1.0 by the engine. gain[1] may equal gain[0] (see linked).
        float* gain[2] { nullptr, nullptr };
        bool linked = false;              // the caller asked for one gain curve for both channels: fill gain[0] only
        bool silent = false;              // total frame power below -150 dBFS
        // return true from computeGains when every gain is exactly 1 (the engine skips the synthesis)
    };

    struct Client
    {
        virtual ~Client() = default;
        // Called on the audio thread once per hop, inside process(). Allocation-free. Return true for "all unity".
        virtual bool computeGains (Frame&) noexcept = 0;
    };

    SpectralEngine();
    ~SpectralEngine();

    // baseFftSize: the size at <= 50 kHz (power of two, 256 .. 8192).
    void prepare (double sampleRate, int baseFftSize, int maxBlockSize);
    void reset() noexcept;

    void setClient (Client*) noexcept;                 // not owned; set before process
    // In place, 2 channels. linked: Stereo-linked gains (Frame::linked), else one gain curve per channel.
    void process (float* const* channels, int numSamples, bool linked) noexcept;

    int getFftSize() const noexcept;
    int getHopSize() const noexcept;
    int getLatencySamples() const noexcept;           // == getFftSize()
    double getBinHz() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps
