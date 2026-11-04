#include "dsp/DeLimiter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

// De-limiter. Contract: DeLimiter.h. Constants fitted by out/delimit/tools/fit_model.py against the reference's renders.
namespace ps
{
namespace
{
struct Hinge
{
    double kneeDb, slope;   // adds slope x (value - kneeDb) above the knee
};

// ---- fitted constants ----
constexpr Hinge kRmsHinges[] = { { -12.0, -0.148836 }, { -9.0, 0.117688 }, { -6.0, -0.003983 }, { -3.0, 0.116624 }, { 0.0, 0.093991 }, { 3.0, 0.021337 }, { 6.0, 0.113053 }, { 9.0, 1.469649 } };
constexpr Hinge kPeakHinges[] = { { -9.0, 0.059024 }, { -6.0, -0.003848 }, { -3.0, 0.067230 }, { 0.0, -0.157136 }, { 3.0, 0.076680 }, { 6.0, -0.334859 } };
constexpr Hinge kSlowHinges[] = { { -15.0, 0.194846 }, { -12.0, -0.274695 }, { -9.0, 0.126665 }, { -6.0, -0.134308 }, { -3.0, 0.061861 }, { 0.0, -0.130474 }, { 3.0, 0.169201 } };
constexpr Hinge kContrastHinges[] = { { -3.0, 0.138429 }, { 0.0, 0.001894 }, { 3.0, 0.272015 }, { 6.0, -0.795283 }, { 9.0, 0.288243 } };
constexpr double kGateFromDb = -14.0, kGateToDb = -8.0;      // peak relative to the threshold: no lift .. full lift
constexpr double kClipBoost = 1.5;                           // full flat-top share: G x (1 + kClipBoost)
constexpr double kClipShareFrom = 0.004, kClipShareTo = 0.03;
constexpr double kFlatStep = 4.0e-4, kFlatLevel = 0.35;      // relative to the threshold's level; step at 44.1 kHz
constexpr double kSpikeScale = 0.9, kSpikePower = 1.3;       // G -> scale x G^power
constexpr double kMaxGainDb = 11.0;
constexpr double kSlowMs = 60.0, kClipMs = 300.0;
// ---- structure ----
constexpr int kHopAt44k = 16;
constexpr int kWindowHops = 6;      // rms and peak windows
constexpr int kSmoothHops = 3;      // final box
constexpr int kLatencyHops = 5;     // window centre 3 + box centre 1 + interpolation 1
constexpr int kHistoryHops = 4;     // one display pair per 4 hops
constexpr double kGlideMs = 20.0;
constexpr double kLearnSilence = 1.0e-6;   // the first sample above this starts the 6 s

int hopFor (double rate) noexcept
{
    return std::max (1, (int) std::lround (kHopAt44k * rate / 44100.0));
}

template <size_t N>
double hingeSum (const Hinge (&h)[N], double v) noexcept
{
    double sum = 0.0;
    for (const auto& k : h)
        if (v > k.kneeDb)
            sum += k.slope * (v - k.kneeDb);
    return sum;
}

double dbFromPower (double e) noexcept { return 10.0 * std::log10 (std::max (e, 1.0e-14)); }
double dbFromLevel (double a) noexcept { return 20.0 * std::log10 (std::max (a, 1.0e-7)); }
double gainFromDb (double db) noexcept { return std::pow (10.0, db * 0.05); }
} // namespace

//==============================================================================
struct DeLimiter::Impl
{
    using Mailbox = maxi::LearnCaptureMailbox;

    double rate = 44100.0;
    int hop = kHopAt44k, latency = kLatencyHops * kHopAt44k;
    double slowCoef = 0.0, clipCoef = 0.0, glide = 0.0, hopGlide = 0.0, flatStepScale = 1.0, holdHops = 0.0;

    std::array<std::vector<float>, 2> delay;
    int writePos = 0;

    // the hop being collected
    int hopCount = 0, historyCount = 0;
    double hopEnergy = 0.0, hopPeak = 0.0, prevMid = 0.0;
    int hopFlat = 0;
    float historyInput = 0.0f, historyLift = 0.0f;

    // windows
    std::array<double, kWindowHops> energies {}, peaks {};
    std::array<double, kSmoothHops> smooth {};
    int windowPos = 0, smoothPos = 0;
    double slow = 0.0, clipShare = 0.0;

