#include "dsp/MatchEq.h"

#include "dsp/Convolver.h"
#include "dsp/CurveFir.h"
#include "dsp/EqDesign.h"   // firLength / firPartition (shared FIR size table with the Equalizer; read-only)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE2__)
 #include <xmmintrin.h>
 #define PS_MEQ_HAS_MXCSR 1
#endif

// Match EQ (dsp-meq). Contract: MatchEq.h. Curve: MatchAnalysis.cpp, kernel design: CurveFir.cpp.
//
// Signal path: output = input delayed by the latency L (an exact delay line, always fed) + the convolver output of
// the residual kernel r = h - delta (Convolver, partition P = N / 16, its own one-partition crossfade on every kernel
// swap). Neutral settings give r = 0, so the output is the input delayed by L, bit-exact.
//
// Choices made where neither the contract nor the research decides:
// - Configuration (phase / FIR size): one preallocated Convolver per FIR size (memory: ~3 MB per instance at 48 kHz,
//   x2 at 96, x4 at 192). A switch fades the output out (4 ms raised cosine), switches the delay tap and the active
//   convolver (reset on the audio thread), fades in again. The delay line keeps its history, so the flat (delayed)
//   signal is exact at once; the residual is gated off until the worker has installed a zero kernel for the new
//   configuration (no kernel left over from an earlier use of that convolver can leak) plus one partition, and the
//   designed kernel is installed only once the new convolver holds N + 2P samples of input history, so the switch
//   never plays the onset transient of a convolution that starts mid-signal (~0.38 s at FIR 16384, 48 kHz). Before
//   anything has been processed since prepare() / reset() the switch is immediate (no fade).
// - Kernel swaps (parameters, spectra, channel mode): the worker designs, then calls Convolver::setKernel for each
//   channel; the Convolver crossfades old -> new over one partition (5 .. 43 ms at 48 kHz). In Stereo the two channels
//   are installed one after the other, so for at most one partition they can run different kernels (rare: only when
//   a partition boundary falls between the two calls, ~1 % of the swaps).
// - Worker: polls every 5 ms (2 ms while work is outstanding). Inputs: a lock-free triple buffer of the set parameters
//   + channel mode (audio thread -> worker), the spectra (mutex shared with the message thread only), the active
//   configuration (one atomic). Offline renders faster than realtime apply a changed curve a few blocks earlier or
//   later than a realtime pass (same limitation as the Equalizer's Digital mode).
// - Capture: the audio thread never overwrites unread samples: a block that does not fit into the FIFO is dropped
//   and counted as one gap (consecutive dropped blocks = one gap). getCaptureSeconds() = non-silent audio analysed
//   (MatchAnalyser::getSeconds()). A stop with less than 1 s of non-silent audio publishes an invalid spectrum (the
//   role is then empty; the processor's undo history can bring the previous one back).
// - Meters: curveMaxDb / curveMinDb of set A and B are published once every channel runs its new kernel (Stereo:
//   set B shows set A's values, both channels use set A).
// - Audition: the latency-aligned input through a unity-peak band pass at auditionHz / auditionQ (as the Equalizer);
//   Stereo: both channels, M/S / L/R: only the chosen channel set, the other is muted; 10 ms fades.
namespace ps
{
namespace
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr int kNumFirSizes = 4;
    constexpr int kFifoCapacity = 1 << 17;
    constexpr double kConfigFadeSeconds = 0.004;
    constexpr double kAuditionFadeSeconds = 0.010;
    constexpr auto kRunningPublishInterval = std::chrono::milliseconds (250);
    constexpr long long kHistoryFull = 1LL << 60;
    constexpr unsigned kUnknownKernel = 0xffffffffu;   // worker: what a convolver channel runs is not known
    constexpr unsigned kZeroKernel = 0;                // worker: the zero kernel (r = 0)

    struct DenormalGuard
    {
#if PS_MEQ_HAS_MXCSR
        unsigned int saved;
        DenormalGuard() noexcept : saved (_mm_getcsr()) { _mm_setcsr (saved | 0x8040u); }   // FTZ | DAZ
        ~DenormalGuard() { _mm_setcsr (saved); }
        DenormalGuard (const DenormalGuard&) = delete;
        DenormalGuard& operator= (const DenormalGuard&) = delete;
#endif
    };

