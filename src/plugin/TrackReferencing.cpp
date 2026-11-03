#include "plugin/TrackReferencing.h"

#include "assist/AssistFeatures.h"
#include "core/Loudness.h"
#include "core/Resampler.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

#if JUCE_INTEL
 #include <immintrin.h>
#endif

//==============================================================================
// Track Referencing (behaviour: TrackReferencing.h and ARCHITECTURE.md, "Track Referencing (phase 3)").
//
// The playback engine, the slot hand-over and the analysis thread are RefMatch's (the family's reference-track matcher),
// reduced to a stereo bus and extended to a list of references with five loops each. Choices made here:
// - One analysis per file: ps::assist::analyse on the whole decoded file gives the TrackProfile the comparison shows
//   for "whole track" and the Assistant's ReferenceTarget. Loop regions get their own TrackProfile (ps::analyseRegion),
//   cached per letter until that loop changes.
// - Memory: decoded audio of the selected reference always stays; the others stay while the total fits the budget
//   (least recently selected evicted first). An evicted reference keeps everything but its audio.
// - Loads run one at a time on the loader thread: the selected reference's load first, then a pending region analysis,
//   then the other loads in the order they were asked for.
// - Mono / Swap of the I/O panel are applied to the reference (ramped like the master's), and the reference is measured
//   as heard after them, the same way the master is measured after the chain's own mono / swap.
//
// Locks (a thread may take a lock further down while holding one further up, never the reverse):
//   stateMutex     the list, jobs, serials, the host rate (message, loader and analysis threads)
//   analysisMutex  the master analyser, the reading side of the FIFO, the matching-gain state
//   resultsMutex   published profiles and comparison
//   slotLock       juce::SpinLock around the track + region the audio thread plays (try-locked on the audio thread)
// Tracks are never destroyed on the audio thread or while holding a lock (Graveyard).
//==============================================================================

namespace psplug
{
namespace
{
const juce::Identifier kTag ("TrackReferencing"), kRefTag ("Ref");
constexpr int kStateVersion = 1;

constexpr int kAnalysisIntervalMs = 15;
constexpr double kPublishIntervalMs = 30.0;
constexpr double kFifoSeconds = 2.0;
constexpr double kFallbackSampleRate = 44100.0;
constexpr int kMinChunk = 1024;
constexpr int kMaxVoices = 8;
constexpr int kMaxRetired = kMaxVoices;
constexpr long long kNoSilence = std::numeric_limits<long long>::min();
constexpr double kHostSyncToleranceMs = 5.0;
constexpr double kShortTermValidSeconds = 3.2;
constexpr double kSilentMeanSquare = 1.0e-7;
constexpr int kSlotLockAttempts = 32;
constexpr int kDecodeChunk = 1 << 16;
constexpr int kResampleChunk = 1 << 15;
constexpr double kMinReferenceSeconds = 0.5;

long long floorMod (long long value, long long modulus) noexcept
{
    const auto r = value % modulus;
    return r < 0 ? r + modulus : r;
}

inline void cpuRelax() noexcept
{
   #if JUCE_INTEL
    _mm_pause();
   #endif
}

// Audio thread: length of the current run of continuous, non-silent audio, judged in exact 100 ms steps.
struct RunCounter
{
    long long run = 0;
    double stepEnergy = 0.0;
    int stepSamples = 0;

    void gap() noexcept { *this = RunCounter(); }

    void add (const float* l, const float* r, int numSamples, int stepLength) noexcept
    {
        for (int done = 0; done < numSamples;)
        {
            const int n = juce::jmin (numSamples - done, stepLength - stepSamples);
            double sum = 0.0;
            for (int i = 0; i < n; ++i)
                sum += (double) l[done + i] * (double) l[done + i] + (double) r[done + i] * (double) r[done + i];

            stepEnergy += sum * 0.5;
            stepSamples += n;
            run += n;
            done += n;

            if (stepSamples >= stepLength)
            {
                if (! (stepEnergy >= kSilentMeanSquare * (double) stepSamples))   // also NaN
                    run = 0;
                stepEnergy = 0.0;
                stepSamples = 0;
            }
        }
    }
};

// table[k] = sin (pi/2 * k / length), k = 0..length (equal-power fade; end points exactly 0 and 1).
std::vector<float> makeQuarterSine (int length)
{
    std::vector<float> table ((size_t) length + 1);
    for (int k = 0; k <= length; ++k)
        table[(size_t) k] = (float) std::sin (juce::MathConstants<double>::halfPi * (double) k / (double) length);
    table.front() = 0.0f;
    table.back() = 1.0f;
    return table;
}

int samplesForMs (double ms, double sampleRate)
{
    return juce::jmax (1, (int) std::lround (ms * sampleRate / 1000.0));
}

using TrackPtr = std::shared_ptr<const RefTrack>;

// Tracks taken out of shared state while holding locks; destroyed after the locks are released (declare before them).
struct Graveyard
{
    std::vector<TrackPtr> tracks;
    void add (TrackPtr t)
    {
        if (t != nullptr)
            tracks.push_back (std::move (t));
    }
};

juce::String loopToString (const TrackReferencing::Loop& l)
{
    return l.set ? juce::String (l.start, 6) + "," + juce::String (l.end, 6) : juce::String();
}

TrackReferencing::Loop loopFromString (const juce::String& s)
{
    TrackReferencing::Loop l;
    if (! s.containsChar (','))
        return l;
    const double a = s.upToFirstOccurrenceOf (",", false, false).getDoubleValue();
    const double b = s.fromFirstOccurrenceOf (",", false, false).getDoubleValue();
    if (std::isfinite (a) && std::isfinite (b) && b > a && a >= 0.0)
        l = { true, a, b };
    return l;
}

bool isMessageThread()
{
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    return mm == nullptr || mm->isThisTheMessageThread();
}
} // namespace

//==============================================================================
struct TrackReferencing::Impl
{
    using State = TrackReferencing::State;

    struct EntryData
    {
        int id = 0;
        juce::File file;
        juce::String name;
        State state = State::Loading;
        juce::String error;
        double lengthSeconds = 0.0, fileSampleRate = 0.0;
        int fileChannels = 0;
        std::array<Loop, kNumLoops> loops {};
        int activeLoop = -1;

        bool analysed = false;                 // profile + target valid
        ps::TrackProfile whole;
        ps::assist::ReferenceTarget target;
        std::array<std::optional<ps::TrackProfile>, kNumLoops> loopProfiles;
        bool analysing = false;                // a region job for this entry is pending / running
        int analysingLetter = -1;
        Loop analysingLoop;

        std::shared_ptr<const RefDrawData> draw;
        TrackPtr track;                        // resident audio (nullptr = evicted / not loaded)
        juce::uint64 serial = 0;               // bumps on relink / remove: cancels loads in flight
        bool loadQueued = false;
        bool analysisUnreported = false;       // analysed, but the decode was redone (rate change): report it then
        long long lastUsed = 0;
    };

    struct LoadJob
    {
        int id = 0;
        juce::File file;
        double sampleRate = 0.0;
        juce::uint64 serial = 0;
        bool analyse = true;
    };

    struct RegionJob
    {
        int id = 0;
        int letter = -1;
        Loop loop;
        TrackPtr track;
        juce::uint64 serial = 0;               // regionSerial at request
        juce::uint64 entrySerial = 0;          // the entry's serial (a relink makes the result stale)
    };

    struct Event
    {
        int id = 0;
        bool ok = false;
        juce::uint64 serial = 0;               // the entry's serial when it happened (stale after a relink)
    };

    struct Slot
    {
        struct Retired
        {
            TrackPtr track;
            juce::uint32 gen = 0;
            bool releasable = false;
        };

        TrackPtr current;
        juce::uint32 currentGen = 0;
        long long regionStart = 0, regionEnd = 0;
        juce::uint32 regionGen = 0;
        std::array<Retired, kMaxRetired> retired {};
        juce::uint32 audioSeenGen = 0;
    };

    struct Target
    {
        juce::uint32 gen = 0;
        long long pos = 0;
        bool active = false;
    };

    struct Voice
    {
        Target head;
        bool used = false;
        bool rising = true;
        int fade = 0;
        long long silentBefore = kNoSilence;
    };

    struct TrackView
    {
        const float* left = nullptr;
        const float* right = nullptr;
        long long length = 0;

        void read (long long pos, float& l, float& r) const noexcept
        {
            if (pos >= 0 && pos < length)
            {
                l = left[pos];
                r = right[pos];
            }
        }
    };

    struct HostPosition
    {
        bool playing = false;
        bool hasTime = false;
        long long sample = 0;
    };

    struct AudioState
    {
        std::array<Voice, kMaxVoices> voices {};
        int mainVoice = 0;
        juce::uint32 seenTrackGen = 0;
        int seenSeekCount = 0;
        long long seekOffset = 0;
        int listenStep = 0;
        int monoStep = 0;
        float lastTargetDb = 0.0f;
        bool mainSounding = false;
        bool haveTrack = false;
        bool transportSeen = false;
        RunCounter mixRun, refRun;
        long long refTrackSamples = 0;
        juce::uint32 regionGen = 0;
    };

    class WorkerThread final : public juce::Thread
    {
    public:
        WorkerThread (const juce::String& name, std::function<void()> bodyToRun)
            : juce::Thread (name), body (std::move (bodyToRun)) {}
        void run() override { body(); }

    private:
        std::function<void()> body;
    };

    //==============================================================================
    explicit Impl (juce::AudioProcessor& p) : owner (p)
    {
        formats.registerBasicFormats();
        comparison = ps::compareProfiles (mixProfile, refProfile);
        listenTable = jumpTable = makeQuarterSine (1);
        resetAudioState();
        cachedState = buildStateTree();
        loaderThread.startThread (juce::Thread::Priority::low);
    }

    ~Impl()
    {
        // Never forced: a load stops at its next cancellation check (the whole-file analysis of a 20-minute file can
        // take a few seconds; JUCE's forced termination would skip destructors and could hold the heap lock).
        loaderThread.signalThreadShouldExit();
        loaderThread.notify();
        analysisThread.stopThread (-1);
        loaderThread.stopThread (-1);
    }

    double targetSampleRate() const noexcept
    {
        const double rate = hostSampleRate.load();
        return rate > 0.0 ? rate : kFallbackSampleRate;
    }