    // the curve as it reaches the audio
    double gainNow = 1.0, gainStep = 0.0, gainTarget = 1.0;

    // parameters
    double inputTarget = 1.0, outputTarget = 1.0, amountTarget = 0.0, thresholdTarget = 0.0;
    double inputNow = 1.0, outputNow = 1.0, amountNow = 0.0, thresholdNow = 0.0, thresholdLevel = 1.0;
    bool snapParams = true;

    // meters
    DeLimiterMeters meters;
    StereoSampleFifo history;
    double heldLift = 0.0, holdLeft = 0.0;

    // learn: the audio thread owns these three while listening
    Mailbox learn;
    unsigned audioGeneration = 0;
    bool learnStarted = false;
    int learnCount = 0;
    double learnPeak = 0.0;
    std::atomic<int> learnLength { 1 };   // read by the UI for the progress
    std::atomic<float> learnResultDb { 0.0f };

    // Allocated once, here: the panel reads the FIFO on its own timer, so the storage must survive a re-prepare.
    Impl() { history.prepare (1 << 13); }

    void prepare (double sampleRate)
    {
        rate = sampleRate > 0.0 ? sampleRate : 44100.0;
        hop = hopFor (rate);
        latency = kLatencyHops * hop;
        const double hopMs = 1000.0 * hop / rate;
        slowCoef = std::exp (-hopMs / kSlowMs);
        clipCoef = std::exp (-hopMs / kClipMs);
        glide = 1.0 - std::exp (-1000.0 / (kGlideMs * rate));
        hopGlide = 1.0 - std::exp (-hopMs / kGlideMs);
        flatStepScale = 44100.0 / rate;
        holdHops = 1000.0 / hopMs;
        learnLength.store (std::max (1, (int) std::lround (kDeLimiterLearnSeconds * rate)), std::memory_order_relaxed);
        for (auto& d : delay)
            d.assign ((size_t) latency + 1, 0.0f);
        learn.cancel();
        audioGeneration = 0;
        resetState();
    }

    void resetState() noexcept
    {
        for (auto& d : delay)
            std::fill (d.begin(), d.end(), 0.0f);
        writePos = 0;
        hopCount = historyCount = hopFlat = 0;
        hopEnergy = hopPeak = prevMid = 0.0;
        historyInput = historyLift = 0.0f;
        energies.fill (0.0);
        peaks.fill (0.0);
        smooth.fill (1.0);
        windowPos = smoothPos = 0;
        slow = clipShare = 0.0;
        gainNow = gainTarget = 1.0;
        gainStep = 0.0;
        heldLift = holdLeft = 0.0;
        meters.gainDb.store (0.0f);
        meters.heldGainDb.store (0.0f);
        snapParams = true;
    }

    // The gain model (dB) for one hop's features.
    double modelDb (double r, double p, double s, double c, double thresholdDb, double share) const noexcept
    {
        const double gate = std::clamp ((p - thresholdDb - kGateFromDb) / (kGateToDb - kGateFromDb), 0.0, 1.0);
        if (gate <= 0.0)
            return 0.0;
        const double sum = hingeSum (kRmsHinges, r - thresholdDb) + hingeSum (kPeakHinges, p - thresholdDb)
                           + hingeSum (kSlowHinges, s - thresholdDb) + hingeSum (kContrastHinges, c);
        const double base = sum * gate;
        if (base <= 0.0)
            return 0.0;
        const double clip = std::clamp ((share - kClipShareFrom) / (kClipShareTo - kClipShareFrom), 0.0, 1.0);
        const double lifted = base * (1.0 + kClipBoost * clip);
        return std::min (kMaxGainDb, kSpikeScale * std::pow (lifted, kSpikePower));
    }