    // Single writer, single reader, newest value wins; never blocks.
    template <typename T>
    class TripleBuffer
    {
    public:
        void write (const T& v) noexcept
        {
            buffers[writeIndex] = v;
            writeIndex = middle.exchange (writeIndex | kFresh, std::memory_order_acq_rel) & 3;
        }
        bool read (T& out) noexcept
        {
            if ((middle.load (std::memory_order_acquire) & kFresh) == 0)
                return false;
            readIndex = middle.exchange (readIndex, std::memory_order_acq_rel) & 3;
            out = buffers[readIndex];
            return true;
        }
        void clear() noexcept
        {
            writeIndex = 0;
            readIndex = 1;
            middle.store (2);
        }

    private:
        static constexpr int kFresh = 4;
        T buffers[3] {};
        int writeIndex = 0, readIndex = 1;
        std::atomic<int> middle { 2 };
    };

    struct Config
    {
        MatchPhase phase = MatchPhase::Linear;
        int sizeIndex = 2;
        bool operator== (const Config& o) const noexcept { return phase == o.phase && sizeIndex == o.sizeIndex; }
        bool operator!= (const Config& o) const noexcept { return ! (*this == o); }
    };

    bool sameSet (const MatchEqSetParams& a, const MatchEqSetParams& b) noexcept
    {
        return a.amountPercent == b.amountPercent && a.smoothPercent == b.smoothPercent && a.lowHz == b.lowHz
               && a.highHz == b.highHz && a.maxBoostDb == b.maxBoostDb && a.maxCutDb == b.maxCutDb;
    }

    struct Request
    {
        MatchEqSetParams sets[kNumChannelSets];
        int mode = 0;   // ChannelMode (T/S -> Stereo)
        bool operator== (const Request& o) const noexcept
        {
            return mode == o.mode && sameSet (sets[0], o.sets[0]) && sameSet (sets[1], o.sets[1]);
        }
    };

    inline unsigned long long packConfig (unsigned epoch, const Config& c) noexcept
    {
        return ((unsigned long long) epoch << 8) | ((unsigned long long) c.sizeIndex << 1)
               | (c.phase == MatchPhase::Minimum ? 1ull : 0ull);
    }

    inline bool isDual (ChannelMode m) noexcept { return m == ChannelMode::MidSide || m == ChannelMode::LeftRight; }
} // namespace

//==============================================================================
struct MatchEq::Impl
{
    MatchEqMeters meters;

    // ---- setup
    double sr = 48000.0;
    int maxBlock = 0;
    bool prepared = false;

    struct Fir
    {
        Convolver conv;
        int taps = 0, partition = 0;
    };
    Fir fir[kNumFirSizes];

    // ---- audio thread
    std::vector<float> ring[2];
    int ringMask = 0, writePos = 0;
    std::vector<float> convBuf[2], alignedBuf[2], capBuf[2];
    MatchEqParams params;
    Config cur, target;
    Fir* active = nullptr;
    int curLatency = 0;
    unsigned epoch = 0;
    bool fresh = true;
    double fadeLin = 1.0, fadeStep = 0.01;
    int fadeDir = 0;
    bool wetOpen = true;
    long long wetCountdown = -1;
    long long historyLocal = kHistoryFull;
    Request lastRequest;
    bool requestPublished = false;
    unsigned long long gapSession = 0;
    ModuleMonitor monitor;
    double audMix = 0.0, audG = 0.0, audK = 0.25, audStep = 0.01;
    double audIc1[2] {}, audIc2[2] {}, audSelect[2] { 1.0, 1.0 };
    bool audDual = false;

    // ---- shared with the worker (atomics)
    TripleBuffer<Request> requests;
    std::atomic<unsigned> requestSerial { 0 }, requestSeen { 0 };
    std::atomic<unsigned long long> activeConfig { 0 };
    std::atomic<unsigned> wetEpoch { 0 };
    std::atomic<unsigned> wetOpenEpoch { 0 };   // audio acknowledgement before a nonzero kernel may be published
    std::atomic<long long> history { kHistoryFull };
    std::atomic<bool> designPending { false }, switching { false };
    std::atomic<int> reportedLatency { 0 };

    // ---- spectra (message thread, worker)
    mutable std::mutex spectraMutex;
    MatchSpectrum spectra[2];
    std::atomic<unsigned> generation { 0 }, generationSeen { 0 };