    //==============================================================================
    // Audio thread
    void process (float* left, float* right, int numSamples, bool monoTarget, bool swap) noexcept
    {
        if (numSamples <= 0 || refScratch.getNumSamples() == 0)
            return;

        const auto host = readHostPosition();
        hostPlaying.store (host.playing, std::memory_order_relaxed);

        // While a host transport that has played is stopped, the output is not part of the master: leave it out of the
        // analysis. Hosts that never report playing (the Standalone app) and offline renders feed everything.
        audio.transportSeen = audio.transportSeen || host.playing;
        const bool nonRealtime = owner.isNonRealtime();
        const bool feedAnalysis = feedMaster.load (std::memory_order_relaxed)
                                  && (nonRealtime || host.playing || ! audio.transportSeen);

        if (feedAnalysis)
        {
            audio.mixRun.add (left, right, numSamples, silenceStepLength);
            if (audio.mixRun.run < shortTermValidSamples)
                mixWindowFull.store (false, std::memory_order_release);
            if (! pushToFifo (left, right, numSamples))
                audio.mixRun.gap();
        }
        else
        {
            audio.mixRun.gap();
        }

        mixWindowFull.store (audio.mixRun.run >= shortTermValidSamples, std::memory_order_release);

        if (nonRealtime)
        {
            // Offline render: always the master. The reference does not play.
            audio.refRun.gap();
            refMatchShortTermLufs.store (ps::kNoValue);
            return;
        }

        const bool listenTarget = listen.load (std::memory_order_relaxed);
        const auto flags = audioSettings.load (std::memory_order_relaxed);
        const bool sync = (flags & 1) != 0, freeRun = (flags & 2) != 0;

        // Level match Off is 0 dB at once (the analysis thread's target follows within 15 ms).
        const float targetDb = levelMatchSetting.load (std::memory_order_relaxed) == (int) ps::LevelMatch::Off
                                   ? 0.0f
                                   : targetGainDb.load (std::memory_order_relaxed);
        if (targetDb != audio.lastTargetDb)
        {
            audio.lastTargetDb = targetDb;
            gain.setTargetValue ((float) ps::dbToGain (targetDb));
        }

        const int capacity = refScratch.getNumSamples();

        for (int start = 0; start < numSamples; start += capacity)
        {
            const int n = juce::jmin (capacity, numSamples - start);
            auto chunkHost = host;
            chunkHost.sample += start;
            if (! sync)
                chunkHost.hasTime = false;

            if (renderReference (n, chunkHost, freeRun))
            {
                monitorReference (n, monoTarget, swap);
                measureReference (n);
            }
            else
            {
                audio.refRun.gap();
            }

            // No installed track: the A/B target is the master whatever Listen says.
            mixChunk (left + start, right + start, n, listenTarget && audio.haveTrack);
        }

        matchGainDb.store (ps::gainToDb (gain.getCurrentValue()), std::memory_order_relaxed);
        listenPosition.store ((float) audio.listenStep / (float) listenLength, std::memory_order_relaxed);
        referenceSounding.store (audio.mainSounding && audio.listenStep > 0, std::memory_order_relaxed);

        const float refShortTerm = refMeter.shortTermLufs();
        refShortTermLufs.store (audio.refTrackSamples >= shortTermValidSamples ? refShortTerm : ps::kNoValue,
                                std::memory_order_relaxed);
        refMatchShortTermLufs.store (audio.refRun.run >= shortTermValidSamples
                                             && refShortTerm > ps::LoudnessMeter::kAbsoluteGateLufs
                                         ? refShortTerm
                                         : ps::kNoValue);
        audioRegionGen.store (audio.regionGen);
    }

    // Swap and the mono fold-down (ramped over kMonoFadeMs) of the rendered reference, as the I/O panel does to the
    // master.
    void monitorReference (int n, bool monoTarget, bool swap) noexcept
    {
        float* l = refScratch.getWritePointer (0);
        float* r = refScratch.getWritePointer (1);

        if (swap)
            for (int i = 0; i < n; ++i)
                std::swap (l[i], r[i]);

        if (audio.monoStep == 0 && ! monoTarget)
            return;

        for (int i = 0; i < n; ++i)
        {
            if (monoTarget)
            {
                if (audio.monoStep < monoLength)
                    ++audio.monoStep;
            }
            else if (audio.monoStep > 0)
            {
                --audio.monoStep;
            }

            const float mid = (l[i] + r[i]) * 0.5f;
            const float m = (float) audio.monoStep / (float) monoLength;
            l[i] += m * (mid - l[i]);
            r[i] += m * (mid - r[i]);
        }
    }

    void measureReference (int n) noexcept
    {
        if (! audio.mainSounding)
        {
            audio.refRun.gap();
            return;
        }

        const float* l = refScratch.getReadPointer (0);
        const float* r = refScratch.getReadPointer (1);
        refMeter.process (l, r, n);
        audio.refRun.add (l, r, n, silenceStepLength);
        audio.refTrackSamples += n;
    }

    bool pushToFifo (const float* l, const float* r, int numSamples) noexcept
    {
        if (fifoData[0] == nullptr || fifo.getFreeSpace() < numSamples)
        {
            droppedBlocks.fetch_add (1, std::memory_order_relaxed);
            return false;
        }

        const auto scope = fifo.write (numSamples);
        auto copy = [&] (int fifoStart, int size, int sourceOffset)
        {
            if (size <= 0)
                return;
            juce::FloatVectorOperations::copy (fifoData[0] + fifoStart, l + sourceOffset, size);
            juce::FloatVectorOperations::copy (fifoData[1] + fifoStart, r + sourceOffset, size);
        };
        copy (scope.startIndex1, scope.blockSize1, 0);
        copy (scope.startIndex2, scope.blockSize2, scope.blockSize1);
        return true;
    }

    HostPosition readHostPosition() const noexcept
    {
        HostPosition h;
        if (auto* ph = owner.getPlayHead())
        {
            if (const auto pos = ph->getPosition())
            {
                h.playing = pos->getIsPlaying();
                if (const auto samples = pos->getTimeInSamples())
                {
                    h.sample = *samples;
                    h.hasTime = true;
                }
                else if (const auto seconds = pos->getTimeInSeconds())
                {
                    h.sample = (long long) std::llround (*seconds * sampleRate);
                    h.hasTime = true;
                }
                else if (const auto ppq = pos->getPpqPosition())
                {
                    if (const auto bpm = pos->getBpm(); bpm.hasValue() && *bpm > 0.0)
                    {
                        h.sample = (long long) std::llround (*ppq * 60.0 / *bpm * sampleRate);
                        h.hasTime = true;
                    }
                }
            }
        }
        return h;
    }

    TrackView viewOf (juce::uint32 gen) const noexcept   // slotLock held
    {
        const RefTrack* t = nullptr;
        if (gen != 0 && gen == slot.currentGen)
            t = slot.current.get();
        else if (gen != 0)
            for (auto& r : slot.retired)
                if (r.track != nullptr && r.gen == gen)
                    t = r.track.get();

        if (t == nullptr)
            return {};
        return { t->audio.getReadPointer (0), t->audio.getReadPointer (1), t->numSamples() };
    }

    Voice& mainVoice() noexcept { return audio.voices[(size_t) audio.mainVoice]; }

    void resetAudioState() noexcept
    {
        audio = AudioState();
        audio.voices[0].used = true;
        audio.voices[0].fade = jumpLength;
    }

    int startJump (const Target& target, long long silentBefore = kNoSilence) noexcept
    {
        auto& current = mainVoice();
        if (current.head.active && current.fade > 0)
            current.rising = false;
        else
            current.used = false;

        int index = -1;
        for (int i = 0; i < kMaxVoices && index < 0; ++i)
            if (! audio.voices[(size_t) i].used)
                index = i;

        if (index < 0)
        {
            index = 0;
            for (int i = 1; i < kMaxVoices; ++i)
                if (audio.voices[(size_t) i].fade < audio.voices[(size_t) index].fade)
                    index = i;
        }

        auto& v = audio.voices[(size_t) index];
        v.head = target;
        v.used = true;
        v.rising = true;
        v.fade = 0;
        v.silentBefore = silentBefore;
        audio.mainVoice = index;
        return index;
    }

