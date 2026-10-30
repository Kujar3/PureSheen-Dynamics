#include "dsp/Chain.h"

#include "core/Loudness.h"
#include "dsp/Dynamics.h"
#include "dsp/Equalizer.h"
#include "dsp/Exciter.h"
#include "dsp/Imager.h"
#include "dsp/Maximizer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>

// The chain host. Choices beyond the header (reference-suite parity where the research documents it, otherwise what a mastering
// engineer would expect):
//
// - Every state change that would click is crossfaded over 20 ms: module power / solo, channel on/off, channel solo,
//   delta, global bypass, Sum to Mono, Swap, DC filter, gain-match engage. Input / output gains ramp over 20 ms.
//   At neutral settings every stage is skipped or multiplies by exactly 1.0, so an empty / neutral chain is bit-exact.
// - Power off / solo-out: the slot fades to its latency-aligned dry signal and then stops calling Module::process.
//   Powering back on reset()s the module, lets it run silently for its latency (so a lookahead / FIR module has valid
//   output) and then fades it in. Module::reset() is therefore called on the audio thread (also for a module that
//   joins the chain) and must be realtime safe.
// - Delta: every slot's change (processed - aligned dry, or the module's own delta, written to
//   ProcessContext::deltaOut, when handlesDelta(); its normal output stays on the normal path and feeds the modules
//   after it) runs continuously through a per-slot delay line set to the latency of the slots after it, so it is
//   aligned to the chain output and has real signal the moment Delta is switched on, whatever the downstream latency.
//   The output crossfades (20 ms) between the normal signal and the sum of the gated deltas of the slots whose Delta
//   is on. So "Delta" plays exactly what that module changes, and is not re-processed by the modules after it (a
//   Maximizer after an EQ would otherwise boost the EQ's delta). Several deltas at once sum; switching from one to
//   another crossfades; on release the gates hold while the output fades back to normal.
// - Channel-mode change (e.g. Stereo -> M/S): the module's buffered audio belongs to the old domain, so the slot fades
//   to its aligned dry signal in the old mode, then switches, reset()s the module, lets it run silently for its
//   latency and fades it in (the power-cycle path): ~40 ms + latency of dry signal, never reinterpreted audio.
// - Channel solo (M/S, L/R modes): the other channel of the module's domain is muted (Mid solo = mono Mid on both
//   speakers; Side solo = Side, -Side; Left solo = left speaker only). Applied at the slot output, so downstream
//   modules process what is heard. Channel on/off and channel solo are ignored in Stereo mode.
//   Transient/Sustain is treated as Stereo (phase 2).
// - Gain match measures the chain input before input gain, latency-aligned (it is the bypass signal), and the chain
//   output after output gain / mono / swap, before bypass and the match gain itself (open loop: no feedback). Both
//   measurements (and so the match gain) are frozen while a monitoring function is active (module solo, delta, channel
//   solo, band solo, EQ audition, detector solo): the monitored audio says nothing about the processed loudness.
//   Smoothing: one-pole, 0.35 s time constant (settles in ~1 s). Classic mode also levels the dry signal heard while a
//   powered-off module is soloed, by that module's own loudness change (not the whole chain's): every slot measures
//   the K-weighted loudness of its aligned dry input and its output (mean squares, ~1 s) while it processes at full
//   wet with no monitor active, and keeps the last estimate while powered off.
// - Global bypass = the raw input (before input gain) delayed by the chain latency; the chain keeps running.
// - Difference meter: RMS of the slot output vs the aligned dry input, measured in the module's channel domain,
//   before channel solo; one-pole 300 ms on the mean squares; 0 dB for silence.
// - True-peak meters: a continuous 4x interpolator (same design as core/TruePeak) that reports a per-block maximum,
//   since core's TruePeakMeter only offers a max since reset.
// - Delay lines: per slot, dry max(0.75 s, 16384) and delta max(1.3 s, 32768) samples (it holds the latency of all
//   the slots after it), rounded up to a power of two (Digital EQ at 192 kHz, ~70k samples, fits); bypass line
//   max(2 s, 65536). A latency beyond that is clamped (alignment then breaks, the reported latency stays exact).
// - Chain order and monitor requests are handed to the audio thread through seqlocks of atomics (the reader never
//   waits: if it races a writer it keeps the previous value and picks the new one up next block).
namespace ps
{
namespace
{
constexpr double kLn10Over20 = 0.11512925464970228;   // ln(10) / 20

int nextPowerOfTwo (int v)
{
    int p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

// a + t * (b - a) with exact end points (t == 0 -> a, t == 1 -> b).
inline float mixExact (float a, float b, float t) noexcept
{
    return t == 0.0f ? a : (t == 1.0f ? b : a + t * (b - a));
}

//==============================================================================
struct Ramp
{
    float value = 1.0f, target = 1.0f, step = 0.0f;
    int remaining = 0, length = 960;

    void setLength (int n) noexcept { length = std::max (1, n); }
    void snap (float v) noexcept { value = target = v; remaining = 0; step = 0.0f; }

    void setTarget (float t) noexcept
    {
        if (t == target)
            return;
        target = t;
        remaining = length;
        step = (target - value) / (float) length;
    }

    float next() noexcept
    {
        if (remaining > 0)
        {
            value += step;
            if (--remaining == 0)
                value = target;
        }
        return value;
    }

    bool isAt (float v) const noexcept { return remaining == 0 && value == v; }
};

//==============================================================================
struct StereoDelay
{
    std::vector<float> buffer[2];
    int mask = 0, writePos = 0, delay = 0;

    void prepare (int capacity)
    {
        const int size = nextPowerOfTwo (std::max (2, capacity));
        buffer[0].assign ((size_t) size, 0.0f);
        buffer[1].assign ((size_t) size, 0.0f);
        mask = size - 1;
        writePos = 0;
        delay = std::min (delay, mask);
    }

    int maxDelay() const noexcept { return mask; }
    void setDelay (int d) noexcept { delay = std::clamp (d, 0, mask); }

    void clear() noexcept
    {
        for (auto& b : buffer)
            std::fill (b.begin(), b.end(), 0.0f);
    }

    // in and out may alias.
    void process (const float* in0, const float* in1, float* out0, float* out1, int n) noexcept
    {
        if (mask == 0)
        {
            if (out0 != in0) std::memmove (out0, in0, sizeof (float) * (size_t) n);
            if (out1 != in1) std::memmove (out1, in1, sizeof (float) * (size_t) n);
            return;
        }

        float* b0 = buffer[0].data();
        float* b1 = buffer[1].data();
        int w = writePos;
        for (int i = 0; i < n; ++i)
        {
            b0[w] = in0[i];
            b1[w] = in1[i];
            const int r = (w - delay) & mask;
            out0[i] = b0[r];
            out1[i] = b1[r];
            w = (w + 1) & mask;
        }
        writePos = w;
    }
};

//==============================================================================
// Per-block true peak (BS.1770 style 4x interpolation: the same 64-tap Kaiser (beta 7) windowed-sinc design as
// core/TruePeak, which only offers a max-since-reset). Runs continuously, so block boundaries are seamless.
struct BlockTruePeak
{
    static constexpr int kPhases = 4, kTaps = 16;
    float phases[kPhases][kTaps] {};
    float history[2][2 * kTaps] {};   // doubled ring: history[c][pos .. pos + kTaps - 1] is contiguous
    int pos = 0;

    BlockTruePeak()
    {
        constexpr int numTaps = kPhases * kTaps;
        constexpr double beta = 7.0;
        const double centre = 0.5 * (numTaps - 1);
        auto besselI0 = [] (double x)
        {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 64; ++k)
            {
                const double f = 0.5 * x / k;
                term *= f * f;
                sum += term;
                if (term < 1.0e-14 * sum)
                    break;
            }
            return sum;
        };
        double h[numTaps] {};
        for (int n = 0; n < numTaps; ++n)
        {
            const double x = (n - centre) / kPhases;
            const double sinc = std::abs (x) < 1.0e-12 ? 1.0 : std::sin (3.14159265358979323846 * x) / (3.14159265358979323846 * x);
            const double r = (n - centre) / centre;
            h[n] = sinc * besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / besselI0 (beta);
        }
        for (int p = 0; p < kPhases; ++p)
        {
            double sum = 0.0;
            for (int k = 0; k < kTaps; ++k)
                sum += h[p + k * kPhases];
            for (int k = 0; k < kTaps; ++k)
                phases[p][k] = (float) (h[p + k * kPhases] / sum);
        }
    }

    void clear() noexcept
    {
        for (auto& c : history)
            std::fill (std::begin (c), std::end (c), 0.0f);
        pos = 0;
    }

    // Linear true peak (max over both channels, including the samples themselves) of this block.
    float process (const float* l, const float* r, int n) noexcept
    {
        float peak = 0.0f;
        const float* src[2] { l, r };
        int p0 = pos;
        for (int c = 0; c < 2; ++c)
        {
            float* hist = history[c];
            int p = p0;
            for (int i = 0; i < n; ++i)
            {
                const float x = src[c][i];
                p = (p + kTaps - 1) % kTaps;   // newest at hist[p]
                hist[p] = hist[p + kTaps] = x;
                peak = std::max (peak, std::abs (x));
                const float* w = hist + p;
                for (int ph = 0; ph < kPhases; ++ph)
                {
                    float acc = 0.0f;
                    for (int k = 0; k < kTaps; ++k)
                        acc += phases[ph][k] * w[k];
                    peak = std::max (peak, std::abs (acc));
                }
            }
            pos = p;
        }
        return peak;
    }
};

//==============================================================================
// Seqlock of atomics for one ModuleMonitor (writer: message thread, reader: audio thread, never waits).
struct MonitorBox
{
    std::atomic<unsigned> seq { 0 };
    std::atomic<int> soloBand { -1 }, soloChannelSet { -1 }, auditionChannelSet { 0 };
    std::atomic<bool> auditionOn { false }, soloDetectionFilter { false };
    std::atomic<float> auditionHz { 1000.0f }, auditionQ { 4.0f };