    void endHop() noexcept
    {
        const double energy = hopEnergy / hop;
        energies[(size_t) windowPos] = energy;
        peaks[(size_t) windowPos] = hopPeak;
        windowPos = (windowPos + 1) % kWindowHops;

        double energySum = 0.0, peak = 0.0;
        for (int i = 0; i < kWindowHops; ++i)
        {
            energySum += energies[(size_t) i];
            peak = std::max (peak, peaks[(size_t) i]);
        }
        slow = slowCoef * slow + (1.0 - slowCoef) * energy;
        clipShare = clipCoef * clipShare + (1.0 - clipCoef) * ((double) hopFlat / hop);
        if (slow < 1.0e-30)
            slow = 0.0;      // long silence: no denormals
        if (clipShare < 1.0e-30)
            clipShare = 0.0;

        const double target = std::abs (thresholdNow - thresholdTarget) < 1.0e-4 ? thresholdTarget
                              : thresholdNow + (thresholdTarget - thresholdNow) * hopGlide;
        if (target != thresholdNow)
        {
            thresholdNow = target;
            thresholdLevel = gainFromDb (thresholdNow);
        }

        const double r = dbFromPower (energySum / kWindowHops), s = dbFromPower (slow);
        const double gainDb = modelDb (r, dbFromLevel (peak), s, r - s, thresholdNow, clipShare);
        smooth[(size_t) smoothPos] = gainDb > 0.0 ? gainFromDb (gainDb) : 1.0;
        smoothPos = (smoothPos + 1) % kSmoothHops;

        gainNow = gainTarget;   // exact at every hop, so a run of 1.0 stays exactly 1.0
        gainTarget = (smooth[0] + smooth[1] + smooth[2]) / 3.0;
        gainStep = (gainTarget - gainNow) / hop;

        // meters
        const double lift = 1.0 + amountNow * (gainTarget - 1.0);
        const double liftDb = lift > 1.0 ? 20.0 * std::log10 (lift) : 0.0;
        meters.gainDb.store ((float) liftDb);
        if (liftDb >= heldLift)
        {
            heldLift = liftDb;
            holdLeft = holdHops;
        }
        else if ((holdLeft -= 1.0) <= 0.0)
        {
            heldLift = liftDb;
        }
        meters.heldGainDb.store ((float) heldLift);

        historyLift = std::max (historyLift, (float) liftDb);
        if (++historyCount >= kHistoryHops)
        {
            const float inputDb = historyInput > 1.0e-6f ? 20.0f * std::log10 (historyInput) : -120.0f;
            history.push (&inputDb, &historyLift, 1);
            historyCount = 0;
            historyInput = historyLift = 0.0f;
        }

        hopEnergy = hopPeak = 0.0;
        hopFlat = 0;
        hopCount = 0;
    }