    // Renders n samples of the (unity gain) reference into refScratch and advances the voices (RefMatch's engine).
    // Returns false when the slot could not be locked (the chunk is silent, nothing advanced).
    bool renderReference (int n, const HostPosition& host, bool freeRun) noexcept
    {
        float* outL = refScratch.getWritePointer (0);
        float* outR = refScratch.getWritePointer (1);

        const juce::SpinLock::ScopedTryLockType lock (slotLock);
        for (int attempt = 1; attempt < kSlotLockAttempts && ! lock.isLocked(); ++attempt)
        {
            cpuRelax();
            lock.retryLock();
        }

        if (! lock.isLocked())
        {
            referenceDropouts.fetch_add (1, std::memory_order_relaxed);
            juce::FloatVectorOperations::clear (outL, n);
            juce::FloatVectorOperations::clear (outR, n);
            return false;
        }

        const RefTrack* track = slot.current.get();
        const juce::uint32 currentGen = slot.currentGen;
        const long long regionStart = slot.regionStart, regionEnd = slot.regionEnd;
        const long long regionLength = regionEnd - regionStart;
        const bool haveTrack = track != nullptr && regionLength > 0;
        audio.haveTrack = haveTrack;
        audio.regionGen = slot.regionGen;

        // Loop wrap: crossfade from the continuation after the region into its start; a region that ends at the end
        // of the file wraps jumpLength early into the lead-in before its start (see RefMatch's ARCHITECTURE).
        const bool lateWrap = (haveTrack && regionEnd + jumpLength <= track->numSamples())
                              || regionLength <= 2 * (long long) jumpLength;
        const long long wrapAt = lateWrap ? regionEnd : regionEnd - jumpLength;
        const long long leadIn = lateWrap ? 0 : jumpLength;
        auto inRegion = [&] (long long p) { return p >= regionStart - leadIn && p < regionEnd; };
        auto silenceFor = [&] (long long p) { return p < regionStart && regionStart < jumpLength ? regionStart : kNoSilence; };

        if (currentGen != audio.seenTrackGen)
        {
            audio.seenTrackGen = currentGen;
            audio.seekOffset = 0;
            audio.refRun.gap();
            audio.refTrackSamples = 0;
        }

        slot.audioSeenGen = currentGen;

        long long seekTarget = -1;
        // A seek waits while the selected reference is still decoding; selecting another reference drops it.
        const int seeks = seekCount.load (std::memory_order_acquire);
        if (seeks != audio.seenSeekCount)
        {
            if (seeks - seekDroppedUpTo.load (std::memory_order_acquire) <= 0)
            {
                audio.seenSeekCount = seeks;
            }
            else if (haveTrack)
            {
                audio.seenSeekCount = seeks;
                seekTarget = juce::jlimit (regionStart, regionEnd - 1,
                                           (long long) std::llround (seekSeconds.load() * track->sampleRate));
            }
        }

        const auto& mainHead = mainVoice().head;
        Target want { currentGen, mainHead.pos, false };
        bool jump = false;

        if (! haveTrack)
        {
            want.pos = 0;
            jump = mainHead.gen != currentGen || mainHead.active;
        }
        else if (host.playing && host.hasTime)
        {
            if (seekTarget >= 0)
                audio.seekOffset = floorMod (seekTarget - regionStart - host.sample, regionLength);

            want.pos = regionStart + floorMod (host.sample + audio.seekOffset, regionLength);
            want.active = true;

            if (mainHead.gen == currentGen && mainHead.active && inRegion (mainHead.pos))
            {
                const long long d = floorMod (mainHead.pos - want.pos, regionLength);
                jump = juce::jmin (d, regionLength - d) > syncTolerance;
            }
            else
            {
                jump = true;
            }
        }
        else
        {
            if (mainHead.gen != currentGen || ! inRegion (want.pos))
                want.pos = regionStart;
            if (seekTarget >= 0)
                want.pos = seekTarget;

            want.active = host.playing || freeRun;
            jump = want.gen != mainHead.gen || want.active != mainHead.active || want.pos != mainHead.pos;
        }

        if (jump)
        {
            if (! want.active && ! mainHead.active)
            {
                mainVoice().head = want;
                mainVoice().silentBefore = silenceFor (want.pos);
            }
            else
            {
                startJump (want, silenceFor (want.pos));
            }
        }

        std::array<TrackView, kMaxVoices> views;
        for (size_t v = 0; v < views.size(); ++v)
            if (audio.voices[v].used)
                views[v] = viewOf (audio.voices[v].head.gen);

        for (int i = 0; i < n; ++i)
        {
            float l = 0.0f, r = 0.0f;

            for (size_t v = 0; v < audio.voices.size(); ++v)
            {
                auto& voice = audio.voices[v];
                if (! voice.used)
                    continue;

                const bool audible = voice.head.active && voice.head.pos >= voice.silentBefore;
                if (audible)
                {
                    float vl = 0.0f, vr = 0.0f;
                    views[v].read (voice.head.pos, vl, vr);
                    const float w = jumpTable[(size_t) voice.fade];
                    l += vl * w;
                    r += vr * w;
                }

                if (voice.head.active)
                    ++voice.head.pos;

                if (voice.rising)
                {
                    if (audible && voice.fade < jumpLength)
                        ++voice.fade;
                }
                else if (--voice.fade <= 0)
                {
                    voice.used = false;
                }
            }

            outL[i] = l;
            outR[i] = r;

            const auto& m = mainVoice().head;
            if (haveTrack && m.active && m.gen == currentGen && m.pos >= wrapAt)
            {
                const long long pos = m.pos - regionLength;
                const int index = startJump ({ currentGen, pos, true }, silenceFor (pos));
                views[(size_t) index] = viewOf (currentGen);
            }
        }

        const auto& m = mainVoice().head;
        const bool onCurrent = haveTrack && m.gen == currentGen;
        audio.mainSounding = onCurrent && m.active;

        if (onCurrent)
        {
            const long long shown = m.pos < regionStart ? m.pos + regionLength : m.pos;
            playheadSeconds.store ((double) shown / track->sampleRate, std::memory_order_relaxed);
        }
        else if (! haveTrack)
        {
            playheadSeconds.store (0.0, std::memory_order_relaxed);
        }

        for (auto& r : slot.retired)
        {
            if (r.track == nullptr)
                continue;
            bool inUse = false;
            for (auto& v : audio.voices)
                inUse = inUse || (v.used && v.head.active && v.head.gen == r.gen);
            r.releasable = ! inUse;
        }

        return true;
    }

    // A/B crossfade (+ matching gain) of one chunk, in place. Settled on the master: untouched.
    void mixChunk (float* outL, float* outR, int n, bool listenTarget) noexcept
    {
        const float* refL = refScratch.getReadPointer (0);
        const float* refR = refScratch.getReadPointer (1);

        if (audio.listenStep == 0 && ! listenTarget)
        {
            gain.skip (n);
            return;
        }

        for (int i = 0; i < n; ++i)
        {
            if (listenTarget)
            {
                if (audio.listenStep < listenLength)
                    ++audio.listenStep;
            }
            else if (audio.listenStep > 0)
            {
                --audio.listenStep;
            }

            const float g = gain.getNextValue();
            if (audio.listenStep == 0)
                continue;

            const float refLeft = refL[i] * g, refRight = refR[i] * g;

            if (audio.listenStep == listenLength)
            {
                outL[i] = refLeft;
                outR[i] = refRight;
            }
            else
            {
                const float wRef = listenTable[(size_t) audio.listenStep];
                const float wMix = listenTable[(size_t) (listenLength - audio.listenStep)];
                outL[i] = outL[i] * wMix + refLeft * wRef;
                outR[i] = outR[i] * wMix + refRight * wRef;
            }
        }
    }

    //==============================================================================
    // Slot (track hand-over to the audio thread)
    void installTrack (TrackPtr track, long long regionStart, long long regionEnd, Graveyard& graveyard)
    {
        const juce::uint32 gen = nextTrackGen++;
        const bool keepForFade = audioPrepared.load();
        const juce::SpinLock::ScopedLockType lock (slotLock);

        for (auto& r : slot.retired)
        {
            if (r.track != nullptr && (r.releasable || ! keepForFade))
            {
                graveyard.add (std::move (r.track));
                r = {};
            }
        }

        if (slot.current != nullptr)
        {
            if (keepForFade && slot.audioSeenGen == slot.currentGen)
            {
                auto* entry = &slot.retired[0];
                for (auto& r : slot.retired)
                    if (r.track == nullptr || (entry->track != nullptr && r.gen < entry->gen))
                        entry = &r;

                graveyard.add (std::move (entry->track));
                entry->track = std::move (slot.current);
                entry->gen = slot.currentGen;
                entry->releasable = false;
            }
            else
            {
                graveyard.add (std::move (slot.current));
            }
        }

        updateRetiredPending();
        slot.current = std::move (track);
        slot.currentGen = gen;
        slot.regionStart = regionStart;
        slot.regionEnd = regionEnd;
        ++slot.regionGen;
    }

    void setSlotRegion (long long regionStart, long long regionEnd)
    {
        const juce::SpinLock::ScopedLockType lock (slotLock);
        slot.regionStart = regionStart;
        slot.regionEnd = regionEnd;
        ++slot.regionGen;
    }

    void updateRetiredPending() noexcept   // slotLock held
    {
        bool any = false;
        for (auto& r : slot.retired)
            any = any || r.track != nullptr;
        retiredPending.store (any, std::memory_order_release);
    }

    void releaseRetiredTracks (bool force)
    {
        if (! force && ! retiredPending.load (std::memory_order_acquire))
            return;

        Graveyard graveyard;
        const juce::SpinLock::ScopedLockType lock (slotLock);
        for (auto& r : slot.retired)
        {
            if (r.track != nullptr && (force || r.releasable))
            {
                graveyard.add (std::move (r.track));
                r = {};
            }
        }
        updateRetiredPending();
    }

    //==============================================================================
    // List helpers (stateMutex held)
    EntryData* find (int id) noexcept
    {
        for (auto& e : entries)
            if (e.id == id)
                return &e;
        return nullptr;
    }

    const EntryData* find (int id) const noexcept
    {
        for (auto& e : entries)
            if (e.id == id)
                return &e;
        return nullptr;
    }

    static Loop clampLoop (double length, const Loop& loop)
    {
        if (! loop.set || ! std::isfinite (loop.start) || ! std::isfinite (loop.end) || length <= 0.0)
            return {};

        double s = juce::jlimit (0.0, length, juce::jmin (loop.start, loop.end));
        double e = juce::jlimit (0.0, length, juce::jmax (loop.start, loop.end));
        const double minLength = juce::jmin (kMinLoopSeconds, length);

        if (e - s < minLength)
        {
            e = s + minLength;
            if (e > length)
            {
                e = length;
                s = length - minLength;
            }
        }
        return { true, s, e };
    }

    static std::pair<long long, long long> regionSamples (const RefTrack& track, const Loop& loop)
    {
        const long long n = track.numSamples();
        if (! loop.set || n <= 1)
            return { 0, n };

        const auto s = juce::jlimit (0LL, n - 1, (long long) std::llround (loop.start * track.sampleRate));
        const auto e = juce::jlimit (s + 1, n, (long long) std::llround (loop.end * track.sampleRate));
        return { s, e };
    }

    Loop activeLoopOf (const EntryData& e) const
    {
        return e.activeLoop >= 0 ? e.loops[(size_t) e.activeLoop] : Loop {};
    }

    void bumpInfo() { infoVersion.fetch_add (1); }

    // `force`: queue even while this entry's load is running (it found the host rate changed).
    void queueLoadLocked (EntryData& e, bool analyse, bool force = false)
    {
        // A running load of the same file (same serial) installs the result; one for a relinked file does not.
        if (! force && runningLoadId == e.id && runningLoadSerial == e.serial)
            return;
        if (e.loadQueued)
        {
            for (auto& j : loadQueue)
                if (j.id == e.id)
                    j.analyse = j.analyse || analyse;
            return;
        }
        e.loadQueued = true;
        e.state = analyse || ! e.analysed ? State::Loading : e.state;
        loadQueue.push_back ({ e.id, e.file, targetSampleRate(), e.serial, analyse || ! e.analysed });
        loaderThread.notify();
    }