    void write (const ModuleMonitor& m) noexcept
    {
        const unsigned s = seq.load (std::memory_order_relaxed);
        seq.store (s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_release);
        soloBand.store (m.soloBand, std::memory_order_relaxed);
        soloChannelSet.store (m.soloChannelSet, std::memory_order_relaxed);
        auditionChannelSet.store (m.auditionChannelSet, std::memory_order_relaxed);
        auditionOn.store (m.auditionOn, std::memory_order_relaxed);
        soloDetectionFilter.store (m.soloDetectionFilter, std::memory_order_relaxed);
        auditionHz.store (m.auditionHz, std::memory_order_relaxed);
        auditionQ.store (m.auditionQ, std::memory_order_relaxed);
        seq.store (s + 2, std::memory_order_release);
    }

    // true when a new, consistent value was read.
    bool read (ModuleMonitor& m, unsigned& lastSeq) const noexcept
    {
        const unsigned s1 = seq.load (std::memory_order_acquire);
        if ((s1 & 1u) != 0 || s1 == lastSeq)
            return false;

        ModuleMonitor t;
        t.soloBand = soloBand.load (std::memory_order_relaxed);
        t.soloChannelSet = soloChannelSet.load (std::memory_order_relaxed);
        t.auditionChannelSet = auditionChannelSet.load (std::memory_order_relaxed);
        t.auditionOn = auditionOn.load (std::memory_order_relaxed);
        t.soloDetectionFilter = soloDetectionFilter.load (std::memory_order_relaxed);
        t.auditionHz = auditionHz.load (std::memory_order_relaxed);
        t.auditionQ = auditionQ.load (std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_acquire);
        if (seq.load (std::memory_order_relaxed) != s1)
            return false;

        m = t;
        lastSeq = s1;
        return true;
    }
};

// Seqlock of the chain order.
struct OrderBox
{
    std::atomic<unsigned> seq { 0 };
    std::atomic<int> count { 0 };
    std::atomic<int> types[kNumModuleTypes] {};