    // ---- capture (audio pushes; worker / message thread consume under captureMutex)
    StereoSampleFifo fifo;
    std::atomic<unsigned long long> consumed { 0 };
    std::atomic<unsigned long long> captureSession { 0 };   // odd = capturing, changes at every start / stop
    std::atomic<bool> capturePushing { false };
    std::atomic<int> captureRole { 0 };
    std::atomic<int> captureGaps { 0 };
    std::atomic<double> captureSeconds { 0.0 };
    std::mutex captureMutex;
    MatchAnalyser analyser;
    std::vector<float> drainL, drainR;
    std::chrono::steady_clock::time_point lastRunningPublish;
    mutable std::mutex runningMutex;
    MatchSpectrum running, runningScratch;
    bool runningValid = false;

    // ---- worker
    std::thread worker;
    std::atomic<bool> quit { false };
    fir::CurveKernelDesigner designer;
    std::vector<float> taps[2];
    MatchSpectrum workRef, workCur;
    float curves[2][kMatchPoints] {};

    //==============================================================================
    int latencyFor (const Config& c) const noexcept
    {
        const auto& f = fir[std::clamp (c.sizeIndex, 0, kNumFirSizes - 1)];
        return (c.phase == MatchPhase::Linear ? f.taps / 2 : 0) + f.partition;
    }

    static Config configOf (const MatchEqParams& p) noexcept
    {
        Config c;
        c.phase = p.phase;
        c.sizeIndex = std::clamp (p.firSizeIndex, 0, kNumFirSizes - 1);
        return c;
    }

    //==============================================================================
    // Audio thread
    void applyConfig() noexcept
    {
        cur = target;
        ++epoch;
        active = &fir[cur.sizeIndex];
        curLatency = std::min (latencyFor (cur), ringMask);
        active->conv.reset();
        historyLocal = fresh ? kHistoryFull : 0;
        history.store (historyLocal, std::memory_order_release);
        wetOpen = false;
        wetCountdown = -1;
        activeConfig.store (packConfig (epoch, cur), std::memory_order_release);
    }

    void publishRequest (ChannelMode mode) noexcept
    {
        Request r;
        r.mode = (int) mode;
        r.sets[0] = params.sets[0];
        r.sets[1] = isDual (mode) ? params.sets[1] : params.sets[0];
        if (requestPublished && r == lastRequest)
            return;
        lastRequest = r;
        requestPublished = true;
        requests.write (r);
        requestSerial.fetch_add (1, std::memory_order_release);
    }

    void pushCapture (float* const* p, int n, ChannelMode mode) noexcept
    {
        const auto session = captureSession.load (std::memory_order_seq_cst);
        if ((session & 1u) == 0)
            return;
        // Enrol before rechecking the session. Stop waits only for an already-enrolled producer; a late
        // producer from an old session fails the recheck even if another capture has since started.
        capturePushing.store (true, std::memory_order_seq_cst);
        if (captureSession.load (std::memory_order_seq_cst) != session)
        {
            capturePushing.store (false, std::memory_order_seq_cst);
            return;
        }
        const auto used = fifo.getWriteCount() - consumed.load (std::memory_order_acquire);
        if (used + (unsigned long long) n > (unsigned long long) kFifoCapacity)
        {
            if (gapSession != session)
                captureGaps.fetch_add (1, std::memory_order_relaxed);
            gapSession = session;
            capturePushing.store (false, std::memory_order_seq_cst);
            return;
        }
        gapSession = 0;
        if (mode == ChannelMode::MidSide)
        {
            for (int i = 0; i < n; ++i)
            {
                capBuf[0][(size_t) i] = p[0][i] + p[1][i];
                capBuf[1][(size_t) i] = p[0][i] - p[1][i];
            }
            fifo.push (capBuf[0].data(), capBuf[1].data(), n);
        }
        else
        {
            fifo.push (p[0], p[1], n);
        }
        capturePushing.store (false, std::memory_order_seq_cst);
    }