    // The selected entry changed, or its track / region did: hand the right audio to the audio thread and publish the
    // right reference profile (stateMutex held).
    void refreshSelectionLocked (Graveyard& graveyard)
    {
        auto* e = find (selectedId);

        if (e == nullptr || e->track == nullptr || e->track->sampleRate != targetSampleRate())
        {
            if (slotTrackOwner != 0 || slotHasTrack)
            {
                installTrack (nullptr, 0, 0, graveyard);
                slotHasTrack = false;
                slotTrackOwner = 0;
                graveyard.add (std::move (slotTrack));   // freed after the locks, not kept outside the accounting
                slotTrack = nullptr;
            }
            if (e != nullptr && e->state != State::Error && e->state != State::Missing)
                queueLoadLocked (*e, false);
            publishSelectedProfileLocked();
            return;
        }

        const auto [rs, re] = regionSamples (*e->track, activeLoopOf (*e));
        if (slotTrackOwner != e->id || slotTrack != e->track)
        {
            installTrack (e->track, rs, re, graveyard);
            slotTrackOwner = e->id;
            graveyard.add (std::move (slotTrack));
            slotTrack = e->track;
            slotHasTrack = true;
            slotRegion = { rs, re };
        }
        else if (slotRegion != std::make_pair (rs, re))
        {
            setSlotRegion (rs, re);
            slotRegion = { rs, re };
        }

        publishSelectedProfileLocked();
    }

    // Publishes the selected entry's active-region profile (cached), or queues its analysis.
    void publishSelectedProfileLocked()
    {
        const auto* e = find (selectedId);
        ps::TrackProfile profile;
        juce::uint64 key = 0;

        if (e != nullptr && e->analysed)
        {
            if (e->activeLoop < 0)
            {
                profile = e->whole;
            }
            else if (const auto& cached = e->loopProfiles[(size_t) e->activeLoop]; cached.has_value())
            {
                profile = *cached;
            }
            else
            {
                // Measured on the loader thread; the previous profile stays published until it is done.
                auto* me = find (selectedId);
                const auto loop = activeLoopOf (*e);
                if (e->track != nullptr
                    && ! (me->analysing && me->analysingLetter == e->activeLoop && me->analysingLoop == loop))
                {
                    for (auto& other : entries)   // one region job at a time: this one supersedes the others
                        other.analysing = false;
                    me->analysing = true;
                    me->analysingLetter = e->activeLoop;
                    me->analysingLoop = loop;
                    pendingRegion = RegionJob { e->id, e->activeLoop, loop, e->track, ++regionSerial, e->serial };
                    regionSerialAtomic.store (regionSerial);
                    loaderThread.notify();
                }
                return;
            }
            key = ((juce::uint64) (juce::uint32) e->id << 8) | (juce::uint64) (e->activeLoop + 1);
            key |= (juce::uint64) e->serial << 40;
        }

        const bool hasProfile = profile.valid || profile.analysedSeconds > 0.0;
        if (key == publishedKey && publishedHasProfile == hasProfile)
            return;
        publishedKey = key;
        publishedHasProfile = hasProfile;
        publishReferenceProfile (profile);
    }

    void publishReferenceProfile (const ps::TrackProfile& profile)
    {
        {
            const std::lock_guard<std::mutex> lock (resultsMutex);
            refProfile = profile;
            comparison = ps::compareProfiles (mixProfile, refProfile);
            holdBands = comparison.valid();
            analysisVersion.fetch_add (1);
        }
        refIntegratedLufs.store (profile.integratedLufs);
        refProfileVersion.fetch_add (1);
    }

    long long residentBytesLocked() const
    {
        long long total = 0;
        for (auto& e : entries)
            if (e.track != nullptr)
                total += (long long) e.track->numSamples() * 2 * (long long) sizeof (float);
        return total;
    }

    // Least recently selected first, never the selected one.
    void evictLocked (Graveyard& graveyard)
    {
        while (residentBytesLocked() > memoryBudget.load())
        {
            EntryData* victim = nullptr;
            for (auto& e : entries)
                if (e.track != nullptr && e.id != selectedId && (victim == nullptr || e.lastUsed < victim->lastUsed))
                    victim = &e;
            if (victim == nullptr)
                break;
            graveyard.add (std::move (victim->track));
            victim->track = nullptr;
            bumpInfo();
        }
    }

    //==============================================================================
    // Message-thread API
    int add (const juce::File& file, bool selectIt)
    {
        Graveyard graveyard;
        int id = 0;
        {
            const std::lock_guard<std::mutex> lock (stateMutex);
            for (auto& e : entries)
            {
                if (e.file == file)
                {
                    id = e.id;
                    break;
                }
            }

            if (id == 0)
            {
                if ((int) entries.size() >= kMaxReferences || ! TrackReferencing::isSupportedFile (file))
                    return 0;

                EntryData e;
                e.id = nextId++;
                e.file = file;
                e.name = file.getFileNameWithoutExtension();
                e.lastUsed = ++useCounter;
                entries.push_back (std::move (e));
                id = entries.back().id;
                queueLoadLocked (entries.back(), true);
                feedMaster.store (true);
            }

            if (selectIt)
                selectLocked (id, graveyard);
            bumpInfo();
        }
        changed();
        return id;
    }

    void selectLocked (int id, Graveyard& graveyard)
    {
        if (id != 0 && find (id) == nullptr)
            return;
        if (id != selectedId)
            seekDroppedUpTo.store (seekCount.load());   // a seek meant for the previous reference
        selectedId = id;
        if (auto* e = find (id))
            e->lastUsed = ++useCounter;
        refreshSelectionLocked (graveyard);
        evictLocked (graveyard);
        bumpInfo();
    }

    void remove (int id)
    {
        Graveyard graveyard;
        bool removed = false;
        juce::String removedPath;
        {
            const std::lock_guard<std::mutex> lock (stateMutex);
            for (auto it = entries.begin(); it != entries.end(); ++it)
            {
                if (it->id != id)
                    continue;
                graveyard.add (std::move (it->track));
                removedPath = it->file.getFullPathName();
                loadQueue.erase (std::remove_if (loadQueue.begin(), loadQueue.end(),
                                                 [id] (const LoadJob& j) { return j.id == id; }),
                                 loadQueue.end());
                if (pendingRegion.has_value() && pendingRegion->id == id)
                    pendingRegion.reset();
                entries.erase (it);
                removed = true;
                break;
            }

            if (removed)
            {
                if (selectedId == id)
                {
                    // The next one in the list (or none), like closing a tab.
                    seekDroppedUpTo.store (seekCount.load());
                    selectedId = entries.empty() ? 0 : entries.front().id;
                    if (selectedId != 0)
                        find (selectedId)->lastUsed = ++useCounter;
                }
                refreshSelectionLocked (graveyard);
                feedMaster.store (! entries.empty());
                if (entries.empty())
                    listen.store (false);
                bumpInfo();
            }
        }

        if (removed)
        {
            changed();
            if (onRemoved)
                onRemoved (id, removedPath);
        }
    }

    void relink (int id, const juce::File& file)
    {
        Graveyard graveyard;
        juce::String oldPath;
        {
            const std::lock_guard<std::mutex> lock (stateMutex);
            auto* e = find (id);
            if (e == nullptr || ! TrackReferencing::isSupportedFile (file))
                return;

            oldPath = e->file.getFullPathName();
            e->file = file;
            e->name = file.getFileNameWithoutExtension();
            e->serial = ++serialCounter;   // loads, region jobs and events of the old file become stale
            e->error = {};
            e->analysed = false;
            e->analysisUnreported = false;
            e->loopProfiles = {};
            e->analysing = false;
            if (pendingRegion.has_value() && pendingRegion->id == id)
                pendingRegion.reset();
            graveyard.add (std::move (e->track));
            e->track = nullptr;
            e->loadQueued = false;
            loadQueue.erase (std::remove_if (loadQueue.begin(), loadQueue.end(),
                                             [id] (const LoadJob& j) { return j.id == id; }),
                             loadQueue.end());
            queueLoadLocked (*e, true);
            if (selectedId == id)
                refreshSelectionLocked (graveyard);
            bumpInfo();
        }
        changed();
        if (onRelinked && oldPath != file.getFullPathName())
            onRelinked (id, oldPath, file.getFullPathName());
    }

    void select (int id)
    {
        Graveyard graveyard;
        {
            const std::lock_guard<std::mutex> lock (stateMutex);
            selectLocked (id, graveyard);
        }
        changed();
    }

    void setLoop (int id, int letter, double start, double end)
    {
        if (letter < 0 || letter >= kNumLoops || ! std::isfinite (start) || ! std::isfinite (end))
            return;

        Graveyard graveyard;
        {
            const std::lock_guard<std::mutex> lock (stateMutex);
            auto* e = find (id);
            if (e == nullptr || e->lengthSeconds <= 0.0)
                return;

            const auto loop = clampLoop (e->lengthSeconds, { true, start, end });
            if (! loop.set)
                return;
            if (e->loops[(size_t) letter] != loop)
            {
                e->loops[(size_t) letter] = loop;
                e->loopProfiles[(size_t) letter].reset();
            }
            e->activeLoop = letter;
            if (id == selectedId)
            {
                publishedKey = 0;   // the region changed under the same key
                refreshSelectionLocked (graveyard);
            }
            bumpInfo();
        }
        changed();
    }

    void clearLoop (int id, int letter)
    {
        if (letter < 0 || letter >= kNumLoops)
            return;

        Graveyard graveyard;
        {
            const std::lock_guard<std::mutex> lock (stateMutex);
            auto* e = find (id);
            if (e == nullptr || ! e->loops[(size_t) letter].set)
                return;
            e->loops[(size_t) letter] = {};
            e->loopProfiles[(size_t) letter].reset();
            if (e->activeLoop == letter)
                e->activeLoop = -1;
            if (id == selectedId)
                refreshSelectionLocked (graveyard);
            bumpInfo();
        }
        changed();
    }

    void setActiveLoop (int id, int letter)
    {
        if (letter < -1 || letter >= kNumLoops)
            return;

        Graveyard graveyard;
        {
            const std::lock_guard<std::mutex> lock (stateMutex);
            auto* e = find (id);
            if (e == nullptr || e->activeLoop == letter || (letter >= 0 && ! e->loops[(size_t) letter].set))
                return;
            e->activeLoop = letter;
            if (id == selectedId)
                refreshSelectionLocked (graveyard);
            bumpInfo();
        }
        changed();
    }