    void write (const std::vector<ModuleType>& order) noexcept
    {
        const unsigned s = seq.load (std::memory_order_relaxed);
        seq.store (s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_release);
        const int n = std::min ((int) order.size(), kNumModuleTypes);
        for (int i = 0; i < n; ++i)
            types[i].store ((int) order[(size_t) i], std::memory_order_relaxed);
        count.store (n, std::memory_order_relaxed);
        seq.store (s + 2, std::memory_order_release);
    }

    bool read (int* out, int& n, unsigned& lastSeq) const noexcept
    {
        const unsigned s1 = seq.load (std::memory_order_acquire);
        if ((s1 & 1u) != 0 || s1 == lastSeq)
            return false;

        int tmp[kNumModuleTypes] {};
        const int c = std::clamp (count.load (std::memory_order_relaxed), 0, kNumModuleTypes);
        for (int i = 0; i < c; ++i)
            tmp[i] = types[i].load (std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_acquire);
        if (seq.load (std::memory_order_relaxed) != s1)
            return false;

        for (int i = 0; i < c; ++i)
            out[i] = tmp[i];
        n = c;
        lastSeq = s1;
        return true;
    }
};

//==============================================================================
// 2nd-order Butterworth high-pass at 1 Hz (bilinear, double, TDF-II), stereo.
struct DcFilter
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1[2] {}, z2[2] {};

    void design (double sr) noexcept
    {
        const double k = std::tan (3.14159265358979323846 * 1.0 / sr);
        const double q = 0.70710678118654752;
        const double norm = 1.0 / (1.0 + k / q + k * k);
        b0 = norm;
        b1 = -2.0 * norm;
        b2 = norm;
        a1 = 2.0 * (k * k - 1.0) * norm;
        a2 = (1.0 - k / q + k * k) * norm;
    }

    void reset() noexcept { z1[0] = z1[1] = z2[0] = z2[1] = 0.0; }

    double process (int c, double x) noexcept
    {
        const double y = b0 * x + z1[c];
        z1[c] = b1 * x - a1 * y + z2[c];
        z2[c] = b2 * x - a2 * y;
        return y;
    }
};

//==============================================================================
// K-weighted loudness of a slot's aligned dry input and its output (classic gain match of a soloed, powered-off
// module). Mean squares smoothed over ~1 s; frozen while the slot is not measured.
struct SlotLoudness
{
    KBiquad shelf[2][2], highPass[2][2];   // [input / output][channel]
    double msIn = 0.0, msOut = 0.0;
    double matchDb = 0.0;                  // output - input (dB), last valid estimate

    void design (double sr)
    {
        for (int side = 0; side < 2; ++side)
            for (int c = 0; c < 2; ++c)
                designKWeighting (sr, shelf[side][c], highPass[side][c]);
    }

    void reset() noexcept
    {
        for (int side = 0; side < 2; ++side)
            for (int c = 0; c < 2; ++c)
            {
                shelf[side][c].reset();
                highPass[side][c].reset();
            }
        msIn = msOut = 0.0;
        matchDb = 0.0;
    }

    // K-weighted sum of squares of both channels
    double weigh (int side, const float* l, const float* r, int n) noexcept
    {
        double acc = 0.0;
        const float* src[2] { l, r };
        for (int c = 0; c < 2; ++c)
        {
            auto& s = shelf[side][c];
            auto& h = highPass[side][c];
            for (int i = 0; i < n; ++i)
            {
                const double y = h.process (s.process ((double) src[c][i]));
                acc += y * y;
            }
            if (std::abs (s.z1) < 1.0e-30) s.z1 = 0.0;
            if (std::abs (s.z2) < 1.0e-30) s.z2 = 0.0;
            if (std::abs (h.z1) < 1.0e-30) h.z1 = 0.0;
            if (std::abs (h.z2) < 1.0e-30) h.z2 = 0.0;
        }
        return acc;
    }

    void update (double eIn, double eOut, int n, double sampleRate) noexcept
    {
        const double mIn = eIn / (double) n, mOut = eOut / (double) n;
        if (mIn < 1.0e-7 || mOut < 1.0e-7)
            return;   // (near) silence on either side says nothing about the module's gain
        const double a = 1.0 - std::exp (-(double) n / (1.0 * sampleRate));
        msIn += a * (mIn - msIn);
        msOut += a * (mOut - msOut);
        if (msIn > 1.0e-6 && msOut > 1.0e-6)
            matchDb = std::clamp (10.0 * std::log10 (msOut / msIn), -24.0, 24.0);
    }
};

//==============================================================================
struct Slot
{
    StereoDelay dry, deltaLine;
    Ramp wet, channelOn[2], keep[2], deltaGain;
    bool processing = true;
    bool enabled = true;        // power on, not soloed out, no channel-mode change pending (this block)
    bool fresh = true;          // snap every ramp on the next block (prepare, reset, joined the chain)
    long long warmup = 0;
    int latency = 0;
    int downstream = 0;         // aligned latency of the slots after this one (delta line delay)
    double msIn = 0.0, msOut = 0.0;
    SlotParams params;
    ChannelMode activeMode = ChannelMode::Stereo;   // the domain the module currently processes in
    ModuleMonitor monitor;
    unsigned monitorSeq = 0;
    bool deltaRequested = false;
    bool monitorActive = false; // band solo / audition / detector solo: the module's output is a monitor signal
    SlotLoudness loudness;
};
} // namespace

//==============================================================================
struct Chain::Impl
{
    std::unique_ptr<Module> modules[kNumModuleTypes];
    Slot slots[kNumModuleTypes];
    MonitorBox monitorBoxes[kNumModuleTypes];
    OrderBox orderBox;

    // message thread
    std::vector<ModuleType> order { kDefaultChain.begin(), kDefaultChain.end() };

    // shared flags (message -> audio)
    std::atomic<int> solo { -1 };
    std::atomic<bool> delta[kNumModuleTypes] {};
    std::atomic<int> channelSolo[kNumModuleTypes] {};
    std::atomic<int> tapModule { -1 };
    std::atomic<bool> resetIntegratedRequest { false };
    std::atomic<int> latency { 0 };

    // audio thread
    int audioOrder[kNumModuleTypes] {};
    int audioCount = 0;
    unsigned orderSeqSeen = ~0u;
    bool audioInChain[kNumModuleTypes] {};
    bool monitoringActive = false;
    bool firstBlock = true;
    IoParams io;
    DitherParams ditherParams;