    void processAudition (float* const* p, int n, bool dual) noexcept
    {
        const double tgt = monitor.auditionOn ? 1.0 : 0.0;
        const int set = std::clamp (monitor.auditionChannelSet, 0, 1);
        if (audMix == 0.0 && tgt == 0.0)
            return;
        if (audMix == 0.0)
        {
            audIc1[0] = audIc1[1] = audIc2[0] = audIc2[1] = 0.0;
            audG = std::tan (kPi * std::clamp ((double) monitor.auditionHz, 10.0, 0.45 * sr) / sr);
            audK = 1.0 / std::clamp ((double) monitor.auditionQ, 0.1, 24.0);
            for (int c = 0; c < 2; ++c)
                audSelect[c] = ! dual || c == set ? 1.0 : 0.0;
        }

        const double gT = std::tan (kPi * std::clamp ((double) monitor.auditionHz, 10.0, 0.45 * sr) / sr);
        const double kT = 1.0 / std::clamp ((double) monitor.auditionQ, 0.1, 24.0);
        const double sm = 1.0 - std::exp (-1.0 / (0.01 * sr));

        for (int i = 0; i < n; ++i)
        {
            audG += (gT - audG) * sm;
            audK += (kT - audK) * sm;
            if (audMix < tgt)
                audMix = std::min (tgt, audMix + audStep);
            else if (audMix > tgt)
                audMix = std::max (tgt, audMix - audStep);

            const double a1 = 1.0 / (1.0 + audG * (audG + audK));
            const double a2 = audG * a1;
            const double a3 = audG * a2;
            for (int c = 0; c < 2; ++c)
            {
                const double v0 = alignedBuf[c][(size_t) i];
                const double v3 = v0 - audIc2[c];
                const double v1 = a1 * audIc1[c] + a2 * v3;
                const double v2 = audIc2[c] + a2 * audIc1[c] + a3 * v3;
                audIc1[c] = 2.0 * v1 - audIc1[c];
                audIc2[c] = 2.0 * v2 - audIc2[c];
                const double heard = ! dual || c == set ? 1.0 : 0.0;
                audSelect[c] += std::clamp (heard - audSelect[c], -audStep, audStep);
                const double bp = audSelect[c] * audK * v1;
                p[c][i] = (float) ((double) p[c][i] + (bp - (double) p[c][i]) * audMix);
            }
        }
        for (int c = 0; c < 2; ++c)
        {
            if (std::abs (audIc1[c]) < 1.0e-30) audIc1[c] = 0.0;
            if (std::abs (audIc2[c]) < 1.0e-30) audIc2[c] = 0.0;
        }
    }

    void runSegment (float* const* p, int off, int len, bool dual) noexcept
    {
        float* x[2] { p[0] + off, p[1] + off };

        // residual gate: opens one partition after the worker confirmed a zero kernel for this configuration
        if (! wetOpen && wetEpoch.load (std::memory_order_acquire) == epoch)
        {
            if (wetCountdown < 0)
                wetCountdown = active->partition;
            if (wetCountdown <= 0)
            {
                wetOpen = true;
                wetOpenEpoch.store (epoch, std::memory_order_release);
            }
        }

        for (int c = 0; c < 2; ++c)
        {
            float* r = ring[c].data();
            for (int i = 0; i < len; ++i)
                r[(writePos + i) & ringMask] = x[c][i];
            std::memcpy (convBuf[c].data(), x[c], sizeof (float) * (size_t) len);
        }
        float* cb[2] { convBuf[0].data(), convBuf[1].data() };
        active->conv.process (cb, len);

        if (historyLocal < kHistoryFull)
        {
            historyLocal += len;
            history.store (historyLocal, std::memory_order_release);
        }
        if (! wetOpen && wetCountdown > 0)
            wetCountdown = std::max (0LL, wetCountdown - len);

        for (int c = 0; c < 2; ++c)
        {
            const float* r = ring[c].data();
            float* a = alignedBuf[c].data();
            for (int i = 0; i < len; ++i)
                a[i] = r[(writePos + i - curLatency) & ringMask];
            if (wetOpen)
                for (int i = 0; i < len; ++i)
                    x[c][i] = a[i] + cb[c][i];
            else
                std::memcpy (x[c], a, sizeof (float) * (size_t) len);
        }
        writePos = (writePos + len) & ringMask;

        if (audMix > 0.0 || monitor.auditionOn)
            processAudition (x, len, dual);

        if (fadeDir != 0 || fadeLin < 1.0)
        {
            for (int i = 0; i < len; ++i)
            {
                if (fadeDir != 0)
                {
                    fadeLin = std::clamp (fadeLin + fadeStep * (double) fadeDir, 0.0, 1.0);
                    if (fadeDir > 0 && fadeLin >= 1.0)
                        fadeDir = 0;
                }
                const float g = (float) (0.5 - 0.5 * std::cos (kPi * fadeLin));
                x[0][i] *= g;
                x[1][i] *= g;
            }
        }
    }