    Info getInfo() const
    {
        Info info;
        const std::lock_guard<std::mutex> lock (stateMutex);
        info.version = infoVersion.load();
        const int loadingId = currentLoadId.load();
        for (size_t i = 0; i < entries.size(); ++i)
        {
            const auto& d = entries[i];
            Entry e;
            e.id = d.id;
            e.file = d.file;
            e.name = d.name;
            e.state = d.state;
            e.error = d.error;
            e.progress = d.id == loadingId ? currentLoadProgress.load() : 0.0f;
            e.resident = d.track != nullptr && d.track->sampleRate == targetSampleRate();
            e.analysing = d.analysing;
            e.lengthSeconds = d.lengthSeconds;
            e.fileSampleRate = d.fileSampleRate;
            e.fileChannels = d.fileChannels;
            e.integratedLufs = d.analysed ? d.whole.integratedLufs : ps::kNoValue;
            e.truePeakDb = d.analysed ? d.whole.truePeakDb : ps::kNoValue;
            e.loops = d.loops;
            e.activeLoop = d.activeLoop;
            if (d.id == selectedId)
                info.selected = (int) i;
            info.entries.push_back (std::move (e));
        }
        return info;
    }

    //==============================================================================
    // Loader thread
    void loaderLoop()
    {
        while (! loaderThread.threadShouldExit())
        {
            std::optional<LoadJob> load;
            std::optional<RegionJob> region;

            {
                const std::lock_guard<std::mutex> lock (stateMutex);

                // The selected reference's load first, then a region analysis, then the other loads in order.
                auto it = std::find_if (loadQueue.begin(), loadQueue.end(),
                                        [this] (const LoadJob& j) { return j.id == selectedId; });
                if (it != loadQueue.end())
                {
                    load = *it;
                    loadQueue.erase (it);
                }
                else if (pendingRegion.has_value())
                {
                    std::swap (region, pendingRegion);
                }
                else if (! loadQueue.empty())
                {
                    load = loadQueue.front();
                    loadQueue.pop_front();
                }

                if (load.has_value())
                    if (auto* e = find (load->id))
                        e->loadQueued = false;

                loaderBusy = load.has_value() || region.has_value();
                currentLoadId.store (load.has_value() ? load->id : 0);
                runningLoadId = load.has_value() ? load->id : 0;
                runningLoadSerial = load.has_value() ? load->serial : 0;
                currentLoadProgress.store (0.0f);
            }

            if (! load.has_value() && ! region.has_value())
            {
                loaderThread.wait (200);
                continue;
            }

            try
            {
                if (load.has_value())
                    runLoadJob (*load);
                else
                    runRegionJob (*region);
            }
            catch (const std::exception&)
            {
                const std::lock_guard<std::mutex> lock (stateMutex);
                if (load.has_value())
                {
                    if (auto* e = find (load->id); e != nullptr && e->serial == load->serial)
                    {
                        e->state = State::Error;
                        e->error = "Not enough memory to load this file";
                        e->analysisUnreported = false;
                        events.push_back ({ e->id, false, e->serial });
                    }
                }
                else if (auto* e = find (region->id))
                {
                    e->analysing = false;
                }
                bumpInfo();
            }

            const std::lock_guard<std::mutex> lock (stateMutex);
            currentLoadId.store (0);
            runningLoadId = 0;
            runningLoadSerial = 0;
        }

        const std::lock_guard<std::mutex> lock (stateMutex);
        loaderBusy = false;
    }

    bool cancelledLoad (const LoadJob& job) const
    {
        if (loaderThread.threadShouldExit())
            return true;
        const std::lock_guard<std::mutex> lock (stateMutex);
        const auto* e = find (job.id);
        return e == nullptr || e->serial != job.serial;
    }

    // Decodes, resamples and (for a first load) analyses one file. Errors become the entry's state.
    void runLoadJob (const LoadJob& job)
    {
        juce::String error;
        bool missing = false;
        double fileRate = 0.0;
        int fileChannels = 0;
        std::shared_ptr<RefTrack> track;
        int lastCheck = 0;
        auto cancelled = [&]
        {
            // Polled often; the lock is cheap but not free, so check every 8th call.
            if ((++lastCheck & 7) != 0)
                return loaderThread.threadShouldExit();
            return cancelledLoad (job);
        };

        if (! job.file.existsAsFile())
        {
            error = "File not found";
            missing = true;
        }
        else if (std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (job.file)); reader == nullptr
                 || reader->sampleRate <= 0.0 || reader->numChannels == 0)
        {
            error = "Unsupported or damaged audio file";
        }
        else
        {
            fileRate = reader->sampleRate;
            fileChannels = (int) reader->numChannels;
            const long long fileLength = reader->lengthInSamples;

            if (fileLength <= 0)
                error = "The file contains no audio";
            else if ((double) fileLength / fileRate < kMinReferenceSeconds)
                error = "File is shorter than half a second";
            else if ((double) fileLength / fileRate > kMaxReferenceSeconds + 1.0
                     || fileLength > (long long) std::numeric_limits<int>::max())
                error = "File is longer than 20 minutes";
            else
                track = decode (*reader, fileLength, fileRate, job, error, cancelled);
        }

        if (track == nullptr && error.isEmpty())
            return;   // cancelled

        std::shared_ptr<RefDrawData> draw;
        bool analysedNow = false;
        ps::TrackProfile whole;
        ps::assist::ReferenceTarget target;

        if (track != nullptr)
        {
            const float* l = track->audio.getReadPointer (0);
            const float* r = track->audio.getReadPointer (1);
            const long long length = track->numSamples();

            draw = std::make_shared<RefDrawData>();
            draw->lengthSeconds = track->lengthSeconds();
            draw->overview = ps::makeWaveformOverview (l, r, length, RefDrawData::kOverviewColumns);
            if (! shortTermCurve (l, r, length, track->sampleRate, draw->shortTermCurve, job))
                return;
            currentLoadProgress.store (0.75f);

            if (cancelledLoad (job))
                return;

            if (job.analyse)
            {
                // Not cancellable inside (a few seconds for a 20-minute file); the destructor waits for it.
                const auto a = ps::assist::analyse (l, r, length, track->sampleRate);
                whole = a.profile;
                target = ps::assist::referenceFromAnalysis (a, job.file.getFileNameWithoutExtension().toStdString());
                target.path = job.file.getFullPathName().toStdString();
                analysedNow = true;
                if (! whole.valid)
                {
                    error = "The file is too short or silent";
                    track = nullptr;
                }
            }
            currentLoadProgress.store (1.0f);
        }

        Graveyard graveyard;
        {
            const std::lock_guard<std::mutex> lock (stateMutex);
            runningLoadId = 0;   // from here on a new request for this entry is queued, not merged with this job
            runningLoadSerial = 0;
            auto* e = find (job.id);
            if (e == nullptr || e->serial != job.serial)
                return;
            if (track != nullptr && track->sampleRate != targetSampleRate())
            {
                // The host rate changed meanwhile: decode again at the new rate (keep what was analysed; the
                // "analysed" event goes out when that decode is installed).
                if (analysedNow)
                {
                    e->whole = whole;
                    e->target = target;
                    e->analysed = true;
                    e->analysisUnreported = true;
                }
                if (draw != nullptr)
                    e->draw = draw;
                queueLoadLocked (*e, false, true);
                bumpInfo();
                return;
            }

            if (track == nullptr)
            {
                e->state = missing ? State::Missing : State::Error;
                e->error = error;
                e->analysed = false;
                e->analysisUnreported = false;
                bumpInfo();
                events.push_back ({ e->id, false, e->serial });
                if (e->id == selectedId)
                    refreshSelectionLocked (graveyard);
                return;
            }

            e->state = State::Ready;
            e->error = {};
            e->fileSampleRate = fileRate;
            e->fileChannels = fileChannels;
            e->lengthSeconds = track->lengthSeconds();
            e->draw = draw;
            graveyard.add (std::move (e->track));
            e->track = track;

            // Loops saved for another file length (relink) are clamped to this one.
            for (auto& lp : e->loops)
                lp = clampLoop (e->lengthSeconds, lp);
            if (e->activeLoop >= 0 && ! e->loops[(size_t) e->activeLoop].set)
                e->activeLoop = -1;

            if (analysedNow)
            {
                e->whole = whole;
                e->target = target;
                e->analysed = true;
                e->loopProfiles = {};
            }
            if (analysedNow || e->analysisUnreported)
                events.push_back ({ e->id, true, e->serial });
            e->analysisUnreported = false;

            if (e->id == selectedId)
            {
                publishedKey = 0;
                refreshSelectionLocked (graveyard);
            }
            evictLocked (graveyard);
            bumpInfo();
        }
    }

    // ps::shortTermLoudnessCurve with cancellation: one value per completed 100 ms step (kNoValue for the first 3 s).
    bool shortTermCurve (const float* l, const float* r, long long numSamples, double rate, std::vector<float>& out,
                         const LoadJob& job)
    {
        ps::LoudnessMeter meter;
        meter.prepare (rate);
        const int step = meter.getSamplesPerStep();
        const long long numSteps = numSamples / step;
        out.clear();
        out.reserve ((size_t) numSteps);
        for (long long k = 0; k < numSteps; ++k)
        {
            if (k % 256 == 0 && cancelledLoad (job))
                return false;
            meter.process (l + k * step, r + k * step, step);
            out.push_back (meter.shortTermLufs());
        }
        return true;
    }