    std::vector<float> raw[2], cur[2], dry[2], bus[2], tmp[2];
    StereoDelay bypassDelay;
    DcFilter dcFilter;
    Ramp inGain[2], outGain[2], mono, swap, bypass, deltaMix, dcAmount, classicAmount, modernAmount, soloClassicAmount;
    double matchDb = 0.0, matchTargetDb = 0.0, matchCoeff = 0.0;
    double soloMatchDb = 0.0;   // classic: level of the soloed, powered-off slot's dry signal (that slot's own gain)

    LoudnessMeter inputMs, inputIntegrated, outputMs, outputIntegrated, matchInput, matchOutput;
    BlockTruePeak inputTp, outputTp;
    Dither dither;
    IoMeters ioMeters;
    StereoSampleFifo inputFifo, outputFifo, modulePreFifo, modulePostFifo;

    double sampleRate = 48000.0;
    int maxBlock = 0;
    bool prepared = false;

    Impl()
    {
        modules[(int) ModuleType::Equalizer1] = std::make_unique<Equalizer> (ModuleType::Equalizer1);
        modules[(int) ModuleType::Equalizer2] = std::make_unique<Equalizer> (ModuleType::Equalizer2);
        modules[(int) ModuleType::Dynamics] = std::make_unique<Dynamics>();
        modules[(int) ModuleType::Imager] = std::make_unique<Imager>();
        modules[(int) ModuleType::Exciter] = std::make_unique<Exciter>();
        modules[(int) ModuleType::Maximizer] = std::make_unique<Maximizer>();
        for (auto& c : channelSolo)
            c.store (-1);
        orderBox.write (order);
    }

    //==============================================================================
    void allRampLengths (int n) noexcept
    {
        for (auto* r : { &inGain[0], &inGain[1], &outGain[0], &outGain[1], &mono, &swap, &bypass, &deltaMix, &dcAmount,
                         &classicAmount, &modernAmount, &soloClassicAmount })
            r->setLength (n);
        for (auto& s : slots)
            for (auto* r : { &s.wet, &s.channelOn[0], &s.channelOn[1], &s.keep[0], &s.keep[1], &s.deltaGain })
                r->setLength (n);
    }

    void clearSlot (Slot& s) noexcept
    {
        s.dry.clear();
        s.deltaLine.clear();
        s.processing = true;
        s.fresh = true;
        s.warmup = 0;
        s.msIn = s.msOut = 0.0;
        s.loudness.reset();
    }

    void pullOrder() noexcept
    {
        int newOrder[kNumModuleTypes] {};
        int n = 0;
        if (! orderBox.read (newOrder, n, orderSeqSeen))
            return;

        bool nowIn[kNumModuleTypes] {};
        for (int i = 0; i < n; ++i)
            if (newOrder[i] >= 0 && newOrder[i] < kNumModuleTypes)
                nowIn[newOrder[i]] = true;

        for (int t = 0; t < kNumModuleTypes; ++t)
        {
            if (nowIn[t] && ! audioInChain[t])
            {
                modules[t]->reset();
                clearSlot (slots[t]);
            }
            if (! nowIn[t])
                ioMeters.moduleDifferenceDb[t].store (0.0f);
            audioInChain[t] = nowIn[t];
        }

        for (int i = 0; i < n; ++i)
            audioOrder[i] = newOrder[i];
        audioCount = n;
    }