    void process (float* const* io, int numSamples) noexcept
    {
        // Learn request for this block. A restart from the message thread changes the generation.
        const auto request = learn.load();
        const bool listening = Mailbox::state (request) == Mailbox::Listening;
        if (listening && Mailbox::generation (request) != audioGeneration)
        {
            audioGeneration = Mailbox::generation (request);
            learnStarted = false;
            learnCount = 0;
            learnPeak = 0.0;
        }
        const int length = learnLength.load (std::memory_order_relaxed);
        bool capturing = listening && learnCount < length;

        const int size = (int) delay[0].size();
        const double flatStep = kFlatStep * flatStepScale;

        for (int n = 0; n < numSamples; ++n)
        {
            double x[2] { (double) io[0][n], (double) io[1][n] };
            for (auto& v : x)
                if (! std::isfinite (v) || std::abs (v) > 1.0e6)
                    v = 0.0;

            inputNow += (inputTarget - inputNow) * glide;
            if (std::abs (inputNow - inputTarget) < 1.0e-7)
                inputNow = inputTarget;
            outputNow += (outputTarget - outputNow) * glide;
            if (std::abs (outputNow - outputTarget) < 1.0e-7)
                outputNow = outputTarget;
            amountNow += (amountTarget - amountNow) * glide;
            if (std::abs (amountNow - amountTarget) < 1.0e-7)
                amountNow = amountTarget;

            x[0] *= inputNow;
            x[1] *= inputNow;
            const double loudest = std::max (std::abs (x[0]), std::abs (x[1]));

            if (capturing)
            {
                learnStarted = learnStarted || loudest > kLearnSilence;
                if (learnStarted)
                {
                    learnPeak = std::max (learnPeak, loudest);
                    capturing = ++learnCount < length;
                }
            }

            const double mid = 0.5 * (x[0] + x[1]);
            const double level = std::abs (mid);
            hopEnergy += mid * mid;
            hopPeak = std::max (hopPeak, level);
            if (std::abs (mid - prevMid) < flatStep * thresholdLevel && level > kFlatLevel * thresholdLevel)
                ++hopFlat;
            prevMid = mid;
            historyInput = std::max (historyInput, (float) loudest);

            // Delay D, then the lift.
            int readPos = writePos + 1;
            if (readPos >= size)
                readPos = 0;
            const double lift = (1.0 + amountNow * (gainNow - 1.0)) * outputNow;
            delay[0][(size_t) writePos] = (float) x[0];
            delay[1][(size_t) writePos] = (float) x[1];
            io[0][n] = (float) ((double) delay[0][(size_t) readPos] * lift);
            io[1][n] = (float) ((double) delay[1][(size_t) readPos] * lift);
            writePos = readPos;

            gainNow += gainStep;
            if (++hopCount >= hop)
                endHop();
        }

        if (listening)
        {
            if (learnCount >= length)
            {
                const double db = learnPeak > 0.0 ? 20.0 * std::log10 (learnPeak) : (double) kDeLimiterMinThresholdDb;
                learnResultDb.store ((float) std::clamp (db, (double) kDeLimiterMinThresholdDb, (double) kDeLimiterMaxThresholdDb),
                                     std::memory_order_relaxed);
                learn.publish (request, (unsigned) learnCount, Mailbox::Captured);
            }
            else
            {
                learn.publish (request, (unsigned) learnCount, Mailbox::Listening);
            }
        }
    }
};

//==============================================================================
DeLimiter::DeLimiter() : impl (std::make_unique<Impl>()) {}
DeLimiter::~DeLimiter() = default;

void DeLimiter::prepare (double sampleRate, int) { impl->prepare (sampleRate); }
void DeLimiter::reset() noexcept { impl->resetState(); }

void DeLimiter::setParams (ParamValues values) noexcept
{
    DeLimiterParams p;
    readDeLimiterParams (values, p);
    auto& m = *impl;
    m.inputTarget = gainFromDb (p.inputGainDb);
    m.outputTarget = gainFromDb (p.outputGainDb);
    m.amountTarget = p.amountPercent * 0.01;
    m.thresholdTarget = p.thresholdDb;
    if (m.snapParams)
    {
        m.snapParams = false;
        m.inputNow = m.inputTarget;
        m.outputNow = m.outputTarget;
        m.amountNow = m.amountTarget;
        m.thresholdNow = m.thresholdTarget;
        m.thresholdLevel = gainFromDb (m.thresholdNow);
    }
}

void DeLimiter::process (float* const* channels, int numSamples, const ProcessContext&) noexcept
{
    impl->process (channels, numSamples);
}

int DeLimiter::getLatencySamples() const noexcept { return impl->latency; }
int DeLimiter::latencyFor (double sampleRate) noexcept { return kLatencyHops * hopFor (sampleRate); }
const DeLimiterMeters& DeLimiter::getMeters() const noexcept { return impl->meters; }
StereoSampleFifo& DeLimiter::getHistoryFifo() noexcept { return impl->history; }

void DeLimiter::startLearn() noexcept { impl->learn.start(); }
void DeLimiter::cancelLearn() noexcept { impl->learn.cancel(); }

bool DeLimiter::isLearning() const noexcept
{
    return Impl::Mailbox::state (impl->learn.load()) != Impl::Mailbox::Idle;
}

bool DeLimiter::isWaitingForAudio() const noexcept
{
    const auto token = impl->learn.load();
    return Impl::Mailbox::state (token) == Impl::Mailbox::Listening && Impl::Mailbox::count (token) == 0;
}

float DeLimiter::getLearnProgress() const noexcept
{
    return (float) Impl::Mailbox::count (impl->learn.load()) / (float) std::max (1, impl->learnLength.load (std::memory_order_relaxed));
}

bool DeLimiter::takeLearnedThreshold (float& thresholdDb) noexcept
{
    const auto token = impl->learn.load();
    if (Impl::Mailbox::state (token) != Impl::Mailbox::Captured)
        return false;
    thresholdDb = impl->learnResultDb.load (std::memory_order_relaxed);
    impl->learn.publish (token, Impl::Mailbox::count (token), Impl::Mailbox::Idle);
    return true;
}

} // namespace ps