    template <typename Cancelled>
    std::shared_ptr<RefTrack> decode (juce::AudioFormatReader& reader, long long fileLength, double fileRate,
                                      const LoadJob& job, juce::String& error, Cancelled&& cancelled)
    {
        const double outRate = job.sampleRate > 0.0 ? job.sampleRate : fileRate;
        const bool resample = std::abs (outRate - fileRate) > 1.0e-6;

        juce::AudioBuffer<float> decoded;
        try
        {
            decoded.setSize (2, (int) fileLength);
        }
        catch (const std::bad_alloc&)
        {
            error = "Not enough memory to load this file";
            return nullptr;
        }

        // NaN / inf samples of a damaged float file become silence (before resampling, which would smear them).
        long long nonFinite = 0;
        for (long long pos = 0; pos < fileLength; pos += kDecodeChunk)
        {
            if (cancelled())
                return nullptr;

            const int n = (int) std::min<long long> (kDecodeChunk, fileLength - pos);
            if (! reader.read (&decoded, (int) pos, n, pos, true, true))
            {
                error = "Unsupported or damaged audio file";
                return nullptr;
            }

            for (int ch = 0; ch < 2; ++ch)
            {
                float* d = decoded.getWritePointer (ch, (int) pos);
                for (int i = 0; i < n; ++i)
                {
                    if (! std::isfinite (d[i]))
                    {
                        d[i] = 0.0f;
                        ++nonFinite;
                    }
                }
            }
            currentLoadProgress.store (0.4f * (float) (pos + n) / (float) fileLength);
        }

        if (nonFinite * 100 > 2 * fileLength)
        {
            error = "Unsupported or damaged audio file";
            return nullptr;
        }

        auto track = std::make_shared<RefTrack>();
        track->sampleRate = outRate;

        if (resample)
        {
            const ps::SincResampler resampler (fileRate, outRate);
            const long long outLength = resampler.outputLength (fileLength);
            if (outLength <= 0 || outLength > (long long) std::numeric_limits<int>::max())
            {
                error = "Unsupported or damaged audio file";
                return nullptr;
            }

            try
            {
                track->audio.setSize (2, (int) outLength);
            }
            catch (const std::bad_alloc&)
            {
                error = "Not enough memory to load this file";
                return nullptr;
            }

            const float* in[2] = { decoded.getReadPointer (0), decoded.getReadPointer (1) };
            float* out[2] = { track->audio.getWritePointer (0), track->audio.getWritePointer (1) };
            for (long long pos = 0; pos < outLength; pos += kResampleChunk)
            {
                if (cancelled())
                    return nullptr;
                resampler.process (in, fileLength, out, 2, pos, std::min<long long> (outLength, pos + kResampleChunk));
                currentLoadProgress.store (0.4f + 0.3f * (float) pos / (float) outLength);
            }
        }
        else
        {
            track->audio = std::move (decoded);
        }

        return track;
    }

    void runRegionJob (const RegionJob& job)
    {
        const auto serial = job.serial;
        const long long n = job.track->numSamples();
        const auto [start, end] = regionSamples (*job.track, job.loop);
        ps::TrackProfile profile;

        if (end > start && n > 0)
        {
            // In chunks so a newer request can cancel it.
            ps::TrackAnalyser analyser;
            analyser.prepare (job.track->sampleRate, ps::Averaging::SinceReset);
            const float* l = job.track->audio.getReadPointer (0);
            const float* r = job.track->audio.getReadPointer (1);
            for (long long pos = start; pos < end; pos += kDecodeChunk)
            {
                if (loaderThread.threadShouldExit() || regionSerialAtomic.load() != serial)
                    break;
                analyser.push (l + pos, r + pos, (int) std::min<long long> (kDecodeChunk, end - pos));
            }
            profile = analyser.profile();
        }

        Graveyard graveyard;
        const std::lock_guard<std::mutex> lock (stateMutex);
        auto* e = find (job.id);
        if (e == nullptr || e->serial != job.entrySerial || e->track != job.track)
            return;   // removed, relinked or decoded again meanwhile
        if (regionSerial != serial)
            return;   // a newer request is pending (it clears `analysing`)

        e->analysing = false;
        if (e->loops[(size_t) job.letter] == job.loop)
            e->loopProfiles[(size_t) job.letter] = profile;
        if (e->id == selectedId)
        {
            publishedKey = 0;
            refreshSelectionLocked (graveyard);
        }
        bumpInfo();
    }

    //==============================================================================
    // Analysis thread
    void analysisLoop()
    {
        while (! analysisThread.threadShouldExit())
        {
            try
            {
                analysisIteration (false);
            }
            catch (const std::exception&)
            {
            }
            analysisThread.wait (kAnalysisIntervalMs);
        }
    }

    bool drainFifo (bool pushToAnalyser)
    {
        bool any = false;
        for (int pass = 0; pass < 2; ++pass)
        {
            const int ready = fifo.getNumReady();
            if (ready <= 0)
                break;

            const auto scope = fifo.read (ready);
            if (pushToAnalyser)
            {
                auto push = [&] (int start, int size)
                {
                    if (size > 0)
                        mixAnalyser.push (fifoData[0] + start, fifoData[1] + start, size);
                };
                push (scope.startIndex1, scope.blockSize1);
                push (scope.startIndex2, scope.blockSize2);
            }
            any = true;
        }
        return any;
    }

    void publishMix (const ps::TrackProfile& profile, bool fresh)
    {
        const std::lock_guard<std::mutex> lock (resultsMutex);
        mixProfile = profile;
        comparison = ps::compareProfiles (mixProfile, refProfile, holdBands && ! fresh ? &comparison : nullptr);
        holdBands = comparison.valid();
        analysisVersion.fetch_add (1);
    }

    void analysisIteration (bool forcePublish)
    {
        releaseRetiredTracks (false);

        const std::lock_guard<std::mutex> lock (analysisMutex);
        float mixIntegrated = ps::kNoValue, mixShortTerm = ps::kNoValue;
        bool mixFull = false;

        if (analyserPrepared)
        {
            const auto averaging = (ps::Averaging) averagingSetting.load();
            if (averaging != mixAnalyser.getAveraging())
            {
                mixAnalyser.setAveraging (averaging);
                analysisDirty = true;
                freshComparison = true;
            }

            const bool fullBefore = mixWindowFull.load (std::memory_order_acquire);
            if (drainFifo (true))
                analysisDirty = true;

            mixFull = fullBefore && mixWindowFull.load (std::memory_order_acquire);
            mixIntegrated = mixAnalyser.loudnessMeter().integratedLufs();
            mixShortTerm = mixAnalyser.loudnessMeter().shortTermLufs();
        }

        updateGainTarget (mixIntegrated, mixShortTerm, mixFull);

        const int refVersion = refProfileVersion.load();
        if (refVersion != seenRefProfileVersion)
        {
            seenRefProfileVersion = refVersion;
            analysisDirty = true;
        }

        const double now = juce::Time::getMillisecondCounterHiRes();
        if (forcePublish || (analysisDirty && now - lastPublishMs >= kPublishIntervalMs))
        {
            publishMix (analyserPrepared ? mixAnalyser.profile() : ps::TrackProfile(), freshComparison);
            analysisDirty = false;
            freshComparison = false;
            lastPublishMs = now;
        }
    }

    void updateGainTarget (float mixIntegrated, float mixShortTerm, bool mixWindowFullNow)
    {
        const auto mode = (ps::LevelMatch) levelMatchSetting.load();
        const float refIntegrated = refIntegratedLufs.load();
        const auto regionGen = audioRegionGen.load();

        if (mode != gainState.mode || regionGen != gainState.regionGen)
        {
            gainState.mode = mode;
            gainState.regionGen = regionGen;
            gainState.haveShortTerm = false;
        }

        float target = 0.0f;
        if (mode == ps::LevelMatch::Integrated)
        {
            target = ps::referenceMatchGainDb (mixIntegrated, refIntegrated);
        }
        else if (mode == ps::LevelMatch::ShortTerm)
        {
            const float refShortTerm = refMatchShortTermLufs.load();
            const bool mixValid = mixWindowFullNow && ps::hasValue (mixShortTerm)
                                  && mixShortTerm > ps::LoudnessMeter::kAbsoluteGateLufs;

            if (mixValid && ps::hasValue (refShortTerm))
            {
                gainState.shortTermDb = ps::referenceMatchGainDb (mixShortTerm, refShortTerm);
                gainState.haveShortTerm = std::isfinite (gainState.shortTermDb);
            }

            target = gainState.haveShortTerm ? gainState.shortTermDb
                                             : ps::referenceMatchGainDb (mixIntegrated, refIntegrated);
        }

        targetGainDb.store (std::isfinite (target) ? target : 0.0f);
    }

    void resetMix()
    {
        const std::lock_guard<std::mutex> lock (analysisMutex);
        if (analyserPrepared)
        {
            drainFifo (false);
            mixAnalyser.reset();
        }
        publishMix (analyserPrepared ? mixAnalyser.profile() : ps::TrackProfile(), true);
        analysisDirty = false;
        freshComparison = false;
        lastPublishMs = juce::Time::getMillisecondCounterHiRes();
    }

    //==============================================================================
    // Host lifecycle (message thread, audio stopped)
    void prepare (double newSampleRate, int maximumBlockSize)
    {
        analysisThread.stopThread (-1);
        audioPrepared.store (false);

        const double rate = newSampleRate > 0.0 ? newSampleRate : kFallbackSampleRate;
        const int maxBlock = juce::jmax (kMinChunk, maximumBlockSize);
        sampleRate = rate;

        refScratch.setSize (2, maxBlock);
        listenLength = samplesForMs (kListenFadeMs, rate);
        jumpLength = samplesForMs (kJumpFadeMs, rate);
        monoLength = samplesForMs (kMonoFadeMs, rate);
        syncTolerance = samplesForMs (kHostSyncToleranceMs, rate);
        shortTermValidSamples = (long long) std::ceil (kShortTermValidSeconds * rate);
        silenceStepLength = samplesForMs (100.0, rate);
        listenTable = makeQuarterSine (listenLength);
        jumpTable = makeQuarterSine (jumpLength);

        refMeter.prepare (rate);
        refShortTermLufs.store (ps::kNoValue);
        refMatchShortTermLufs.store (ps::kNoValue);
        mixWindowFull.store (false);

        {
            const std::lock_guard<std::mutex> lock (analysisMutex);
            const int fifoSize = juce::jmax ((int) std::ceil (kFifoSeconds * rate), 4 * maxBlock);
            fifoBuffer.setSize (2, fifoSize);
            fifoBuffer.clear();
            fifoData[0] = fifoBuffer.getWritePointer (0);
            fifoData[1] = fifoBuffer.getWritePointer (1);
            fifo.setTotalSize (fifoSize);
            fifo.reset();

            if (! analyserPrepared || analyserRate != rate)
            {
                mixAnalyser.prepare (rate, (ps::Averaging) averagingSetting.load());
                analyserPrepared = true;
                analyserRate = rate;
                analysisDirty = true;
            }
            gainState = GainState();
        }

        const float targetDb = targetGainDb.load();
        gain.reset (rate, kGainSmoothMs / 1000.0);
        gain.setCurrentAndTargetValue ((float) ps::dbToGain (targetDb));

        resetAudioState();
        audio.lastTargetDb = targetDb;
        audio.seenSeekCount = seekCount.load();
        matchGainDb.store (targetDb);

        // The reference audio is at the host rate: tracks at another rate are dropped (the selected one decodes again;
        // the master is heard until then).
        {
            Graveyard graveyard;
            const std::lock_guard<std::mutex> lock (stateMutex);
            hostSampleRate.store (rate);
            for (auto& e : entries)
            {
                if (e.track != nullptr && e.track->sampleRate != rate)
                {
                    graveyard.add (std::move (e.track));
                    e.track = nullptr;
                }
                for (auto& j : loadQueue)
                    j.sampleRate = rate;
            }
            refreshSelectionLocked (graveyard);
            bumpInfo();
        }

        {
            const juce::SpinLock::ScopedLockType lock (slotLock);
            audio.haveTrack = slot.current != nullptr && slot.regionEnd > slot.regionStart;
        }
        audio.listenStep = listen.load() && audio.haveTrack ? listenLength : 0;
        listenPosition.store ((float) audio.listenStep / (float) listenLength);

        releaseRetiredTracks (true);
        audioPrepared.store (true);
        analysisThread.startThread (juce::Thread::Priority::low);
    }