    // Block-level state: params, latency, ramp targets. Called once per process() call.
    void updateBlockState (ParamValues values, int numSamples) noexcept
    {
        pullOrder();

        if (values != nullptr)
        {
            readIoParams (values, io);
            readDitherParams (values, ditherParams);
        }
        dither.setParams (ditherParams);

        const int soloType = solo.load (std::memory_order_relaxed);
        bool soloValid = false;
        bool anyDelta = false;
        bool monitoring = soloType >= 0;
        int totalLatency = 0, alignedLatency = 0;

        for (int t = 0; t < kNumModuleTypes; ++t)
        {
            auto& s = slots[t];
            if (s.monitorSeq != monitorBoxes[t].seq.load (std::memory_order_acquire)
                && monitorBoxes[t].read (s.monitor, s.monitorSeq))
                modules[t]->setMonitor (s.monitor);
        }

        for (int k = 0; k < audioCount; ++k)
        {
            auto& s = slots[audioOrder[k]];
            s.deltaRequested = delta[audioOrder[k]].load (std::memory_order_relaxed);
            anyDelta = anyDelta || s.deltaRequested;
        }
        const bool deltaIdle = deltaMix.isAt (0.0f) || firstBlock;

        for (int k = 0; k < audioCount; ++k)
        {
            const int t = audioOrder[k];
            auto& s = slots[t];
            auto& m = *modules[t];

            if (values != nullptr)
            {
                readSlotParams (values, (ModuleType) t, s.params);
                m.setParams (values);
            }

            const int lat = std::max (0, m.getLatencySamples());
            totalLatency += lat;
            s.latency = std::min (lat, s.dry.maxDelay());
            alignedLatency += s.latency;

            if (t == soloType)
                soloValid = true;

            const bool fresh = s.fresh || firstBlock;

            // Channel-mode change: the module's buffered state belongs to the old domain. The slot fades to its
            // aligned dry signal in the old mode, stops, switches, reset()s the module and warms it up silently for
            // its latency, then fades the module in again (the power-cycle path).
            if (fresh || ! s.processing)
                s.activeMode = s.params.channelMode;
            const bool modePending = s.activeMode != s.params.channelMode;

            const ChannelMode mode = s.activeMode;
            const bool dual = mode == ChannelMode::MidSide || mode == ChannelMode::LeftRight;
            const int chSolo = dual ? channelSolo[t].load (std::memory_order_relaxed) : -1;
            s.enabled = s.params.power && (soloType < 0 || soloType == t) && ! modePending;
            s.monitorActive = s.monitor.soloBand >= 0 || s.monitor.auditionOn || s.monitor.soloDetectionFilter;

            monitoring = monitoring || s.deltaRequested || chSolo >= 0 || s.monitorActive;

            float onTarget[2], keepTarget[2];
            for (int c = 0; c < 2; ++c)
            {
                onTarget[c] = (! dual || s.params.channelOn[c]) ? 1.0f : 0.0f;
                keepTarget[c] = (chSolo < 0 || chSolo == c) ? 1.0f : 0.0f;
            }
            const float deltaTarget = s.deltaRequested ? 1.0f : 0.0f;

            if (fresh)
            {
                s.processing = s.enabled;
                s.warmup = 0;
                s.wet.snap (s.enabled ? 1.0f : 0.0f);
                for (int c = 0; c < 2; ++c)
                {
                    s.channelOn[c].snap (onTarget[c]);
                    s.keep[c].snap (keepTarget[c]);
                }
                s.deltaGain.snap (deltaTarget);
                s.fresh = false;
                continue;
            }

            for (int c = 0; c < 2; ++c)
            {
                if (s.processing)
                {
                    s.channelOn[c].setTarget (onTarget[c]);
                    s.keep[c].setTarget (keepTarget[c]);
                }
                else
                {
                    s.channelOn[c].snap (onTarget[c]);   // (only heard through the module)
                    s.keep[c].setTarget (keepTarget[c]);
                }
            }

            // Delta gates (applied where the slot's latency-aligned delta joins the delta sum). While the output is
            // fully normal they follow the requests at once; while Delta is heard they crossfade; when the last
            // Delta is released they hold, so the delta sum keeps its signal while the output fades back to normal.
            if (deltaIdle)
                s.deltaGain.snap (deltaTarget);
            else if (anyDelta)
                s.deltaGain.setTarget (deltaTarget);

            if (s.enabled && ! s.processing)
            {
                m.reset();
                s.processing = true;
                s.warmup = lat;
                s.wet.snap (0.0f);
            }

            if (s.processing && s.enabled)
            {
                if (s.warmup > 0)
                    s.warmup -= numSamples;   // runs silently (wet 0) for its latency, fades in next block
                else
                    s.wet.setTarget (1.0f);
            }
            else if (s.processing)
            {
                s.wet.setTarget (0.0f);
            }
        }

        // delta lines: each slot's change is delayed by the latency of the slots after it (chain-output aligned)
        int downstream = 0;
        for (int k = audioCount - 1; k >= 0; --k)
        {
            auto& s = slots[audioOrder[k]];
            s.downstream = downstream;
            downstream += s.latency;
        }

        latency.store (totalLatency, std::memory_order_relaxed);
        bypassDelay.setDelay (alignedLatency);
        monitoringActive = monitoring;

        const bool classic = io.gainMatch && ! io.gainMatchModern;
        const bool modern = io.gainMatch && io.gainMatchModern;
        const bool soloOfPoweredOff = classic && soloValid && ! slots[soloType].params.power;
        if (soloValid)
            soloMatchDb = slots[soloType].loudness.matchDb;
        const bool maximizerInChain = audioInChain[(int) ModuleType::Maximizer];

        const float targets[] = {
            (float) std::pow (10.0, io.inputGainDb[0] / 20.0), (float) std::pow (10.0, io.inputGainDb[1] / 20.0),
            (float) std::pow (10.0, io.outputGainDb[0] / 20.0), (float) std::pow (10.0, io.outputGainDb[1] / 20.0),
            io.sumToMono ? 1.0f : 0.0f, io.swapChannels ? 1.0f : 0.0f, io.bypass ? 1.0f : 0.0f,
            anyDelta ? 1.0f : 0.0f, (ditherParams.dcFilter && maximizerInChain) ? 1.0f : 0.0f,
            classic ? 1.0f : 0.0f, modern ? 1.0f : 0.0f, soloOfPoweredOff ? 1.0f : 0.0f
        };
        Ramp* ramps[] = { &inGain[0], &inGain[1], &outGain[0], &outGain[1], &mono, &swap, &bypass, &deltaMix, &dcAmount,
                          &classicAmount, &modernAmount, &soloClassicAmount };

        if (dcAmount.isAt (0.0f) && targets[8] > 0.0f)
            dcFilter.reset();

        for (size_t i = 0; i < std::size (ramps); ++i)
        {
            if (firstBlock)
                ramps[i]->snap (targets[i]);
            else
                ramps[i]->setTarget (targets[i]);
        }

        if (resetIntegratedRequest.exchange (false, std::memory_order_relaxed))
        {
            inputIntegrated.reset();
            outputIntegrated.reset();
        }

        firstBlock = false;
    }

    void finishBlockState() noexcept
    {
        for (int k = 0; k < audioCount; ++k)
        {
            auto& s = slots[audioOrder[k]];
            if (s.processing && s.wet.isAt (0.0f) && s.warmup <= 0 && ! s.enabled)
                s.processing = false;
        }
    }