    void processChunk (float* const* p, int n, ChannelMode mode) noexcept
    {
        pushCapture (p, n, mode);
        const bool dual = isDual (mode);

        int done = 0;
        while (done < n)
        {
            if (target != cur)
            {
                if (fresh)
                    applyConfig();
                else if (fadeDir >= 0)
                    fadeDir = -1;
            }
            else if (fadeDir < 0)
            {
                fadeDir = 1;   // switched back before the fade-out ended
            }

            int len = n - done;
            if (fadeDir < 0)
                len = std::min (len, std::max (1, (int) std::ceil (fadeLin / fadeStep - 1.0e-9)));
            runSegment (p, done, len, dual);
            done += len;

            if (fadeDir < 0 && fadeLin <= 0.0)
            {
                applyConfig();
                fadeDir = 1;
            }
        }
        fresh = false;
        switching.store (target != cur || fadeDir != 0 || ! wetOpen, std::memory_order_release);
    }

    //==============================================================================
    // Capture consumer side (worker / message thread, under captureMutex)
    bool captureActive() const noexcept
    {
        return (captureSession.load (std::memory_order_seq_cst) & 1u) != 0;
    }

    bool closeCapture() noexcept
    {
        if (! captureActive())
            return false;
        captureSession.fetch_add (1, std::memory_order_seq_cst);
        // Message thread only. The audio thread never waits and only holds this flag during one FIFO push.
        while (capturePushing.load (std::memory_order_seq_cst))
            std::this_thread::yield();
        return true;
    }

    void drainIntoAnalyser (bool feed) noexcept
    {
        // One bounded batch: continuous input must not starve kernel design or a message-thread Stop.
        // With the producer closed, this also drains the entire remaining FIFO in one pull.
        const int n = fifo.pull (drainL.data(), drainR.data(), kFifoCapacity);
        consumed.fetch_add ((unsigned long long) n, std::memory_order_release);
        if (feed && n > 0)
            analyser.push (drainL.data(), drainR.data(), n);
    }

    void serviceCapture() noexcept
    {
        if (! captureActive())
            return;
        std::unique_lock<std::mutex> lock (captureMutex, std::try_to_lock);
        if (! lock.owns_lock() || ! captureActive())
            return;
        drainIntoAnalyser (true);
        captureSeconds.store (analyser.getSeconds(), std::memory_order_relaxed);

        const auto now = std::chrono::steady_clock::now();
        if (now - lastRunningPublish >= kRunningPublishInterval)
        {
            lastRunningPublish = now;
            analyser.spectrum (runningScratch);
            setMatchSource (runningScratch, "capture");
            const std::lock_guard<std::mutex> rl (runningMutex);
            running = runningScratch;
            runningValid = true;
        }
    }