    void release()
    {
        analysisThread.stopThread (-1);
        audioPrepared.store (false);
        releaseRetiredTracks (true);
    }

    //==============================================================================
    // Settings and state
    Settings settings;   // message thread

    void applySettings (const Settings& s)
    {
        settings = s;
        levelMatchSetting.store ((int) s.levelMatch);
        averagingSetting.store ((int) s.averaging);
        audioSettings.store ((s.sync ? 1 : 0) | (s.playWhenStopped ? 2 : 0));
    }

    juce::ValueTree buildStateTree() const
    {
        juce::ValueTree t (kTag);
        t.setProperty ("version", kStateVersion, nullptr);
        t.setProperty ("levelMatch", (int) settings.levelMatch, nullptr);
        t.setProperty ("sync", settings.sync, nullptr);
        t.setProperty ("playWhenStopped", settings.playWhenStopped, nullptr);
        t.setProperty ("averaging", (int) settings.averaging, nullptr);

        const std::lock_guard<std::mutex> lock (stateMutex);
        int selectedIndex = -1;
        for (size_t i = 0; i < entries.size(); ++i)
        {
            const auto& e = entries[i];
            juce::ValueTree r (kRefTag);
            r.setProperty ("path", e.file.getFullPathName(), nullptr);
            r.setProperty ("activeLoop", e.activeLoop, nullptr);
            for (int k = 0; k < kNumLoops; ++k)
                if (e.loops[(size_t) k].set)
                    r.setProperty ("loop" + juce::String::charToString ((juce::juce_wchar) ('A' + k)),
                                   loopToString (e.loops[(size_t) k]), nullptr);
            t.appendChild (r, nullptr);
            if (e.id == selectedId)
                selectedIndex = (int) i;
        }
        t.setProperty ("selected", selectedIndex, nullptr);
        return t;
    }

    void changed()
    {
        if (restorePending.load())
            return;   // saves return the queued restore until it is applied (it rebuilds the cache then)
        auto t = buildStateTree();
        const std::lock_guard<std::mutex> lock (cacheMutex);
        cachedState = t;
    }

    void restoreNow (const juce::ValueTree& t)
    {
        Settings s;
        if (t.isValid())
        {
            s.levelMatch = (ps::LevelMatch) juce::jlimit (0, 2, (int) t.getProperty ("levelMatch", 1));
            s.sync = (bool) t.getProperty ("sync", true);
            s.playWhenStopped = (bool) t.getProperty ("playWhenStopped", true);
            s.averaging = (ps::Averaging) juce::jlimit (0, 1, (int) t.getProperty ("averaging", 0));
        }
        applySettings (s);
        listen.store (false);   // never start on the reference

        struct Saved
        {
            juce::File file;
            std::array<Loop, kNumLoops> loops {};
            int activeLoop = -1;
        };
        std::vector<Saved> saved;
        for (int i = 0; t.isValid() && i < t.getNumChildren() && (int) saved.size() < kMaxReferences; ++i)
        {
            const auto c = t.getChild (i);
            if (! c.hasType (kRefTag))
                continue;
            const auto path = c.getProperty ("path").toString().trim();
            if (path.isEmpty())
                continue;
            Saved sv;
            sv.file = juce::File::isAbsolutePath (path) ? juce::File (path)
                                                        : juce::File::getCurrentWorkingDirectory().getChildFile (path);
            for (int k = 0; k < kNumLoops; ++k)
                sv.loops[(size_t) k] = loopFromString (
                    c.getProperty ("loop" + juce::String::charToString ((juce::juce_wchar) ('A' + k))).toString());
            sv.activeLoop = juce::jlimit (-1, kNumLoops - 1, (int) c.getProperty ("activeLoop", -1));
            if (sv.activeLoop >= 0 && ! sv.loops[(size_t) sv.activeLoop].set)
                sv.activeLoop = -1;
            saved.push_back (std::move (sv));
        }
        const int selectedIndex = t.isValid() ? (int) t.getProperty ("selected", -1) : -1;

        std::vector<std::pair<int, juce::String>> removedIds;
        Graveyard graveyard;
        {
            const std::lock_guard<std::mutex> lock (stateMutex);

            // Re-use entries for the same files (a host restoring the state it just saved keeps everything loaded).
            std::vector<EntryData> next;
            for (auto& sv : saved)
            {
                auto it = std::find_if (entries.begin(), entries.end(),
                                        [&] (const EntryData& e) { return e.file == sv.file; });
                EntryData e;
                if (it != entries.end())
                {
                    e = std::move (*it);
                    entries.erase (it);
                    e.loopProfiles = {};
                }
                else
                {
                    e.id = nextId++;
                    e.file = sv.file;
                    e.name = sv.file.getFileNameWithoutExtension();
                    e.serial = ++serialCounter;
                }
                e.loops = sv.loops;
                if (e.lengthSeconds > 0.0)
                    for (auto& lp : e.loops)
                        lp = clampLoop (e.lengthSeconds, lp);
                e.activeLoop = sv.activeLoop >= 0 && e.loops[(size_t) sv.activeLoop].set ? sv.activeLoop : -1;
                next.push_back (std::move (e));
            }

            for (auto& old : entries)
            {
                removedIds.push_back ({ old.id, old.file.getFullPathName() });
                graveyard.add (std::move (old.track));
            }
            entries = std::move (next);
            loadQueue.clear();
            pendingRegion.reset();

            for (auto& e : entries)
            {
                e.loadQueued = false;
                e.analysing = false;
                if (! e.analysed)
                {
                    e.state = State::Loading;
                    queueLoadLocked (e, true);
                }
            }

            seekDroppedUpTo.store (seekCount.load());
            selectedId = selectedIndex >= 0 && selectedIndex < (int) entries.size() ? entries[(size_t) selectedIndex].id : 0;
            if (auto* e = find (selectedId))
                e->lastUsed = ++useCounter;
            publishedKey = ~(juce::uint64) 0;
            refreshSelectionLocked (graveyard);
            feedMaster.store (! entries.empty());
            bumpInfo();
        }

        changed();
        if (onRemoved)
            for (auto& [id, path] : removedIds)
                onRemoved (id, path);
    }

    //==============================================================================
    juce::AudioProcessor& owner;
    juce::AudioFormatManager formats;   // loader thread only after construction

    // ---- list (stateMutex) ------------------------------------------------------------------------------------
    mutable std::mutex stateMutex;
    std::vector<EntryData> entries;
    int selectedId = 0;
    int nextId = 1;
    long long useCounter = 0;
    juce::uint64 serialCounter = 0;
    std::deque<LoadJob> loadQueue;
    std::optional<RegionJob> pendingRegion;
    juce::uint64 regionSerial = 0;
    std::atomic<juce::uint64> regionSerialAtomic { 0 };
    bool loaderBusy = false;
    std::atomic<double> hostSampleRate { 0.0 };
    std::atomic<int> infoVersion { 1 };
    std::atomic<int> currentLoadId { 0 };
    std::atomic<float> currentLoadProgress { 0.0f };
    std::atomic<long long> memoryBudget { kDefaultMemoryBudget };
    std::vector<Event> events;                 // finished analyses for tick()
    int runningLoadId = 0;                     // the load job the loader runs now (cleared when it has installed)
    juce::uint64 runningLoadSerial = 0;
    juce::uint64 publishedKey = ~(juce::uint64) 0;
    bool publishedHasProfile = false;

    // what the slot holds (stateMutex)
    int slotTrackOwner = 0;
    TrackPtr slotTrack;
    bool slotHasTrack = false;
    std::pair<long long, long long> slotRegion { 0, 0 };
    juce::uint32 nextTrackGen = 1;

    juce::SpinLock slotLock;
    Slot slot;
    std::atomic<bool> retiredPending { false };
    std::atomic<bool> audioPrepared { false };

    std::atomic<double> seekSeconds { 0.0 };
    std::atomic<int> seekCount { 0 };
    std::atomic<int> seekDroppedUpTo { 0 };   // seeks up to this count were for a reference no longer selected

    // ---- settings read by other threads ------------------------------------------------------------------------
    std::atomic<bool> listen { false };
    std::atomic<bool> feedMaster { false };
    std::atomic<int> levelMatchSetting { (int) ps::LevelMatch::Integrated };
    std::atomic<int> averagingSetting { (int) ps::Averaging::Recent };
    std::atomic<int> audioSettings { 3 };   // bit 0 sync, bit 1 play when stopped

    // ---- audio thread ------------------------------------------------------------------------------------------
    AudioState audio;
    juce::AudioBuffer<float> refScratch;
    ps::LoudnessMeter refMeter;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> gain { 1.0f };
    std::vector<float> listenTable, jumpTable;
    int listenLength = 1, jumpLength = 1, monoLength = 1;
    long long syncTolerance = 1;
    long long shortTermValidSamples = 1;
    int silenceStepLength = 1;
    double sampleRate = kFallbackSampleRate;

    // ---- live values for the UI --------------------------------------------------------------------------------
    std::atomic<double> playheadSeconds { 0.0 };
    std::atomic<float> matchGainDb { 0.0f }, listenPosition { 0.0f };
    std::atomic<bool> hostPlaying { false }, referenceSounding { false };
    std::atomic<float> refShortTermLufs { ps::kNoValue };
    std::atomic<int> referenceDropouts { 0 };