    //==============================================================================
    static void encodeMs (float* a, float* b, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const float l = a[i], r = b[i];
            a[i] = 0.5f * (l + r);
            b[i] = 0.5f * (l - r);
        }
    }

    static void decodeMs (float* a, float* b, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const float m = a[i], s = b[i];
            a[i] = m + s;
            b[i] = m - s;
        }
    }

    void processSlot (int t, int n, const ProcessContext& baseCtx) noexcept
    {
        auto& s = slots[t];
        auto& m = *modules[t];
        float* x[2] { cur[0].data(), cur[1].data() };
        float* d[2] { dry[0].data(), dry[1].data() };
        float* dt[2] { tmp[0].data(), tmp[1].data() };
        const bool tapped = tapModule.load (std::memory_order_relaxed) == t;

        if (tapped)
            modulePreFifo.push (x[0], x[1], n);

        s.dry.setDelay (s.latency);
        s.dry.process (x[0], x[1], d[0], d[1], n);

        const ChannelMode mode = s.activeMode;
        const bool dual = mode == ChannelMode::MidSide || mode == ChannelMode::LeftRight;
        const bool ms = mode == ChannelMode::MidSide;
        const bool keepActive = dual && (! s.keep[0].isAt (1.0f) || ! s.keep[1].isAt (1.0f));
        const bool needDomain = ms && (s.processing || keepActive);
        const bool handles = m.handlesDelta();

        // loudness of the module's own effect (L/R, aligned dry vs output), for classic gain match while soloed
        const bool measure = s.processing && s.params.power && s.wet.isAt (1.0f) && ! keepActive && ! s.monitorActive;
        const double eIn = measure ? s.loudness.weigh (0, d[0], d[1], n) : 0.0;

        if (needDomain)
        {
            encodeMs (x[0], x[1], n);
            encodeMs (d[0], d[1], n);
        }

        if (s.processing)
        {
            ProcessContext ctx = baseCtx;
            ctx.channelMode = mode == ChannelMode::TransientSustain ? ChannelMode::Stereo : mode;
            ctx.delta = s.deltaRequested;
            if (handles)
            {
                // the module writes its own delta here and keeps its normal output in x (Module.h); always passed,
                // so the delta line is warm whenever Delta is switched on
                std::fill (dt[0], dt[0] + n, 0.0f);
                std::fill (dt[1], dt[1] + n, 0.0f);
                ctx.deltaOut = dt;
            }
            m.process (x, n, ctx);

            for (int c = 0; c < 2; ++c)
            {
                if (! dual || s.channelOn[c].isAt (1.0f))
                    continue;
                for (int i = 0; i < n; ++i)
                {
                    const float g = s.channelOn[c].next();
                    x[c][i] = mixExact (d[c][i], x[c][i], g);
                    if (handles)
                        dt[c][i] *= g;
                }
            }

            if (! s.wet.isAt (1.0f))
            {
                for (int i = 0; i < n; ++i)
                {
                    const float w = s.wet.next();
                    for (int c = 0; c < 2; ++c)
                    {
                        x[c][i] = mixExact (d[c][i], x[c][i], w);
                        if (handles)
                            dt[c][i] *= w;
                    }
                }
            }

            if (! handles)
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < n; ++i)
                        dt[c][i] = x[c][i] - d[c][i];
        }
        else
        {
            std::memcpy (x[0], d[0], sizeof (float) * (size_t) n);
            std::memcpy (x[1], d[1], sizeof (float) * (size_t) n);
            std::fill (dt[0], dt[0] + n, 0.0f);
            std::fill (dt[1], dt[1] + n, 0.0f);
        }

        // difference meter (module domain, before channel solo)
        {
            double eDry = 0.0, eOut = 0.0;
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i)
                {
                    eDry += (double) d[c][i] * (double) d[c][i];
                    eOut += (double) x[c][i] * (double) x[c][i];
                }
            const double a = 1.0 - std::exp (-(double) n / (0.3 * sampleRate));
            s.msIn += a * (eDry / (2.0 * n) - s.msIn);
            s.msOut += a * (eOut / (2.0 * n) - s.msOut);
            const float diff = (s.msIn > 1.0e-14 && s.msOut > 1.0e-14) ? (float) (10.0 * std::log10 (s.msOut / s.msIn))
                                                                       : 0.0f;
            ioMeters.moduleDifferenceDb[t].store (diff);
        }

        // channel solo (normal output and delta alike)
        if (keepActive)
        {
            for (int i = 0; i < n; ++i)
            {
                for (int c = 0; c < 2; ++c)
                {
                    const float k = s.keep[c].next();
                    x[c][i] *= k;
                    dt[c][i] *= k;
                }
            }
        }

        if (needDomain)
        {
            decodeMs (x[0], x[1], n);
            decodeMs (dt[0], dt[1], n);
        }

        if (measure)
            s.loudness.update (eIn, s.loudness.weigh (1, x[0], x[1], n), n, sampleRate);

        // This slot's delta, delayed by the latency of the slots after it: chain-output aligned. The line always
        // runs, so switching Delta on plays real signal at once whatever the downstream latency.
        s.deltaLine.setDelay (s.downstream);
        s.deltaLine.process (dt[0], dt[1], dt[0], dt[1], n);
        if (! s.deltaGain.isAt (0.0f))
        {
            float* b[2] { bus[0].data(), bus[1].data() };
            for (int i = 0; i < n; ++i)
            {
                const float g = s.deltaGain.next();
                b[0][i] += g * dt[0][i];
                b[1][i] += g * dt[1][i];
            }
        }

        if (tapped)
            modulePostFifo.push (x[0], x[1], n);
    }

    void applyDcFilter (int n) noexcept
    {
        if (dcAmount.isAt (0.0f))
            return;
        float* x[2] { cur[0].data(), cur[1].data() };
        for (int i = 0; i < n; ++i)
        {
            const float a = dcAmount.next();
            for (int c = 0; c < 2; ++c)
            {
                const double y = dcFilter.process (c, (double) x[c][i]);
                x[c][i] = a == 1.0f ? (float) y : (float) ((double) x[c][i] + (double) a * (y - (double) x[c][i]));
            }
        }
    }

    void meterPeaks (const float* l, const float* r, int n, AtomicFloat* peaks, AtomicFloat* clips) noexcept
    {
        const float* src[2] { l, r };
        for (int c = 0; c < 2; ++c)
        {
            float pk = 0.0f;
            for (int i = 0; i < n; ++i)
                pk = std::max (pk, std::abs (src[c][i]));
            peaks[c].storeMax (pk);
            if (pk > 1.0f)
                clips[c].store (1.0f);
        }
    }

    void processChunk (float* out0, float* out1, int n, const ProcessContext& ctx) noexcept
    {
        float* r[2] { raw[0].data(), raw[1].data() };
        float* x[2] { cur[0].data(), cur[1].data() };
        std::memcpy (r[0], out0, sizeof (float) * (size_t) n);
        std::memcpy (r[1], out1, sizeof (float) * (size_t) n);

        // 2. input gain + input taps
        for (int c = 0; c < 2; ++c)
        {
            if (inGain[c].isAt (1.0f))
                std::memcpy (x[c], r[c], sizeof (float) * (size_t) n);
            else
                for (int i = 0; i < n; ++i)
                    x[c][i] = r[c][i] * inGain[c].next();
        }

        meterPeaks (x[0], x[1], n, ioMeters.inputPeak, ioMeters.inputClip);
        ioMeters.inputTruePeak.storeMax (inputTp.process (x[0], x[1], n));
        inputMs.process (x[0], x[1], n);
        inputIntegrated.process (x[0], x[1], n);
        ioMeters.inputMomentaryLufs.store (inputMs.momentaryLufs());
        ioMeters.inputShortTermLufs.store (inputMs.shortTermLufs());
        ioMeters.inputIntegratedLufs.store (inputIntegrated.integratedLufs());
        inputFifo.push (x[0], x[1], n);

        // 3. modules
        std::fill (bus[0].begin(), bus[0].begin() + n, 0.0f);
        std::fill (bus[1].begin(), bus[1].begin() + n, 0.0f);
        for (int k = 0; k < audioCount; ++k)
        {
            const int t = audioOrder[k];
            if (t == (int) ModuleType::Maximizer)
                applyDcFilter (n);
            processSlot (t, n, ctx);
        }

        if (! deltaMix.isAt (0.0f))
        {
            for (int i = 0; i < n; ++i)
            {
                const float mix = deltaMix.next();
                for (int c = 0; c < 2; ++c)
                    x[c][i] = mixExact (x[c][i], bus[c][i], mix);
            }
        }

        // 4. output gain, mono, swap
        for (int c = 0; c < 2; ++c)
            if (! outGain[c].isAt (1.0f))
                for (int i = 0; i < n; ++i)
                    x[c][i] *= outGain[c].next();

        if (! mono.isAt (0.0f))
        {
            for (int i = 0; i < n; ++i)
            {
                const float a = mono.next();
                const float mid = 0.5f * (x[0][i] + x[1][i]);
                x[0][i] = mixExact (x[0][i], mid, a);
                x[1][i] = mixExact (x[1][i], mid, a);
            }
        }

        if (! swap.isAt (0.0f))
        {
            for (int i = 0; i < n; ++i)
            {
                const float a = swap.next();
                const float l = x[0][i], rr = x[1][i];
                x[0][i] = mixExact (l, rr, a);
                x[1][i] = mixExact (rr, l, a);
            }
        }

        // 5. gain match: aligned bypass signal vs processed output
        bypassDelay.process (r[0], r[1], r[0], r[1], n);
        if (! monitoringActive)
        {
            matchInput.process (r[0], r[1], n);
            matchOutput.process (x[0], x[1], n);
            const float lin = matchInput.shortTermLufs(), lout = matchOutput.shortTermLufs();
            if (hasValue (lin) && hasValue (lout) && lin >= -60.0f && lout >= -60.0f)
                matchTargetDb = std::clamp ((double) lout - (double) lin, -24.0, 24.0);
        }

        // 6. match gain + global bypass
        const bool gainsIdle = classicAmount.isAt (0.0f) && modernAmount.isAt (0.0f) && soloClassicAmount.isAt (0.0f);
        float appliedDb = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            matchDb += matchCoeff * (matchTargetDb - matchDb);
            float gp = 1.0f, gb = 1.0f;
            if (! gainsIdle)
            {
                const float cA = classicAmount.next(), mA = modernAmount.next(), sA = soloClassicAmount.next();
                // classic + soloed powered-off slot: its dry signal gets that slot's own loudness change
                const double procDb = soloMatchDb * (double) sA - matchDb * (double) mA;
                const double bypDb = matchDb * (double) cA;
                gp = procDb != 0.0 ? (float) std::exp (procDb * kLn10Over20) : 1.0f;
                gb = bypDb != 0.0 ? (float) std::exp (bypDb * kLn10Over20) : 1.0f;
                appliedDb = (float) (bypass.value >= 0.5f ? bypDb : procDb);
            }

            const float bp = bypass.next();
            for (int c = 0; c < 2; ++c)
            {
                const float y = gp == 1.0f ? x[c][i] : x[c][i] * gp;
                const float b = gb == 1.0f ? r[c][i] : r[c][i] * gb;
                x[c][i] = mixExact (y, b, bp);
            }
        }
        ioMeters.gainMatchDb.store (appliedDb);

        // 7. dither
        dither.process (x, n);

        // 8. output taps
        meterPeaks (x[0], x[1], n, ioMeters.outputPeak, ioMeters.outputClip);
        ioMeters.outputTruePeak.storeMax (outputTp.process (x[0], x[1], n));
        outputMs.process (x[0], x[1], n);
        outputIntegrated.process (x[0], x[1], n);
        ioMeters.outputMomentaryLufs.store (outputMs.momentaryLufs());
        ioMeters.outputShortTermLufs.store (outputMs.shortTermLufs());
        ioMeters.outputIntegratedLufs.store (outputIntegrated.integratedLufs());
        ioMeters.outputLra.store (outputIntegrated.loudnessRange());
        outputFifo.push (x[0], x[1], n);

        std::memcpy (out0, x[0], sizeof (float) * (size_t) n);
        std::memcpy (out1, x[1], sizeof (float) * (size_t) n);
    }
};