    //==============================================================================
    void workerLoop() noexcept
    {
        DenormalGuard denormals;
        Request req;
        bool haveReq = false, designDirty = true;
        unsigned seenGen = generation.load() + 1u;   // force a first read
        Config designed;
        int designedSize = -1;
        bool flat[2] { true, true };
        bool dual = false;
        unsigned designId = 0;
        unsigned installed[2] { kUnknownKernel, kUnknownKernel };
        unsigned workerEpoch = 0xffffffffu;
        bool zeroConfirmed = false, metersCurrent = false;
        const float zeroTap = 0.0f;

        while (! quit.load (std::memory_order_acquire))
        {
            serviceCapture();
            bool busy = false;

            const unsigned serial = requestSerial.load (std::memory_order_acquire);
            Request r;
            if (requests.read (r))
            {
                if (! haveReq || ! (r == req))
                    designDirty = true;
                req = r;
                haveReq = true;
            }

            const unsigned gen = generation.load (std::memory_order_acquire);
            if (gen != seenGen)
            {
                const std::lock_guard<std::mutex> lock (spectraMutex);
                workRef = spectra[0];
                workCur = spectra[1];
                seenGen = generation.load (std::memory_order_relaxed);
                designDirty = true;
            }

            const auto packed = activeConfig.load (std::memory_order_acquire);
            const unsigned e = (unsigned) (packed >> 8);
            Config cfg;
            cfg.sizeIndex = (int) ((packed >> 1) & 3u);
            cfg.phase = (packed & 1u) != 0 ? MatchPhase::Minimum : MatchPhase::Linear;
            if (designedSize < 0 || cfg != designed)
                designDirty = true;

            auto& f = fir[cfg.sizeIndex];

            if (designDirty && haveReq)
            {
                const auto mode = (ChannelMode) req.mode;
                dual = isDual (mode);
                for (int set = 0; set < (dual ? 2 : 1); ++set)
                    computeMatchCurve (workRef, workCur, matchChannelFor (mode, set), req.sets[set], curves[set]);
                if (! dual)
                    std::copy (curves[0], curves[0] + kMatchPoints, curves[1]);
                for (int set = 0; set < (dual ? 2 : 1); ++set)
                    flat[set] = ! designer.design (curves[set], sr, f.taps, cfg.phase, taps[set].data());
                if (! dual)
                    flat[1] = flat[0];
                designed = cfg;
                designedSize = cfg.sizeIndex;
                if (++designId == kUnknownKernel || designId == kZeroKernel)
                    designId = 1;
                designDirty = false;
                metersCurrent = false;
            }
            requestSeen.store (serial, std::memory_order_release);
            generationSeen.store (seenGen, std::memory_order_release);

            if (e != workerEpoch)
            {
                workerEpoch = e;
                installed[0] = installed[1] = kUnknownKernel;
                zeroConfirmed = false;
            }

            if (! zeroConfirmed)
            {
                for (int c = 0; c < 2; ++c)
                    if (installed[c] != kZeroKernel && f.conv.setKernel (c, &zeroTap, 1))
                        installed[c] = kZeroKernel;
                if (installed[0] == kZeroKernel && installed[1] == kZeroKernel && ! f.conv.isKernelPending (0)
                    && ! f.conv.isKernelPending (1))
                {
                    wetEpoch.store (e, std::memory_order_release);
                    zeroConfirmed = true;
                }
                else
                {
                    busy = true;
                }
            }

            if (zeroConfirmed && haveReq && ! designDirty && designed == cfg)
            {
                const bool historyOk = history.load (std::memory_order_acquire) >= (long long) f.taps + 2LL * f.partition
                                       && wetOpenEpoch.load (std::memory_order_acquire) == e;
                bool allInstalled = true;
                for (int c = 0; c < 2; ++c)
                {
                    const int set = dual ? c : 0;
                    const unsigned want = flat[set] ? kZeroKernel : designId;
                    if (installed[c] != want)
                    {
                        allInstalled = false;
                        if (want != kZeroKernel && ! historyOk)
                            continue;
                        const bool ok = want == kZeroKernel ? f.conv.setKernel (c, &zeroTap, 1)
                                                            : f.conv.setKernel (c, taps[set].data(), f.taps);
                        if (ok)
                            installed[c] = want;
                    }
                    if (f.conv.isKernelPending (c))
                        allInstalled = false;
                }
                if (allInstalled && ! metersCurrent)
                {
                    for (int set = 0; set < kNumChannelSets; ++set)
                    {
                        const float* cv = curves[dual ? set : 0];
                        const auto mm = std::minmax_element (cv, cv + kMatchPoints);
                        meters.curveMinDb[set].store (std::min (0.0f, *mm.first));
                        meters.curveMaxDb[set].store (std::max (0.0f, *mm.second));
                    }
                    metersCurrent = true;
                }
                busy = busy || ! allInstalled;
            }
            else if (haveReq)
            {
                busy = true;
            }

            designPending.store (busy || designDirty, std::memory_order_release);
            std::this_thread::sleep_for (std::chrono::milliseconds (busy ? 2 : 5));
        }
    }

    void startWorker()
    {
        quit.store (false);
        worker = std::thread ([this] { workerLoop(); });
    }

    void stopWorker()
    {
        if (worker.joinable())
        {
            quit.store (true);
            worker.join();
        }
    }
};

//==============================================================================
MatchEq::MatchEq() : impl (std::make_unique<Impl>())
{
    for (auto& s : impl->spectra)
        for (auto& ch : s.psdDb)
            ch.fill (kMatchNoValue);
}

MatchEq::~MatchEq()
{
    impl->stopWorker();
}