    // ---- audio thread -> analysis thread -----------------------------------------------------------------------
    std::atomic<bool> mixWindowFull { false };
    std::atomic<float> refMatchShortTermLufs { ps::kNoValue };
    std::atomic<juce::uint32> audioRegionGen { 0 };

    // ---- analysis (analysisMutex) ------------------------------------------------------------------------------
    std::mutex analysisMutex;
    ps::TrackAnalyser mixAnalyser;
    bool analyserPrepared = false;
    double analyserRate = 0.0;
    juce::AbstractFifo fifo { 1 };
    juce::AudioBuffer<float> fifoBuffer;
    float* fifoData[2] = { nullptr, nullptr };
    std::atomic<int> droppedBlocks { 0 };
    bool analysisDirty = true;
    bool freshComparison = false;
    int seenRefProfileVersion = -1;
    double lastPublishMs = 0.0;
    std::atomic<float> targetGainDb { 0.0f };
    std::atomic<float> refIntegratedLufs { ps::kNoValue };
    std::atomic<int> refProfileVersion { 0 };

    struct GainState
    {
        ps::LevelMatch mode = ps::LevelMatch::Integrated;
        juce::uint32 regionGen = 0;
        bool haveShortTerm = false;
        float shortTermDb = 0.0f;
    };
    GainState gainState;

    // ---- published results (resultsMutex) ----------------------------------------------------------------------
    mutable std::mutex resultsMutex;
    ps::TrackProfile mixProfile, refProfile;
    ps::Comparison comparison;
    bool holdBands = false;
    std::atomic<int> analysisVersion { 0 };

    // ---- state cache / queued restore --------------------------------------------------------------------------
    mutable std::mutex cacheMutex;
    juce::ValueTree cachedState;
    std::mutex restoreMutex;
    juce::ValueTree pendingRestore;
    std::atomic<bool> restorePending { false };

    std::function<void (int, const juce::String&)> onRemoved;
    std::function<void (int, const juce::String&, const juce::String&)> onRelinked;

    // Last: started after (and stopped before) everything they use.
    WorkerThread loaderThread { "PureSheen reference loader", [this] { loaderLoop(); } };
    WorkerThread analysisThread { "PureSheen reference analysis", [this] { analysisLoop(); } };
};

//==============================================================================
TrackReferencing::TrackReferencing (juce::AudioProcessor& owner) : impl (std::make_unique<Impl> (owner))
{
    impl->onRemoved = [this] (int id, const juce::String& path)
    {
        if (onReferenceRemoved)
            onReferenceRemoved (id, path);
    };
    impl->onRelinked = [this] (int id, const juce::String& oldPath, const juce::String& newPath)
    {
        if (onReferenceRelinked)
            onReferenceRelinked (id, oldPath, newPath);
    };
}

TrackReferencing::~TrackReferencing() = default;

void TrackReferencing::prepare (double sampleRate, int maximumBlockSize) { impl->prepare (sampleRate, maximumBlockSize); }
void TrackReferencing::release() { impl->release(); }

void TrackReferencing::process (float* left, float* right, int numSamples, bool mono, bool swap) noexcept
{
    impl->process (left, right, numSamples, mono, swap);
}

bool TrackReferencing::tick()
{
    auto& d = *impl;
    bool changedSomething = false;

    if (d.restorePending.load())
    {
        juce::ValueTree t;
        bool take = false;
        {
            // The tree and its flag change together, so an overlapping restore is never half taken.
            const std::lock_guard<std::mutex> lock (d.restoreMutex);
            take = d.restorePending.exchange (false);
            t = d.pendingRestore;
            d.pendingRestore = {};
        }
        if (take)
        {
            d.restoreNow (t);
            changedSomething = true;
        }
    }

    std::vector<Impl::Event> events;
    {
        const std::lock_guard<std::mutex> lock (d.stateMutex);
        for (auto& ev : d.events)
            if (const auto* e = d.find (ev.id); e != nullptr && e->serial == ev.serial)   // not removed / relinked since
                events.push_back (ev);
        d.events.clear();
    }
    for (auto& ev : events)
    {
        changedSomething = true;
        if (onReferenceAnalysed)
            onReferenceAnalysed (ev.id, ev.ok);
    }
    if (! events.empty())
        d.changed();

    return changedSomething;
}

bool TrackReferencing::isSupportedFile (const juce::File& f)
{
    return f.hasFileExtension ("wav;aif;aiff;flac;mp3;ogg");
}

juce::String TrackReferencing::getSupportedWildcard() { return "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg"; }

int TrackReferencing::addReference (const juce::File& f, bool select) { return impl->add (f, select); }
void TrackReferencing::removeReference (int id) { impl->remove (id); }
void TrackReferencing::relink (int id, const juce::File& f) { impl->relink (id, f); }
void TrackReferencing::select (int id) { impl->select (id); }

int TrackReferencing::getSelectedId() const
{
    const std::lock_guard<std::mutex> lock (impl->stateMutex);
    return impl->selectedId;
}

TrackReferencing::Info TrackReferencing::getInfo() const { return impl->getInfo(); }
int TrackReferencing::getInfoVersion() const noexcept { return impl->infoVersion.load(); }

std::shared_ptr<const RefDrawData> TrackReferencing::getDrawData (int id) const
{
    const std::lock_guard<std::mutex> lock (impl->stateMutex);
    const auto* e = impl->find (id);
    return e != nullptr ? e->draw : nullptr;
}

void TrackReferencing::setLoop (int id, int letter, double s, double e) { impl->setLoop (id, letter, s, e); }
void TrackReferencing::clearLoop (int id, int letter) { impl->clearLoop (id, letter); }
void TrackReferencing::setActiveLoop (int id, int letter) { impl->setActiveLoop (id, letter); }

void TrackReferencing::setListening (bool reference) { impl->listen.store (reference); }
bool TrackReferencing::isListening() const noexcept { return impl->listen.load(); }
TrackReferencing::Settings TrackReferencing::getSettings() const { return impl->settings; }

void TrackReferencing::setSettings (const Settings& s)
{
    impl->applySettings (s);
    impl->changed();
}

void TrackReferencing::seek (double seconds)
{
    if (! std::isfinite (seconds))
        return;
    impl->seekSeconds.store (juce::jmax (0.0, seconds));
    impl->seekCount.fetch_add (1, std::memory_order_release);
}

double TrackReferencing::getPlayheadSeconds() const noexcept { return impl->playheadSeconds.load(); }
float TrackReferencing::getMatchGainDb() const noexcept { return impl->matchGainDb.load(); }
float TrackReferencing::getListenPosition() const noexcept { return impl->listenPosition.load(); }
bool TrackReferencing::isHostPlaying() const noexcept { return impl->hostPlaying.load(); }
bool TrackReferencing::isReferenceSounding() const noexcept { return impl->referenceSounding.load(); }
float TrackReferencing::getReferenceShortTermLufs() const noexcept { return impl->refShortTermLufs.load(); }

ps::TrackProfile TrackReferencing::getMixProfile() const
{
    const std::lock_guard<std::mutex> lock (impl->resultsMutex);
    return impl->mixProfile;
}

ps::TrackProfile TrackReferencing::getReferenceProfile() const
{
    const std::lock_guard<std::mutex> lock (impl->resultsMutex);
    return impl->refProfile;
}

ps::Comparison TrackReferencing::getComparison() const
{
    const std::lock_guard<std::mutex> lock (impl->resultsMutex);
    return impl->comparison;
}

int TrackReferencing::getAnalysisVersion() const noexcept { return impl->analysisVersion.load(); }
void TrackReferencing::resetMixAnalysis() { impl->resetMix(); }

ps::assist::ReferenceTarget TrackReferencing::getAssistantTarget (int id) const
{
    const std::lock_guard<std::mutex> lock (impl->stateMutex);
    const auto* e = impl->find (id);
    return e != nullptr && e->analysed ? e->target : ps::assist::ReferenceTarget {};
}

int TrackReferencing::findByPath (const juce::String& path) const
{
    if (path.isEmpty())
        return 0;
    const std::lock_guard<std::mutex> lock (impl->stateMutex);
    for (auto& e : impl->entries)
        if (e.file.getFullPathName() == path)
            return e.id;
    return 0;
}

juce::ValueTree TrackReferencing::createStateTree() const
{
    const std::lock_guard<std::mutex> lock (impl->cacheMutex);
    return impl->cachedState.createCopy();
}

void TrackReferencing::restoreStateTree (const juce::ValueTree& t)
{
    if (isMessageThread())
    {
        {
            const std::lock_guard<std::mutex> lock (impl->restoreMutex);   // this restore supersedes a queued one
            impl->restorePending.store (false);
            impl->pendingRestore = {};
        }
        impl->restoreNow (t);
        return;
    }

    // The tree, its flag and the cached save change together (the newest restore wins, saves return it).
    const std::lock_guard<std::mutex> lock (impl->restoreMutex);
    impl->pendingRestore = t.createCopy();
    impl->restorePending.store (true);
    const std::lock_guard<std::mutex> cacheLock (impl->cacheMutex);
    impl->cachedState = t.isValid() ? t.createCopy() : juce::ValueTree (kTag);
}

void TrackReferencing::runAnalysisNow() { impl->analysisIteration (true); }

bool TrackReferencing::waitUntilIdle (int timeoutMs) const
{
    const auto start = juce::Time::getMillisecondCounterHiRes();
    for (;;)
    {
        {
            const std::lock_guard<std::mutex> lock (impl->stateMutex);
            if (impl->loadQueue.empty() && ! impl->pendingRegion.has_value() && ! impl->loaderBusy)
                return true;
        }
        if (juce::Time::getMillisecondCounterHiRes() - start > timeoutMs)
            return false;
        juce::Thread::sleep (5);
    }
}

int TrackReferencing::getDropoutCount() const noexcept { return impl->referenceDropouts.load(); }

void TrackReferencing::setMemoryBudget (long long bytes)
{
    impl->memoryBudget.store (juce::jmax (0LL, bytes));
    Graveyard graveyard;
    const std::lock_guard<std::mutex> lock (impl->stateMutex);
    impl->evictLocked (graveyard);
}

long long TrackReferencing::getMemoryBudget() const noexcept { return impl->memoryBudget.load(); }

long long TrackReferencing::getResidentBytes() const
{
    const std::lock_guard<std::mutex> lock (impl->stateMutex);
    return impl->residentBytesLocked();
}
} // namespace psplug