//==============================================================================
Chain::Chain() : impl (std::make_unique<Impl>()) {}
Chain::~Chain() = default;

void Chain::prepare (double sampleRate, int maxBlockSize)
{
    auto& d = *impl;
    d.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    d.maxBlock = std::max (1, maxBlockSize);

    for (auto* set : { &d.raw, &d.cur, &d.dry, &d.bus, &d.tmp })
        for (auto& v : *set)
            v.assign ((size_t) d.maxBlock, 0.0f);

    const int slotCapacity = std::max (16384, (int) std::ceil (0.75 * d.sampleRate));
    const int deltaCapacity = std::max (32768, (int) std::ceil (1.3 * d.sampleRate));   // downstream latency
    for (auto& s : d.slots)
    {
        s.dry.prepare (slotCapacity);
        s.deltaLine.prepare (deltaCapacity);
        s.loudness.design (d.sampleRate);
    }
    d.bypassDelay.prepare (std::max (65536, (int) std::ceil (2.0 * d.sampleRate)));
    d.dcFilter.design (d.sampleRate);

    for (auto& m : d.modules)
        m->prepare (d.sampleRate, d.maxBlock);
    d.dither.prepare (d.sampleRate, d.maxBlock);

    for (auto* f : { &d.inputFifo, &d.outputFifo, &d.modulePreFifo, &d.modulePostFifo })
        f->prepare (1 << 15);

    for (auto* lm : { &d.inputMs, &d.inputIntegrated, &d.outputMs, &d.outputIntegrated, &d.matchInput, &d.matchOutput })
        lm->prepare (d.sampleRate);
    d.inputTp.clear();
    d.outputTp.clear();

    d.allRampLengths ((int) std::lround (0.02 * d.sampleRate));
    d.matchCoeff = 1.0 - std::exp (-1.0 / (0.35 * d.sampleRate));

    // The chain is not processing during prepare: take the message-thread order directly.
    d.audioCount = 0;
    for (int t = 0; t < kNumModuleTypes; ++t)
        d.audioInChain[t] = false;
    for (auto t : d.order)
    {
        d.audioOrder[d.audioCount++] = (int) t;
        d.audioInChain[(int) t] = true;
    }
    d.orderSeqSeen = d.orderBox.seq.load();

    int lat = 0;
    for (auto t : d.order)
        lat += std::max (0, d.modules[(int) t]->getLatencySamples());
    d.latency.store (lat);

    d.prepared = true;
    reset();
}