void MatchEq::prepare (double sampleRate, int maxBlockSize)
{
    auto& m = *impl;
    m.stopWorker();

    m.sr = sampleRate > 0.0 && std::isfinite (sampleRate) ? sampleRate : 48000.0;
    m.maxBlock = std::max (1, maxBlockSize);

    int maxTaps = 0, maxLatency = 0;
    const float zeroTap = 0.0f;
    for (int i = 0; i < kNumFirSizes; ++i)
    {
        auto& f = m.fir[i];
        f.taps = eqdesign::firLength (i, m.sr);
        f.conv.prepare (f.taps, eqdesign::firPartition (f.taps), 2);
        f.partition = f.conv.getLatencySamples();
        f.conv.setKernel (0, &zeroTap, 1);   // replaces the identity kernel prepare() installs
        f.conv.setKernel (1, &zeroTap, 1);
        f.conv.reset();                      // installs the pending zero kernels at once
        maxTaps = std::max (maxTaps, f.taps);
        maxLatency = std::max (maxLatency, f.taps / 2 + f.partition);
    }

    int cap = 1;
    while (cap < maxLatency + m.maxBlock + 1)
        cap <<= 1;
    for (int c = 0; c < 2; ++c)
    {
        m.ring[c].assign ((size_t) cap, 0.0f);
        m.convBuf[c].assign ((size_t) m.maxBlock, 0.0f);
        m.alignedBuf[c].assign ((size_t) m.maxBlock, 0.0f);
        m.capBuf[c].assign ((size_t) m.maxBlock, 0.0f);
        m.taps[c].assign ((size_t) maxTaps, 0.0f);
    }
    m.ringMask = cap - 1;
    m.writePos = 0;

    m.designer.prepare (maxTaps);
    m.fifo.prepare (kFifoCapacity);
    m.consumed.store (0);
    m.drainL.assign ((size_t) kFifoCapacity, 0.0f);
    m.drainR.assign ((size_t) kFifoCapacity, 0.0f);
    m.analyser.prepare (m.sr);
    m.captureSession.store (0);
    m.capturePushing.store (false);
    m.captureGaps.store (0);
    m.captureSeconds.store (0.0);
    {
        const std::lock_guard<std::mutex> rl (m.runningMutex);
        m.runningValid = false;
    }

    m.fadeStep = 1.0 / std::max (1.0, kConfigFadeSeconds * m.sr);
    m.audStep = 1.0 / std::max (1.0, kAuditionFadeSeconds * m.sr);

    // configuration: the parameter defaults until the first setParams
    m.params = MatchEqParams {};
    if (getParamSet() == 1)
        m.params.sets[0] = m.params.sets[1];
    m.cur = m.target = Impl::configOf (m.params);
    m.active = &m.fir[m.cur.sizeIndex];
    m.curLatency = std::min (m.latencyFor (m.cur), m.ringMask);
    m.reportedLatency.store (m.latencyFor (m.target));
    m.epoch = 0;
    m.activeConfig.store (packConfig (0, m.cur));
    m.wetEpoch.store (0);      // every convolver runs a zero kernel now
    m.wetOpenEpoch.store (0);
    m.wetOpen = true;
    m.wetCountdown = -1;
    m.historyLocal = kHistoryFull;
    m.history.store (kHistoryFull);
    m.fadeLin = 1.0;
    m.fadeDir = 0;
    m.fresh = true;
    m.requests.clear();
    m.requestPublished = false;
    m.requestSerial.store (0);
    m.requestSeen.store (0);
    m.designPending.store (false);
    m.switching.store (false);
    m.gapSession = 0;
    m.audMix = 0.0;
    for (int set = 0; set < kNumChannelSets; ++set)
    {
        m.meters.curveMaxDb[set].store (0.0f);
        m.meters.curveMinDb[set].store (0.0f);
    }

    m.prepared = true;
    m.startWorker();
}

void MatchEq::reset() noexcept
{
    auto& m = *impl;
    if (! m.prepared)
        return;
    for (auto& r : m.ring)
        std::fill (r.begin(), r.end(), 0.0f);
    m.writePos = 0;
    m.fresh = true;
    if (m.target != m.cur)
    {
        m.applyConfig();   // history cleared too: no fade needed
    }
    else
    {
        m.active->conv.reset();
        m.historyLocal = kHistoryFull;
        m.history.store (kHistoryFull, std::memory_order_release);
    }
    m.fadeLin = 1.0;
    m.fadeDir = 0;
    m.audMix = 0.0;
    m.audIc1[0] = m.audIc1[1] = m.audIc2[0] = m.audIc2[1] = 0.0;
    m.gapSession = 0;
}

void MatchEq::setParams (ParamValues values) noexcept
{
    auto& m = *impl;
    readMatchEqParams (values, m.params);
    if (getParamSet() == 1)
        m.params.sets[0] = m.params.sets[1];   // T/S twin: set B for both channels (Module.h rule 1)
    m.target = Impl::configOf (m.params);
    if (m.prepared)
        m.reportedLatency.store (m.latencyFor (m.target), std::memory_order_relaxed);
}

void MatchEq::setMonitor (const ModuleMonitor& monitor) noexcept
{
    impl->monitor = monitor;
}

int MatchEq::getLatencySamplesForParams (ParamValues values) const noexcept
{
    MatchEqParams params;
    readMatchEqParams (values, params);
    return impl->prepared ? impl->latencyFor (Impl::configOf (params)) : getLatencySamples();
}

void MatchEq::process (float* const* channels, int numSamples, const ProcessContext& ctx) noexcept
{
    auto& m = *impl;
    if (! m.prepared || numSamples <= 0 || channels == nullptr || channels[0] == nullptr || channels[1] == nullptr)
        return;

    DenormalGuard denormals;
    const ChannelMode mode = ctx.channelMode == ChannelMode::TransientSustain ? ChannelMode::Stereo : ctx.channelMode;
    m.publishRequest (mode);

    for (int done = 0; done < numSamples;)
    {
        const int n = std::min (numSamples - done, m.maxBlock);
        float* p[2] { channels[0] + done, channels[1] + done };
        m.processChunk (p, n, mode);
        done += n;
    }
}

int MatchEq::getLatencySamples() const noexcept { return impl->reportedLatency.load (std::memory_order_relaxed); }

const MatchEqMeters& MatchEq::getMeters() const noexcept { return impl->meters; }

bool MatchEq::isDesignPending() const noexcept
{
    const auto& m = *impl;
    return m.designPending.load (std::memory_order_acquire) || m.switching.load (std::memory_order_acquire)
           || m.requestSerial.load (std::memory_order_acquire) != m.requestSeen.load (std::memory_order_acquire)
           || m.generation.load (std::memory_order_acquire) != m.generationSeen.load (std::memory_order_acquire);
}

//==============================================================================
void MatchEq::startCapture (MatchRole role)
{
    auto& m = *impl;
    const std::lock_guard<std::mutex> lock (m.captureMutex);
    m.closeCapture();
    if (m.prepared)
        m.drainIntoAnalyser (false);   // drop whatever an earlier capture left
    m.analyser.reset();
    m.captureGaps.store (0);
    m.captureSeconds.store (0.0);
    {
        const std::lock_guard<std::mutex> rl (m.runningMutex);
        m.runningValid = false;
    }
    m.lastRunningPublish = std::chrono::steady_clock::now();
    m.captureRole.store (role == MatchRole::Current ? 1 : 0);
    m.captureSession.fetch_add (1, std::memory_order_seq_cst);
}

void MatchEq::stopCapture()
{
    auto& m = *impl;
    MatchSpectrum result;
    MatchRole role = MatchRole::Reference;
    {
        const std::lock_guard<std::mutex> lock (m.captureMutex);
        if (! m.closeCapture())
            return;
        if (m.prepared)
            m.drainIntoAnalyser (true);
        m.analyser.spectrum (result);
        setMatchSource (result, "capture");
        role = m.captureRole.load() == 1 ? MatchRole::Current : MatchRole::Reference;
        m.captureSeconds.store (m.analyser.getSeconds());
        const std::lock_guard<std::mutex> rl (m.runningMutex);
        m.runningValid = false;
    }
    setSpectrum (role, result);
}

bool MatchEq::isCapturing (MatchRole* roleOut) const noexcept
{
    const auto& m = *impl;
    if (roleOut != nullptr)
        *roleOut = m.captureRole.load() == 1 ? MatchRole::Current : MatchRole::Reference;
    return m.captureActive();
}

double MatchEq::getCaptureSeconds() const noexcept { return impl->captureSeconds.load (std::memory_order_relaxed); }
int MatchEq::getCaptureGaps() const noexcept { return impl->captureGaps.load (std::memory_order_relaxed); }

bool MatchEq::getRunningSpectrum (MatchSpectrum& out) const
{
    const auto& m = *impl;
    if (! m.captureActive())
        return false;
    const std::lock_guard<std::mutex> rl (m.runningMutex);
    if (! m.runningValid)
        return false;
    out = m.running;
    return true;
}

MatchSpectrum MatchEq::getSpectrum (MatchRole role) const
{
    const std::lock_guard<std::mutex> lock (impl->spectraMutex);
    return impl->spectra[role == MatchRole::Current ? 1 : 0];
}

void MatchEq::setSpectrum (MatchRole role, const MatchSpectrum& s)
{
    const std::lock_guard<std::mutex> lock (impl->spectraMutex);
    impl->spectra[role == MatchRole::Current ? 1 : 0] = s;
    impl->generation.fetch_add (1, std::memory_order_release);
}

void MatchEq::clearSpectrum (MatchRole role)
{
    MatchSpectrum empty;
    for (auto& ch : empty.psdDb)
        ch.fill (kMatchNoValue);
    setSpectrum (role, empty);
}

unsigned MatchEq::getSpectrumGeneration() const noexcept { return impl->generation.load (std::memory_order_acquire); }
} // namespace ps