void Chain::reset() noexcept
{
    auto& d = *impl;
    for (auto& m : d.modules)
        m->reset();
    for (auto& s : d.slots)
        d.clearSlot (s);
    d.bypassDelay.clear();
    d.dcFilter.reset();
    d.dither.reset();
    for (auto* lm : { &d.inputMs, &d.inputIntegrated, &d.outputMs, &d.outputIntegrated, &d.matchInput, &d.matchOutput })
        lm->reset();
    d.inputTp.clear();
    d.outputTp.clear();
    d.matchDb = d.matchTargetDb = 0.0;
    d.soloMatchDb = 0.0;
    d.firstBlock = true;
}

void Chain::setOrder (const std::vector<ModuleType>& newOrder)
{
    std::vector<ModuleType> cleaned;
    cleaned.reserve ((size_t) kNumModuleTypes);
    for (auto t : newOrder)
        if ((int) t >= 0 && (int) t < kNumModuleTypes && std::find (cleaned.begin(), cleaned.end(), t) == cleaned.end())
            cleaned.push_back (t);
    impl->order = cleaned;
    impl->orderBox.write (cleaned);
}

std::vector<ModuleType> Chain::getOrder() const { return impl->order; }

bool Chain::isInChain (ModuleType t) const
{
    return std::find (impl->order.begin(), impl->order.end(), t) != impl->order.end();
}

void Chain::setSolo (int moduleTypeOrMinus1) noexcept
{
    impl->solo.store (moduleTypeOrMinus1 >= 0 && moduleTypeOrMinus1 < kNumModuleTypes ? moduleTypeOrMinus1 : -1);
}

int Chain::getSolo() const noexcept { return impl->solo.load(); }

void Chain::setDelta (ModuleType t, bool on) noexcept
{
    if ((int) t >= 0 && (int) t < kNumModuleTypes)
        impl->delta[(int) t].store (on);
}

bool Chain::getDelta (ModuleType t) const noexcept
{
    return (int) t >= 0 && (int) t < kNumModuleTypes && impl->delta[(int) t].load();
}

void Chain::setChannelSolo (ModuleType t, int channelSetOrMinus1) noexcept
{
    if ((int) t >= 0 && (int) t < kNumModuleTypes)
        impl->channelSolo[(int) t].store (channelSetOrMinus1 == 0 || channelSetOrMinus1 == 1 ? channelSetOrMinus1 : -1);
}

void Chain::setMonitor (ModuleType t, const ModuleMonitor& m) noexcept
{
    if ((int) t >= 0 && (int) t < kNumModuleTypes)
        impl->monitorBoxes[(int) t].write (m);
}

void Chain::setTapModule (int moduleTypeOrMinus1) noexcept
{
    impl->tapModule.store (moduleTypeOrMinus1 >= 0 && moduleTypeOrMinus1 < kNumModuleTypes ? moduleTypeOrMinus1 : -1);
}

void Chain::resetIntegratedLoudness() noexcept { impl->resetIntegratedRequest.store (true); }

void Chain::process (float* const* channels, int numSamples, ParamValues values, const HostInfo& host) noexcept
{
    auto& d = *impl;
    if (! d.prepared || channels == nullptr || channels[0] == nullptr || channels[1] == nullptr || numSamples <= 0)
        return;

    d.updateBlockState (values, numSamples);

    ProcessContext ctx;
    ctx.bpm = host.bpm > 0.0 ? host.bpm : 120.0;
    ctx.isPlaying = host.isPlaying;

    for (int offset = 0; offset < numSamples; offset += d.maxBlock)
    {
        const int n = std::min (d.maxBlock, numSamples - offset);
        d.processChunk (channels[0] + offset, channels[1] + offset, n, ctx);
    }

    d.finishBlockState();
}

int Chain::getLatencySamples() const noexcept { return impl->latency.load (std::memory_order_relaxed); }
double Chain::getSampleRate() const noexcept { return impl->sampleRate; }

Module& Chain::getModule (ModuleType t) noexcept
{
    const int i = (int) t;
    return *impl->modules[i >= 0 && i < kNumModuleTypes ? i : 0];
}

const IoMeters& Chain::getIoMeters() const noexcept { return impl->ioMeters; }
IoMeters& Chain::getIoMeters() noexcept { return impl->ioMeters; }
const DitherMeters& Chain::getDitherMeters() const noexcept { return impl->dither.getMeters(); }
void Chain::resetDitherPeakBits() noexcept { impl->dither.resetPeakBits(); }

void Chain::setModule (ModuleType type, std::unique_ptr<Module> module)
{
    const int i = (int) type;
    if (i >= 0 && i < kNumModuleTypes && module != nullptr)
        impl->modules[i] = std::move (module);
}

StereoSampleFifo& Chain::getInputFifo() noexcept { return impl->inputFifo; }
StereoSampleFifo& Chain::getOutputFifo() noexcept { return impl->outputFifo; }
StereoSampleFifo& Chain::getModulePreFifo() noexcept { return impl->modulePreFifo; }
StereoSampleFifo& Chain::getModulePostFifo() noexcept { return impl->modulePostFifo; }
} // namespace ps
