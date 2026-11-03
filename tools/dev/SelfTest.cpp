#include "AllocationTracking.h"
#include "core/Loudness.h"
#include "core/TruePeak.h"
#include "dsp/Maximizer.h"
#include "dsp/TransientSplit.h"
#include "dsp/BassShaper.h"
#include "plugin/Assistant.h"
#include "plugin/FactoryPresets.h"
#include "plugin/MatchEqState.h"
#include "plugin/PluginProcessor.h"
#include "plugin/PluginState.h"
#include "plugin/TrackReferencing.h"
#include "ui/ImagerPanel.h"
#include "ui/ExciterPanel.h"
#include "ui/MultibandDisplay.h"
#include "ui/DynamicEqDisplay.h"
#include "ui/DynamicEqPanel.h"
#include "ui/LowContrastPanel.h"
#include "ui/LowRegionDisplay.h"
#include "ui/SpectralDisplay.h"
#include "ui/PunchViews.h"
#include "ui/PunchPanel.h"
#include "ui/MatchDisplay.h"
#include "ui/MatchEqPanel.h"
#include "ui/BassScope.h"
#include "ui/Controls.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

// Processor integration tests (owner: plugin). Print one line per check; return 0 when all pass.
//
// The tests drive PureSheenProcessor like a host: prepareToPlay, a fake AudioPlayHead, processBlock, getState /
// setState, parameter gestures. Timers do not run in the dev tool, so the tests call flushPendingChanges() where the
// processor's 30 Hz timer would. Every processBlock runs with allocation tracking on (AllocationTracking.h): operator
// new / delete in all forms and, through this executable's imports, the CRT malloc / calloc / realloc / free that
// JUCE's HeapBlock uses. A canary check proves the counter sees both before the processor tests run. Presets go to a
// temporary folder of their own (unique per run).
//
// Checks marked [dsp] in their name measure the real DSP modules / chain (null tests, latency, Maximizer ceiling,
// learn, delta); each "is neutral" check has a counterpart that proves the path does audible work when it should.
// Every other check tests the processor itself.

namespace juce::detail
{
bool dispatchNextMessageOnSystemQueue (bool returnIfNoPendingMessages);
}

namespace
{
//==============================================================================
int numPassed = 0;
int numFailed = 0;

void check (bool ok, const std::string& name, const std::string& detail = {})
{
    if (ok)
    {
        ++numPassed;
        std::cout << "PASS " << name << "\n";
    }
    else
    {
        ++numFailed;
        std::cout << "FAIL " << name << (detail.empty() ? std::string() : ": " + detail) << "\n";
    }
}

template <typename... Args>
std::string str (Args&&... args)
{
    std::ostringstream ss;
    (ss << ... << args);
    return ss.str();
}

constexpr double kRate = 48000.0;
juce::File tempDir;
std::set<std::string> unknownIds;   // ids the tests refer to that do not exist (reported once at the end)

float dbOf (float linear) { return linear > 0.0f ? 20.0f * std::log10 (linear) : -200.0f; }

int idx (const std::string& id)
{
    const int i = ps::paramIndex (id);
    if (i < 0)
        unknownIds.insert (id);
    return i;
}

float defaultOf (const std::string& id)
{
    const int i = idx (id);
    return i >= 0 ? ps::paramSpecs()[(size_t) i].defaultValue : 0.0f;
}

//==============================================================================
struct Lcg
{
    juce::uint32 state;
    float next() noexcept
    {
        state = state * 1664525u + 1013904223u;
        return (float) ((double) (state >> 8) / 8388608.0 - 1.0);   // -1 .. 1
    }
};

struct FakePlayHead final : juce::AudioPlayHead
{
    bool playing = true;
    long long time = 0;
    double sampleRate = kRate;
    double bpm = 128.0;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying (playing);
        info.setBpm (bpm);
        info.setTimeInSamples ((juce::int64) time);
        info.setTimeInSeconds ((double) time / sampleRate);
        return info;
    }
};

struct Output
{
    std::vector<float> l, r;
};

using InputFn = std::function<void (long long index, float& l, float& r)>;

InputFn noiseInput (float amp, juce::uint32 seed = 1)
{
    auto lcg = std::make_shared<Lcg> (Lcg { seed });
    return [lcg, amp] (long long, float& l, float& r)
    {
        l = amp * lcg->next();
        r = amp * lcg->next();
    };
}

// Noise with a short loud burst every 4800 samples (drives limiters / compressors / clippers).
InputFn musicLikeInput (float amp, juce::uint32 seed = 7)
{
    auto lcg = std::make_shared<Lcg> (Lcg { seed });
    return [lcg, amp] (long long n, float& l, float& r)
    {
        const bool burst = n % 4800 < 120;
        const float a = burst ? juce::jmin (1.4f, amp * 4.0f) : amp;
        const float s = (float) std::sin (juce::MathConstants<double>::twoPi * 55.0 * (double) n / kRate);
        l = a * (0.6f * lcg->next() + 0.4f * s);
        r = a * (0.6f * lcg->next() + 0.4f * s);
    };
}

struct Host
{
    explicit Host (double rate = kRate, int block = 512) : sampleRate (rate), blockSize (block)
    {
        head.sampleRate = rate;
        proc.setPlayHead (&head);
    }

    ~Host() { proc.releaseResources(); }

    void prepare() { proc.prepareToPlay (sampleRate, blockSize); }

    // Host automation (no gesture).
    void set (const std::string& id, float plain)
    {
        if (auto* p = proc.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (plain));
        else
            unknownIds.insert (id);
    }

    // A UI / host gesture (begin, set, end).
    void gesture (const std::string& id, float plain)
    {
        if (auto* p = proc.getParameter (id))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (plain));
            p->endChangeGesture();
        }
        else
        {
            unknownIds.insert (id);
        }
    }

    float get (const std::string& id) const { return proc.getParameterValue (id); }

    void processBlock (juce::AudioBuffer<float>& buffer)
    {
        psdev::setAllocationTracking (true);
        proc.processBlock (buffer, midi);
        psdev::setAllocationTracking (false);
        head.time += buffer.getNumSamples();
    }

    Output stream (long long numSamples, const InputFn& input, std::vector<int> sizes = {},
                   const std::function<void (int blockIndex)>& before = {})
    {
        if (sizes.empty())
            sizes = { blockSize };
        int maxSize = 1;
        for (auto s : sizes)
            maxSize = juce::jmax (maxSize, s);

        Output out;
        out.l.reserve ((size_t) numSamples);
        out.r.reserve ((size_t) numSamples);
        juce::AudioBuffer<float> buffer (2, maxSize);
        long long done = 0;
        int blockIndex = 0;

        while (done < numSamples)
        {
            if (before)
                before (blockIndex);
            const int n = (int) std::min<long long> (sizes[(size_t) blockIndex % sizes.size()], numSamples - done);
            buffer.setSize (2, n, false, false, true);
            for (int i = 0; i < n; ++i)
            {
                float l = 0.0f, r = 0.0f;
                if (input)
                    input (done + i, l, r);
                buffer.setSample (0, i, l);
                buffer.setSample (1, i, r);
            }
            processBlock (buffer);
            for (int i = 0; i < n; ++i)
            {
                out.l.push_back (buffer.getSample (0, i));
                out.r.push_back (buffer.getSample (1, i));
            }
            done += n;
            processed += n;
            ++blockIndex;
        }
        return out;
    }

    PureSheenProcessor proc;
    FakePlayHead head;
    juce::MidiBuffer midi;
    double sampleRate;
    int blockSize;
    long long processed = 0;
};

// Plain-value comparison with the tolerance of the host's 0..1 round trip (see psplug::sameValue).
bool approx (const Host& h, const std::string& id, float want)
{
    return psplug::sameValue (idx (id), h.get (id), want);
}

bool allFinite (const Output& o)
{
    for (size_t i = 0; i < o.l.size(); ++i)
        if (! std::isfinite (o.l[i]) || ! std::isfinite (o.r[i]))
            return false;
    return true;
}

// Peak |out[n] - in[n - latency]| over n >= latency + skip (both channels), as dBFS.
float nullDb (const Output& in, const Output& out, int latency, size_t skip = 0)
{
    float m = 0.0f;
    for (size_t n = (size_t) latency + skip; n < out.l.size(); ++n)
    {
        m = juce::jmax (m, std::abs (out.l[n] - in.l[n - (size_t) latency]));
        m = juce::jmax (m, std::abs (out.r[n] - in.r[n - (size_t) latency]));
    }
    return dbOf (m);
}

Output recordInput (long long n, const InputFn& fn)
{
    Output o;
    for (long long i = 0; i < n; ++i)
    {
        float l = 0.0f, r = 0.0f;
        fn (i, l, r);
        o.l.push_back (l);
        o.r.push_back (r);
    }
    return o;
}

InputFn replay (const Output& o)
{
    return [&o] (long long n, float& l, float& r)
    {
        l = n < (long long) o.l.size() ? o.l[(size_t) n] : 0.0f;
        r = n < (long long) o.r.size() ? o.r[(size_t) n] : 0.0f;
    };
}

std::string chainText (const std::vector<ps::ModuleType>& order)
{
    std::string s;
    for (auto t : order)
        s += std::string (s.empty() ? "" : " ") + ps::moduleInfo (t).idPrefix;
    return s.empty() ? "(empty)" : s;
}

std::vector<ps::ModuleType> defaultChain() { return { ps::kDefaultChain.begin(), ps::kDefaultChain.end() }; }

//==============================================================================
// The allocation counter must see JUCE's malloc-based allocations (HeapBlock) as well as operator new.
void testAllocationCounter()
{
    std::cout << "\n-- Allocation counter\n";

    const auto hooked = psdev::installHeapHooks();
    check (hooked.find ("malloc") != std::string::npos && hooked.find ("free") != std::string::npos,
           "realtime: CRT heap functions hooked (" + hooked + ")", "no import of malloc / free found (static CRT?)");

    const auto saved = psdev::getAllocationCounts();
    volatile float sink = 0.0f;

    psdev::setAllocationTracking (true);
    {
        juce::AudioBuffer<float> buffer;
        buffer.setSize (2, 64);
        buffer.setSample (1, 63, 0.5f);
        juce::Array<int> array;
        array.add (1);
        sink = sink + buffer.getSample (1, 63) + (float) array[0];
    }
    psdev::setAllocationTracking (false);
    const auto heap = psdev::getAllocationCounts();

    psdev::setAllocationTracking (true);
    {
        auto vector = std::make_unique<std::vector<int>> (16, 1);
        sink = sink + (float) (*vector)[3];
    }
    psdev::setAllocationTracking (false);
    const auto all = psdev::getAllocationCounts();

    check (heap.allocations - saved.allocations >= 2 && heap.frees - saved.frees >= 2,
           "realtime: allocation counter sees JUCE HeapBlock malloc / free",
           str (heap.allocations - saved.allocations, " allocations, ", heap.frees - saved.frees, " frees"));
    check (all.allocations - heap.allocations >= 2 && all.frees - heap.frees >= 2,
           "realtime: allocation counter sees operator new / delete",
           str (all.allocations - heap.allocations, " allocations, ", all.frees - heap.frees, " frees"));

    psdev::setAllocationCounts (saved);
    juce::ignoreUnused (sink);
}

//==============================================================================
void testParameters()
{
    std::cout << "\n-- Parameters and buses\n";
    Host h;
    const auto& specs = ps::paramSpecs();
    const auto& hostParams = h.proc.getParameters();

    bool sameOrder = (size_t) hostParams.size() == specs.size();
    std::string first;
    for (int i = 0; sameOrder && i < hostParams.size(); ++i)
    {
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (hostParams[i]);
        const int version = i < ps::kNumParams_0_1_1 ? 1 : 2;   // 0.1.1 table: 1, phase 2 appendix: 2
        sameOrder = p != nullptr && p->getParameterID() == juce::String (specs[(size_t) i].id)
                    && p->getVersionHint() == version && p->getName (256) == juce::String (specs[(size_t) i].name);
        if (! sameOrder)
            first = specs[(size_t) i].id;
    }
    check (sameOrder, str ("params: one host parameter per paramSpecs() entry, same order / id / name, version 1 / 2 (",
                           specs.size(), ")"), first);

    bool defaults = true;
    for (const auto& s : specs)
        defaults = defaults && psplug::sameValue (ps::paramIndex (s.id), h.get (s.id), s.defaultValue);
    check (defaults, "params: every parameter starts at its default (plain value)");

    check (h.proc.getParameter ("nope.nothing") == nullptr && h.proc.getParameterValue ("nope.nothing") == 0.0f,
           "params: unknown id -> nullptr / 0");
    check (h.proc.getBypassParameter() == h.proc.getParameter ("io.bypass"), "params: host bypass parameter = io.bypass");

    // Plain-value mapping of a log parameter.
    h.set ("eq1.a.b3.freq", 2500.0f);
    check (std::abs (h.get ("eq1.a.b3.freq") - 2500.0f) < 0.05f, "params: plain value round trip through the host range",
           str (h.get ("eq1.a.b3.freq")));
    h.set ("eq1.a.b3.shape", (float) ps::EqShape::Surgical);
    check (h.get ("eq1.a.b3.shape") == (float) ps::EqShape::Surgical, "params: choice index round trip");

    // Buses
    using CS = juce::AudioChannelSet;
    auto layout = [] (CS in, CS out)
    {
        juce::AudioProcessor::BusesLayout l;
        l.inputBuses.add (in);
        l.outputBuses.add (out);
        return l;
    };
    check (h.proc.checkBusesLayoutSupported (layout (CS::stereo(), CS::stereo())), "buses: stereo -> stereo supported");
    check (! h.proc.checkBusesLayoutSupported (layout (CS::mono(), CS::mono())), "buses: mono -> mono rejected");
    check (! h.proc.checkBusesLayoutSupported (layout (CS::mono(), CS::stereo())), "buses: mono -> stereo rejected");
    check (! h.proc.checkBusesLayoutSupported (layout (CS::stereo(), CS::mono())), "buses: stereo -> mono rejected");
    check (! h.proc.checkBusesLayoutSupported (layout (CS::create5point1(), CS::create5point1())), "buses: 5.1 rejected");
    check (h.proc.getTotalNumInputChannels() == 2 && h.proc.getTotalNumOutputChannels() == 2,
           "buses: default layout is stereo in / out");
    check (! h.proc.acceptsMidi() && ! h.proc.producesMidi() && h.proc.hasEditor()
               && h.proc.getName() == "PureSheen Dynamics",
           "plugin: no MIDI, has an editor, name");
}

//==============================================================================
void testNullAndBypass()
{
    std::cout << "\n-- Null tests and bypass\n";

    const long long n = 48000;
    const auto quiet = recordInput (n, noiseInput (0.03f, 11));   // -30 dBFS peak

    {
        Host h;
        h.prepare();
        h.set ("io.bypass", 1.0f);
        const auto out = h.stream (n, replay (quiet), { 512, 100, 1, 2048, 333 });
        const int lat = h.proc.getCurrentLatencySamples();
        const float d = nullDb (quiet, out, lat);
        check (d < -120.0f, str ("bypass: output = input delayed by the latency (", lat, " samples), incl. a 2048 block"),
               str (d, " dBFS"));
    }

    {
        Host h;
        h.prepare();
        for (int t = 0; t < ps::kNumModuleTypes; ++t)
            h.set (std::string (ps::moduleInfo ((ps::ModuleType) t).idPrefix) + ".on", 0.0f);
        const auto out = h.stream (n, replay (quiet));
        const int lat = h.proc.getCurrentLatencySamples();
        check (nullDb (quiet, out, lat) < -120.0f, "bypass: every module powered off -> latency-aligned null",
               str (nullDb (quiet, out, lat), " dBFS"));
    }

    {
        Host h;
        h.prepare();
        const auto out = h.stream (n, replay (quiet), { 512, 77 });
        h.proc.flushPendingChanges (true);
        const int lat = h.proc.getCurrentLatencySamples();
        const float d = nullDb (quiet, out, lat, 4096);
        check (d < -120.0f, str ("[dsp] null: default chain at -30 dBFS is transparent (latency ", lat, ")"),
               str (d, " dBFS"));
    }

    {
        Host h;
        h.prepare();
        h.proc.addModule (ps::ModuleType::Equalizer2);
        for (const char* id : { "eq1.mode", "eq2.mode" })
            h.set (id, 1.0f);   // Digital (linear phase, latency)
        h.set ("dyn.bands", 4.0f);
        h.set ("dyn.xoverType", (float) ps::CrossoverType::Digital);
        h.set ("img.bands", 3.0f);
        h.set ("exc.bands", 4.0f);
        h.set ("exc.xoverType", (float) ps::CrossoverType::Hybrid);
        auto out = h.stream (8192, replay (quiet));   // let latency-changing settings settle
        h.proc.flushPendingChanges (true);
        out = h.stream (n, replay (quiet));
        const int lat = h.proc.getCurrentLatencySamples();
        const float d = nullDb (quiet, out, lat, 16384);
        check (d < -100.0f, str ("[dsp] null: all 6 modules, digital EQs, 4-band crossovers stay transparent (latency ", lat,
                                 ")"),
               str (d, " dBFS"));
    }

    {
        // The host may render with a sample rate other than 48 kHz.
        for (double rate : { 44100.0, 96000.0 })
        {
            Host h (rate, 256);
            h.prepare();
            h.set ("io.bypass", 1.0f);
            const auto out = h.stream ((long long) rate / 2, replay (quiet));
            const float d = nullDb (quiet, out, h.proc.getCurrentLatencySamples());
            check (d < -120.0f, str ("bypass: null at ", rate, " Hz"), str (d, " dBFS"));
        }
    }
}

//==============================================================================
// Measures the delay of an impulse through the (bypassed or linear) chain and compares with the reported latency.
// The chain's delay lines (bypass delay, module latency) still hold whatever the previous case played; when the
// latency grows, an older impulse would come out again inside the measured window. So the chain is flushed with
// silence (longer than its latency) before the impulse is sent.
int impulseDelay (Host& h, int length = 32768)
{
    h.stream (h.proc.getChain().getLatencySamples() + 4096, [] (long long, float& l, float& r) { l = r = 0.0f; });
    const auto out = h.stream (length, [] (long long i, float& l, float& r) { l = r = i == 1000 ? 0.5f : 0.0f; });
    int best = -1;
    float peak = 0.0f;
    for (size_t i = 0; i < out.l.size(); ++i)
        if (std::abs (out.l[i]) > peak)
        {
            peak = std::abs (out.l[i]);
            best = (int) i;
        }
    return best < 0 ? -1 : best - 1000;
}

void testLatency()
{
    std::cout << "\n-- Latency reporting\n";
    Host h;
    h.prepare();
    h.proc.flushPendingChanges (true);
    check (h.proc.getLatencySamples() == h.proc.getChain().getLatencySamples(),
           str ("latency: prepareToPlay reports the chain latency (", h.proc.getLatencySamples(), ")"));

    struct Case
    {
        const char* what;
        std::vector<std::pair<std::string, float>> params;
    };
    const std::vector<Case> cases {
        { "EQ 1 Digital", { { "eq1.mode", 1.0f } } },
        { "EQ 1 Digital, FIR 32768", { { "eq1.mode", 1.0f }, { "eq1.firSize", 3.0f } } },
        { "Dynamics lookahead 10 ms", { { "dyn.lookahead", 10.0f } } },
        { "Dynamics 4 bands Digital crossover", { { "dyn.bands", 4.0f }, { "dyn.xoverType", 1.0f } } },
        { "Imager 3 bands Digital crossover", { { "img.bands", 3.0f }, { "img.xoverType", 1.0f } } },
        { "Exciter oversampling off", { { "exc.os", 0.0f } } },
        { "Maximizer Low Latency", { { "max.mode", 0.0f } } },
        { "Maximizer Spectral", { { "max.mode", 4.0f } } },
        { "Maximizer Multiband", { { "max.mode", 5.0f } } },
        { "Maximizer Multiband, True Peak off", { { "max.mode", 5.0f }, { "max.truePeak", 0.0f } } },
    };

    bool allReported = true, allMeasured = true;
    std::string detail, measuredDetail;
    std::set<int> distinct;

    for (const auto& c : cases)
    {
        for (const auto& [id, v] : c.params)
            h.set (id, v);

        h.stream (2048, noiseInput (0.01f));
        h.proc.flushPendingChanges (true);
        const int chainLatency = h.proc.getChain().getLatencySamples();
        distinct.insert (chainLatency);
        if (h.proc.getLatencySamples() != chainLatency || h.proc.getCurrentLatencySamples() != chainLatency)
        {
            allReported = false;
            detail += str (c.what, ": host ", h.proc.getLatencySamples(), " chain ", chainLatency, "; ");
        }

        // The bypassed chain must be delayed by exactly the reported latency.
        h.set ("io.bypass", 1.0f);
        const int measured = impulseDelay (h, 2048 + juce::jmax (4096, chainLatency * 2 + 2048));
        h.set ("io.bypass", 0.0f);
        if (measured != chainLatency)
        {
            allMeasured = false;
            measuredDetail += str (c.what, ": impulse at ", measured, " reported ", chainLatency, "; ");
        }

        for (const auto& [id, v] : c.params)
            h.set (id, defaultOf (id));
    }

    check (allReported, "latency: host latency follows every latency-changing setting (after the timer tick)", detail);
    check (allMeasured, "[dsp] latency: bypassed impulse delay == reported latency in every case", measuredDetail);
    check (distinct.size() > 3, str ("[dsp] latency: modes report different latencies (", distinct.size(), " distinct)"));

    // Chain edits change the latency at once.
    h.set ("eq1.mode", 1.0f);
    h.stream (2048, noiseInput (0.01f));
    h.proc.flushPendingChanges (true);
    const int withEq = h.proc.getLatencySamples();
    h.proc.removeModule (ps::ModuleType::Equalizer1);
    h.stream (512, noiseInput (0.01f));
    h.proc.flushPendingChanges (true);
    const int without = h.proc.getLatencySamples();
    check (without == h.proc.getChain().getLatencySamples(),
           str ("latency: removing a module updates the host latency (", withEq, " -> ", without, ")"));
    check (withEq > without, "[dsp] latency: removing the digital EQ lowers the latency", str (withEq, " -> ", without));

    // Moving a parameter that does not change the mode keeps the latency constant.
    h.proc.undo();
    h.stream (2048, noiseInput (0.01f));
    h.proc.flushPendingChanges (true);
    const int before = h.proc.getLatencySamples();
    bool constant = true;
    for (int k = 0; k < 8; ++k)
    {
        h.set ("eq1.a.b2.on", 1.0f);
        h.set ("eq1.a.b2.gain", -6.0f + (float) k);
        h.set ("max.gain", (float) k);
        h.set ("dyn.a.b1.comp.thr", -5.0f * (float) k);
        h.stream (512, noiseInput (0.01f));
        h.proc.flushPendingChanges (true);
        constant = constant && h.proc.getLatencySamples() == before;
    }
    check (constant, str ("latency: constant while only parameters move (", before, ")"));
}

//==============================================================================
// Realtime safety: every module and mode, with the parameters set between blocks like host automation.
using Config = std::vector<std::pair<std::string, float>>;

void runGroup (Host& h, const std::string& name, const std::vector<Config>& configs, const InputFn& input)
{
    const auto before = psdev::getAllocationCounts();
    bool finite = true;
    std::set<std::string> touched;

    for (const auto& c : configs)
    {
        for (const auto& [id, v] : c)
        {
            h.set (id, v);
            touched.insert (id);
        }
        const auto out = h.stream (1700, input, { 512, 100, 1, 1024, 63 });
        finite = finite && allFinite (out);
    }
    for (const auto& id : touched)
        h.set (id, defaultOf (id));
    h.stream (512, input);

    const auto after = psdev::getAllocationCounts();
    check (after.allocations == before.allocations && after.frees == before.frees,
           str ("realtime: ", name, " (", configs.size(), " configurations) allocation free"),
           str (after.allocations - before.allocations, " allocations, ", after.frees - before.frees, " frees"));
    check (finite, str ("realtime: ", name, " output finite"));
}

Config eqConfig (const std::string& p, int mode, int chMode, int rotation)
{
    Config c { { p + ".mode", (float) mode }, { p + ".chMode", (float) chMode }, { p + ".amount", rotation == 2 ? 200.0f : 100.0f },
               { p + ".softSat", rotation == 1 ? 1.0f : 0.0f }, { p + ".firSize", (float) (rotation % 4) },
               { p + ".link", chMode == 0 ? 1.0f : 0.0f } };
    for (int set = 0; set < 2; ++set)
        for (int b = 1; b <= ps::kEqBands; ++b)
        {
            const auto id = p + (set == 0 ? ".a.b" : ".b.b") + std::to_string (b) + ".";
            const int shape = (b - 1 + rotation * 8 + set * 3) % (int) ps::EqShape::Count;
            c.push_back ({ id + "on", 1.0f });
            c.push_back ({ id + "shape", (float) shape });
            c.push_back ({ id + "freq", 30.0f * std::pow (1.9f, (float) b + (float) rotation * 0.3f) });
            c.push_back ({ id + "gain", (b % 2 == 0 ? 6.0f : -9.0f) * (rotation == 1 ? -0.5f : 1.0f) });
            c.push_back ({ id + "q", 0.3f + (float) b * 0.9f });
            c.push_back ({ id + "slope", (float) ((b + rotation) % (int) ps::EqSlope::Count) });
            c.push_back ({ id + "phase", (float) (rotation * 40) });
        }
    return c;
}

void testRealtime()
{
    std::cout << "\n-- Realtime safety (allocation tracking in processBlock)\n";
    Host h;
    h.prepare();
    h.proc.addModule (ps::ModuleType::Equalizer2);   // all six modules in the chain
    const auto input = musicLikeInput (0.25f);

    runGroup (h, "defaults", { {} }, input);

    for (const char* p : { "eq1", "eq2" })
    {
        std::vector<Config> configs;
        for (int mode = 0; mode < 2; ++mode)
            for (int ch = 0; ch < 4; ++ch)
                for (int rot = 0; rot < 3; ++rot)
                    configs.push_back (eqConfig (p, mode, ch, rot));
        configs.push_back ({ { std::string (p) + ".a.on", 0.0f }, { std::string (p) + ".chMode", 1.0f } });
        configs.push_back ({ { std::string (p) + ".on", 0.0f } });
        runGroup (h, str (p, ": every shape, slope, filter mode, channel mode, FIR size"), configs, input);
    }

    {
        std::vector<Config> configs;
        for (int bands = 1; bands <= 4; ++bands)
            for (int xo = 0; xo < 3; ++xo)
                for (int det = 0; det < 3; ++det)
                {
                    Config c { { "dyn.bands", (float) bands }, { "dyn.xoverType", (float) xo }, { "dyn.detect", (float) det },
                               { "dyn.detFilter", (float) ((det + xo) % 3) }, { "dyn.detSlope", (float) bands - 1 },
                               { "dyn.detTilt", det == 2 ? 3.0f : -3.0f }, { "dyn.lookahead", bands % 2 == 0 ? 5.0f : 0.0f },
                               { "dyn.adaptRel", (float) (det % 2) }, { "dyn.autoGain", (float) (xo % 2) },
                               { "dyn.chMode", (float) ((bands + xo) % 2) }, { "dyn.gain", 2.0f },
                               { "dyn.xover1", 150.0f }, { "dyn.xover2", 1500.0f }, { "dyn.xover3", 7000.0f } };
                    for (int set = 0; set < 2; ++set)
                        for (int b = 1; b <= 4; ++b)
                        {
                            const auto id = std::string (set == 0 ? "dyn.a.b" : "dyn.b.b") + std::to_string (b) + ".";
                            c.push_back ({ id + "comp.thr", -30.0f });
                            c.push_back ({ id + "comp.ratio", b == 2 ? -3.0f : 4.0f });
                            c.push_back ({ id + "comp.att", 0.1f + (float) b * 5.0f });
                            c.push_back ({ id + "comp.rel", 20.0f * (float) b });
                            c.push_back ({ id + "comp.knee", (float) (b * 4) });
                            c.push_back ({ id + "lim.on", (float) ((b + det) % 2) });
                            c.push_back ({ id + "lim.thr", -12.0f });
                            c.push_back ({ id + "lim.ratio", b == 3 ? -2.0f : 20.0f });
                            c.push_back ({ id + "mix", b == 4 ? 50.0f : 100.0f });
                            c.push_back ({ id + "gain", b == 1 ? -3.0f : 1.0f });
                            c.push_back ({ id + "on", b == 3 && det == 1 ? 0.0f : 1.0f });
                        }
                    configs.push_back (c);
                }
        runGroup (h, "Dynamics: 1-4 bands, every crossover / detection / filter, M/S, limiter, upward", configs, input);
    }

    {
        std::vector<Config> configs;
        for (int bands = 1; bands <= 4; ++bands)
            for (int xo = 0; xo < 3; ++xo)
                for (int stz = 0; stz < 2; ++stz)
                    configs.push_back ({ { "img.bands", (float) bands }, { "img.xoverType", (float) xo },
                                         { "img.b1.width", -100.0f }, { "img.b2.width", 50.0f }, { "img.b3.width", -40.0f },
                                         { "img.b4.width", 100.0f }, { "img.amount", stz == 0 ? 100.0f : 60.0f },
                                         { "img.stz.on", 1.0f }, { "img.stz.mode", (float) stz }, { "img.stz.amount", 80.0f },
                                         { "img.rec.on", 1.0f }, { "img.rec.gain", stz == 0 ? 6.0f : -6.0f },
                                         { "img.antiphase", (float) ((bands + xo) % 2) } });
        runGroup (h, "Imager: 1-4 bands, every crossover, Stereoize I / II, Recover Sides, antiphase", configs, input);
    }

    {
        std::vector<Config> configs;
        for (int bands = 1; bands <= 4; ++bands)
            for (int xo = 0; xo < 3; ++xo)
                for (int os = 0; os < 2; ++os)
                {
                    Config c { { "exc.bands", (float) bands }, { "exc.xoverType", (float) xo }, { "exc.os", (float) os },
                               { "exc.post.on", (float) ((bands + os) % 2) }, { "exc.post.freq", 6000.0f },
                               { "exc.post.gain", -4.0f }, { "exc.chMode", (float) (xo % 2) } };
                    for (int set = 0; set < 2; ++set)
                        for (int b = 1; b <= 4; ++b)
                        {
                            const auto id = std::string (set == 0 ? "exc.a.b" : "exc.b.b") + std::to_string (b) + ".";
                            c.push_back ({ id + "mode", (float) ((b + bands + xo + set) % (int) ps::ExciterMode::Count) });
                            c.push_back ({ id + "drive", 70.0f });
                            c.push_back ({ id + "mix", 80.0f });
                        }
                    configs.push_back (c);
                }
        // Every mode on its own at full drive.
        for (int m = 0; m < (int) ps::ExciterMode::Count; ++m)
            configs.push_back ({ { "exc.bands", 1.0f }, { "exc.a.b1.mode", (float) m }, { "exc.a.b1.drive", 100.0f } });
        runGroup (h, "Exciter: 1-4 bands, every crossover / mode, oversampling on / off, post filter, M/S", configs, input);
    }

    {
        std::vector<Config> configs;
        for (int m = 0; m < (int) ps::MaximizerMode::Count; ++m)
            for (int tp = 0; tp < 2; ++tp)
                configs.push_back ({ { "max.mode", (float) m }, { "max.truePeak", (float) tp }, { "max.gain", 12.0f },
                                     { "max.ceiling", tp == 0 ? -0.3f : -3.0f }, { "max.character", (float) (m * 2) } });
        for (int s = 0; s < 4; ++s)
            configs.push_back ({ { "max.mode", 3.0f }, { "max.style3", (float) s }, { "max.gain", 10.0f } });
        for (int s = 0; s < 3; ++s)
            configs.push_back ({ { "max.mode", 4.0f }, { "max.style4", (float) s }, { "max.gain", 10.0f } });
        for (int c = 0; c < 3; ++c)
            configs.push_back ({ { "max.clip.on", 1.0f }, { "max.clip.mode", (float) c }, { "max.clip.amount", 70.0f },
                                 { "max.gain", 8.0f } });
        configs.push_back ({ { "max.trans.on", 1.0f }, { "max.trans.amount", 80.0f }, { "max.upward", 8.0f },
                             { "max.si.trans", 50.0f }, { "max.si.sus", 100.0f }, { "max.character", 0.0f } });
        configs.push_back ({ { "max.character", 10.0f }, { "max.gain", 20.0f }, { "max.ceiling", -20.0f } });
        runGroup (h, "Maximizer: every mode / style, True Peak, soft clip, transient emphasis, upward, independence",
                  configs, input);
    }

    {
        std::vector<Config> configs;
        for (int bits = 0; bits < 5; ++bits)
            for (int amount = 0; amount < 4; ++amount)
                configs.push_back ({ { "dith.on", 1.0f }, { "dith.bits", (float) bits }, { "dith.amount", (float) amount },
                                     { "dith.harm", amount == 3 ? 1.0f : 0.0f }, { "dith.shape", (float) (bits * 25) },
                                     { "dith.limit", (float) (amount % 2) }, { "dith.blank", (float) (bits % 2) },
                                     { "dith.dc", (float) ((bits + amount) % 2) } });
        runGroup (h, "Dither + DC filter: every bit depth / amount / shaping", configs, input);
    }

    {
        std::vector<Config> configs;
        for (int k = 0; k < 32; ++k)
            configs.push_back ({ { "io.gainMatch", (float) (k & 1) }, { "io.gainMatchModern", (float) ((k >> 1) & 1) },
                                 { "io.mono", (float) ((k >> 2) & 1) }, { "io.swap", (float) ((k >> 3) & 1) },
                                 { "io.bypass", (float) ((k >> 4) & 1) }, { "in.gainL", (float) (k % 7) - 3.0f },
                                 { "in.gainR", -6.0f }, { "out.gainL", 3.0f }, { "out.gainR", (float) -(k % 5) } });
        runGroup (h, "I/O: gain match classic / modern, mono, swap, bypass, gains", configs, input);
    }

    // Chain edits and monitoring between blocks (message-thread calls, audio keeps running).
    {
        const auto before = psdev::getAllocationCounts();
        bool finite = true;
        const std::vector<std::vector<ps::ModuleType>> orders {
            { ps::ModuleType::Maximizer, ps::ModuleType::Exciter, ps::ModuleType::Imager, ps::ModuleType::Dynamics,
              ps::ModuleType::Equalizer2, ps::ModuleType::Equalizer1 },
            {},
            { ps::ModuleType::Dynamics },
            { ps::ModuleType::Imager, ps::ModuleType::Maximizer },
            { ps::ModuleType::Equalizer1, ps::ModuleType::Equalizer2, ps::ModuleType::Dynamics, ps::ModuleType::Imager,
              ps::ModuleType::Exciter, ps::ModuleType::Maximizer },
        };
        h.set ("dyn.bands", 3.0f);
        h.set ("img.bands", 4.0f);
        h.set ("exc.bands", 2.0f);
        h.set ("eq1.chMode", 1.0f);

        int step = 0;
        const auto out = h.stream (48000 * 4, input, { 512, 211, 1024 }, [&] (int block)
        {
            if (block % 3 != 0)
                return;
            const int s = step++;
            const auto t = (ps::ModuleType) (s % ps::kNumModuleTypes);
            switch ((s / ps::kNumModuleTypes) % 8)
            {
                case 0: h.proc.setChainOrder (orders[(size_t) (s % (int) orders.size())]); break;
                case 1: h.proc.setSoloModule (s % 2 == 0 ? (int) t : -1); break;
                case 2: h.proc.setDelta (t, s % 2 == 0); break;
                case 3: h.proc.setChannelSolo (t, s % 3 - 1); break;
                case 4:
                {
                    ps::ModuleMonitor m;
                    m.soloBand = s % 5 - 1;
                    m.auditionOn = s % 2 == 1;
                    m.auditionHz = 200.0f * (float) (1 + s % 9);
                    m.soloDetectionFilter = s % 3 == 0;
                    h.proc.setMonitor (t, m);
                    break;
                }
                case 5: h.proc.setTapModule (s % 7 - 1); break;
                case 6: h.proc.getChain().resetIntegratedLoudness(); h.proc.startMaximizerLearn(); break;
                default: h.proc.setChainOrder (orders.back()); h.proc.setSoloModule (-1); break;
            }
        });
        finite = allFinite (out);
        for (int t = 0; t < ps::kNumModuleTypes; ++t)
        {
            h.proc.setDelta ((ps::ModuleType) t, false);
            h.proc.setChannelSolo ((ps::ModuleType) t, -1);
            h.proc.setMonitor ((ps::ModuleType) t, {});
        }
        h.proc.setSoloModule (-1);

        const auto after = psdev::getAllocationCounts();
        check (after.allocations == before.allocations && after.frees == before.frees,
               str ("realtime: chain order / solo / delta / channel solo / band solo / audition / tap / learn changes (",
                    step, " edits) allocation free"),
               str (after.allocations - before.allocations, " allocations"));
        check (finite, "realtime: output finite through chain edits");
    }

    // Other sample rates, everything on.
    for (double rate : { 44100.0, 96000.0, 192000.0 })
    {
        Host hr (rate, 256);
        hr.prepare();
        hr.proc.addModule (ps::ModuleType::Equalizer2);
        runGroup (hr, str ("everything on at ", rate, " Hz"),
                  { eqConfig ("eq1", 1, 1, 0),
                    { { "dyn.bands", 4.0f }, { "dyn.a.b1.comp.thr", -30.0f }, { "img.bands", 4.0f }, { "img.b4.width", 60.0f },
                      { "exc.bands", 4.0f }, { "exc.a.b4.drive", 50.0f }, { "max.mode", 4.0f }, { "max.gain", 10.0f },
                      { "dith.on", 1.0f } },
                    { { "max.mode", 5.0f }, { "max.gain", 10.0f }, { "eq2.mode", 1.0f }, { "io.gainMatch", 1.0f } } },
                  input);
    }
}

//==============================================================================
// Phase 2: the new modules, the Transient/Sustain channel mode, realtime safety of both.
std::vector<float> currentValues (const PureSheenProcessor& p);
int countDifferences (const std::vector<float>& a, const std::vector<float>& b, std::string* first = nullptr);
void setStateXml (PureSheenProcessor& p, const juce::String& xmlText);

std::vector<ps::ModuleType> phase2Types()
{
    std::vector<ps::ModuleType> t;
    for (int i = ps::kNumCoreModuleTypes; i < ps::kNumModuleTypes; ++i)
        t.push_back ((ps::ModuleType) i);
    return t;
}

Config channelModeConfig (ps::ModuleType type, ps::ChannelMode mode)
{
    Config c;
    for (const auto& [id, v] : ps::channelModeParamValues (type, mode))
        c.push_back ({ id, v });
    return c;
}

// Non-default values for every parameter of a module (each one moved inside its range), plus a channel mode.
Config busyConfig (ps::ModuleType type, int rotation)
{
    Config c;
    const auto& specs = ps::paramSpecs();
    const std::string prefix = ps::moduleInfo (type).idPrefix;
    for (size_t i = 0; i < specs.size(); ++i)
    {
        const auto& sp = specs[i];
        if (sp.module != (int) type || sp.id == prefix + ".on" || sp.id == prefix + ".chMode" || sp.id == prefix + ".ts.on")
            continue;
        const float u = (float) ((i * 7 + (size_t) rotation * 3) % 11) / 10.0f;   // 0 .. 1
        c.push_back ({ sp.id, psplug::sanitiseValue ((int) i, sp.minValue + u * (sp.maxValue - sp.minValue)) });
    }
    const auto modes = ps::supportedChannelModes (type);
    for (const auto& m : channelModeConfig (type, modes[(size_t) rotation % modes.size()]))
        c.push_back (m);
    return c;
}

void testPhase2()
{
    std::cout << "\n-- Phase 2 modules and the Transient/Sustain channel mode\n";

    // every phase 2 module can be added, is neutral at its defaults, and survives busy settings allocation-free
    {
        Host h;
        h.prepare();
        for (auto t : phase2Types())
            h.proc.addModule (t);
        h.proc.flushPendingChanges (true);
        const auto order = h.proc.getChainOrder();
        check ((int) order.size() == (int) ps::kDefaultChain.size() + (int) phase2Types().size(),
               str ("phase 2: all eight modules added to the default chain (", order.size(), " modules)"),
               chainText (order));
        check (order.back() == ps::ModuleType::Maximizer, "phase 2: added modules go before the Maximizer",
               chainText (order));

        const long long n = 48000;
        const auto quiet = recordInput (n, noiseInput (0.03f, 21));
        h.stream (8192, replay (quiet));
        const auto out = h.stream (n, replay (quiet));
        const int lat = h.proc.getCurrentLatencySamples();
        const float d = nullDb (quiet, out, lat, 4096);
        check (d < -120.0f, str ("[dsp] null: 14-module chain at defaults is transparent (latency ", lat, ")"),
               str (d, " dBFS"));

        const auto input = musicLikeInput (0.25f);
        for (auto t : phase2Types())
        {
            std::vector<Config> configs;
            for (int rot = 0; rot < 4; ++rot)
                configs.push_back (busyConfig (t, rot));
            configs.push_back ({ { std::string (ps::moduleInfo (t).idPrefix) + ".on", 0.0f } });
            runGroup (h, str (ps::moduleInfo (t).name, ": every parameter moved, every channel mode"), configs, input);
        }
    }

    // Transient/Sustain on every module that offers it: realtime safety at 44.1 and 96 kHz with T/S automation,
    // channel power / solo, band solo, delta, mode switches and power cycling
    for (double rate : { 44100.0, 96000.0 })
    {
        Host h (rate, 256);
        h.prepare();
        std::vector<ps::ModuleType> tsTypes;
        for (int t = 0; t < ps::kNumModuleTypes; ++t)
            if (ps::supportsTransientSustain ((ps::ModuleType) t))
            {
                tsTypes.push_back ((ps::ModuleType) t);
                if (! h.proc.isModuleInChain ((ps::ModuleType) t))
                    h.proc.addModule ((ps::ModuleType) t);
            }
        h.proc.flushPendingChanges (true);
        const auto input = musicLikeInput (0.25f);

        const auto before = psdev::getAllocationCounts();
        bool finite = true;
        int step = 0;
        for (auto t : tsTypes)
            for (const auto& [id, v] : channelModeConfig (t, ps::ChannelMode::TransientSustain))
                h.set (id, v);
        const auto out = h.stream ((long long) (rate * 2.0), input, { 256, 100, 1, 512 }, [&] (int block)
        {
            if (block % 4 != 0)
                return;
            const int k = step++;
            const auto t = tsTypes[(size_t) k % tsTypes.size()];
            const std::string p = ps::moduleInfo (t).idPrefix;
            switch ((k / (int) tsTypes.size()) % 7)
            {
                case 0: h.set (p + ".ts.sens", (float) ((k * 37) % 101)); h.set (p + ".ts.len", 5.0f + (float) (k % 96)); break;
                case 1: h.set (p + ".a.on", (float) (k % 2)); h.set (p + ".b.on", (float) ((k + 1) % 2)); break;
                case 2: h.proc.setChannelSolo (t, k % 3 - 1); break;
                case 3: { ps::ModuleMonitor m; m.soloBand = k % 3 - 1; m.auditionOn = k % 2 == 0; m.auditionChannelSet = k % 2;
                          h.proc.setMonitor (t, m); break; }
                case 4: h.proc.setDelta (t, k % 2 == 0); break;
                case 5:
                {
                    const auto modes = ps::supportedChannelModes (t);
                    for (const auto& [id, v] : channelModeConfig (t, modes[(size_t) k % modes.size()]))
                        h.set (id, v);
                    break;
                }
                default: h.set (p + ".on", (float) (k % 2)); break;
            }
        });
        finite = allFinite (out);
        for (auto t : tsTypes)
        {
            h.proc.setChannelSolo (t, -1);
            h.proc.setDelta (t, false);
            h.proc.setMonitor (t, {});
        }
        const auto after = psdev::getAllocationCounts();
        check (after.allocations == before.allocations && after.frees == before.frees,
               str ("realtime: T/S on ", tsTypes.size(), " modules at ", rate, " Hz (", step, " edits) allocation free"),
               str (after.allocations - before.allocations, " allocations"));
        check (finite, str ("realtime: T/S output finite at ", rate, " Hz"));
    }

    // T/S of EQ 1 alone: transparent at neutral settings, and the latency reported to the host follows the switch
    {
        Host h;
        h.prepare();
        h.proc.setChainOrder ({ ps::ModuleType::Equalizer1 });
        h.set ("eq1.chMode", 3.0f);
        const long long n = 48000;
        const auto quiet = recordInput (n, noiseInput (0.1f, 31));
        h.stream (4096, replay (quiet));
        h.proc.flushPendingChanges (true);
        const auto out = h.stream (n, replay (quiet));
        const int lat = h.proc.getCurrentLatencySamples();
        check (lat == ps::TransientSplitter::latencyFor (kRate), str ("T/S: EQ 1 alone reports the splitter latency (", lat, ")"));
        check (h.proc.getLatencySamples() == lat, "T/S: the host latency follows the switch into T/S",
               str (h.proc.getLatencySamples()));
        const float d = nullDb (quiet, out, lat, 4096);
        check (d < -120.0f, "[dsp] null: EQ 1 in Transient/Sustain at neutral settings", str (d, " dBFS"));
        h.set ("eq1.chMode", 0.0f);
        h.stream (8192, replay (quiet));
        h.proc.flushPendingChanges (true);
        check (h.proc.getCurrentLatencySamples() == 0 && h.proc.getLatencySamples() == 0,
               "T/S: back to Stereo, latency 0 again");
    }

    // Every phase 2 module alone, in every channel mode it offers, at neutral settings: the output is the input delayed
    // by the reported latency (Stereo, M/S, L/R, T/S), and the reported latency includes the T/S splitter.
    {
        std::string failures, latencies;
        bool allNull = true;
        const long long n = 48000;
        const auto quiet = recordInput (n, noiseInput (0.05f, 41));
        for (auto t : phase2Types())
        {
            for (auto mode : ps::supportedChannelModes (t))
            {
                Host h;
                h.prepare();
                h.proc.setChainOrder ({ t });
                for (const auto& [id, v] : channelModeConfig (t, mode))
                    h.set (id, v);
                h.stream (8192, replay (quiet));
                h.proc.flushPendingChanges (true);
                const auto out = h.stream (n, replay (quiet));
                const int lat = h.proc.getCurrentLatencySamples();
                const float d = nullDb (quiet, out, lat, 8192);
                if (! (d < -120.0f) || h.proc.getLatencySamples() != lat)
                {
                    allNull = false;
                    failures += str (ps::moduleInfo (t).idPrefix, " mode ", (int) mode, ": ", d, " dBFS (lat ", lat, ") ");
                }
                static const char* const modeNames[] { "St", "MS", "LR", "TS" };
                latencies += str (ps::moduleInfo (t).idPrefix, ".", modeNames[(int) mode & 3], "=", lat, " ");
            }
        }
        check (allNull, "[dsp] null: every phase 2 module alone in every channel mode at neutral settings (latency "
                        "reported to the host)", failures.empty() ? latencies : failures);
    }

    // State round trip with all 14 modules, every T/S-capable one in Transient/Sustain and random values everywhere:
    // the restored processor holds the same values, chain and channel modes and plays finite audio.
    {
        Host a;
        a.prepare();
        Lcg lcg { 977 };
        const auto& specs = ps::paramSpecs();
        for (size_t i = 0; i < specs.size(); ++i)
        {
            const auto& s = specs[i];
            const float u = 0.5f * (lcg.next() + 1.0f);
            a.set (s.id, psplug::sanitiseValue ((int) i, s.minValue + u * (s.maxValue - s.minValue)));
        }
        std::vector<ps::ModuleType> order;
        for (int t = ps::kNumModuleTypes - 1; t >= 0; --t)
            order.push_back ((ps::ModuleType) t);
        a.proc.setChainOrder (order);
        for (int t = 0; t < ps::kNumModuleTypes; ++t)
            if (ps::supportsTransientSustain ((ps::ModuleType) t))
                for (const auto& [id, v] : channelModeConfig ((ps::ModuleType) t, ps::ChannelMode::TransientSustain))
                    a.set (id, v);
        a.proc.flushPendingChanges (true);
        const auto values = currentValues (a.proc);
        juce::MemoryBlock blob;
        a.proc.getStateInformation (blob);

        Host b;
        b.prepare();
        b.proc.setStateInformation (blob.getData(), (int) blob.getSize());
        b.proc.flushPendingChanges (true);
        std::string first;
        const int diffs = countDifferences (values, currentValues (b.proc), &first);
        bool tsKept = true;
        const auto restored = currentValues (b.proc);
        for (int t = 0; t < ps::kNumModuleTypes; ++t)
            if (ps::supportsTransientSustain ((ps::ModuleType) t))
            {
                ps::SlotParams slot;
                ps::readSlotParams (restored.data(), (ps::ModuleType) t, slot);
                tsKept = tsKept && slot.channelMode == ps::ChannelMode::TransientSustain;
            }
        check (diffs == 0 && b.proc.getChainOrder() == order && tsKept,
               "state: 14 modules in T/S with random values round trip (values, chain, channel modes)",
               str (diffs, " differ, e.g. ", first, "; chain ", chainText (b.proc.getChainOrder())));
        const auto out = b.stream (24000, musicLikeInput (0.2f));
        b.proc.flushPendingChanges (true);
        a.stream (24000, musicLikeInput (0.2f));
        a.proc.flushPendingChanges (true);
        check (allFinite (out), str ("state: the restored 14-module T/S chain plays finite audio"));
        check (b.proc.getLatencySamples() == b.proc.getCurrentLatencySamples()
                   && b.proc.getLatencySamples() == a.proc.getCurrentLatencySamples(),
               "state: the restored 14-module T/S chain reports the same latency to the host",
               str (b.proc.getLatencySamples(), " / ", b.proc.getCurrentLatencySamples(), " / ", a.proc.getCurrentLatencySamples()));
    }

    // A 0.1.x state (six module types, only the 479 parameters of 0.1.1) loads unchanged: listed values applied, every
    // phase 2 parameter at its default, the chain as stored.
    {
        juce::String params;
        const auto& specs = ps::paramSpecs();
        for (int i = 0; i < ps::kNumParams_0_1_1; ++i)
        {
            const auto& sp = specs[(size_t) i];
            float v = sp.defaultValue;
            if (sp.id == "eq1.a.b2.gain") v = -4.5f;
            if (sp.id == "exc.chMode") v = 1.0f;
            if (sp.id == "img.b2.width") v = 35.0f;
            if (sp.id == "max.gain") v = 3.0f;
            params << "<PARAM id=\"" << sp.id << "\" value=\"" << juce::String (v) << "\"/>";
        }
        Host c;
        setStateXml (c.proc, "<PureSheen version=\"1\"><PARAMS>" + params + "</PARAMS><Chain><Module type=\"eq1\"/>"
                             "<Module type=\"dyn\"/><Module type=\"img\"/><Module type=\"exc\"/><Module type=\"eq2\"/>"
                             "<Module type=\"max\"/></Chain><Options/><Snapshots activeSnapshot=\"-1\"/></PureSheen>");
        bool phase2Default = true;
        std::string firstOff;
        for (size_t i = (size_t) ps::kNumParams_0_1_1; i < specs.size(); ++i)
            if (! psplug::sameValue ((int) i, c.get (specs[i].id), specs[i].defaultValue))
            {
                phase2Default = false;
                if (firstOff.empty())
                    firstOff = specs[i].id;
            }
        const std::vector<ps::ModuleType> order { ps::ModuleType::Equalizer1, ps::ModuleType::Dynamics, ps::ModuleType::Imager,
                                                  ps::ModuleType::Exciter, ps::ModuleType::Equalizer2, ps::ModuleType::Maximizer };
        check (approx (c, "eq1.a.b2.gain", -4.5f) && approx (c, "exc.chMode", 1.0f) && approx (c, "img.b2.width", 35.0f)
                   && approx (c, "max.gain", 3.0f) && c.proc.getChainOrder() == order,
               "state: a 0.1.x state (6 modules, 479 parameters) loads its values and chain unchanged",
               chainText (c.proc.getChainOrder()));
        check (phase2Default, "state: a 0.1.x state leaves every phase 2 parameter at its default", firstOff);
        ps::SlotParams slot;
        const auto v = currentValues (c.proc);
        ps::readSlotParams (v.data(), ps::ModuleType::Exciter, slot);
        check (slot.channelMode == ps::ChannelMode::MidSide, "state: 0.1.x Exciter Mid/Side stays Mid/Side");
        ps::readSlotParams (v.data(), ps::ModuleType::Imager, slot);
        check (slot.channelMode == ps::ChannelMode::Stereo, "state: 0.1.x Imager stays Stereo");
    }
}

//==============================================================================
std::vector<float> currentValues (const PureSheenProcessor& p)
{
    std::vector<float> v;
    for (const auto& s : ps::paramSpecs())
        v.push_back (p.getParameterValue (s.id));
    return v;
}

int countDifferences (const std::vector<float>& a, const std::vector<float>& b, std::string* first)
{
    int n = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        if (! psplug::sameValue ((int) i, a[i], b[i]))
        {
            if (n == 0 && first != nullptr)
                *first = str (ps::paramSpecs()[i].id, " ", a[i], " vs ", b[i]);
            ++n;
        }
    return n;
}

juce::MemoryBlock stateOf (PureSheenProcessor& p)
{
    juce::MemoryBlock m;
    p.getStateInformation (m);
    return m;
}

void setStateXml (PureSheenProcessor& p, const juce::String& xmlText)
{
    if (auto xml = juce::parseXML (xmlText))
    {
        juce::MemoryBlock m;
        juce::AudioProcessor::copyXmlToBinary (*xml, m);
        p.setStateInformation (m.getData(), (int) m.getSize());
    }
}

void testState()
{
    std::cout << "\n-- State\n";
    Host a;
    a.prepare();

    // Random legal values everywhere.
    Lcg lcg { 1234 };
    const auto& specs = ps::paramSpecs();
    for (size_t i = 0; i < specs.size(); ++i)
    {
        const auto& s = specs[i];
        const float u = 0.5f * (lcg.next() + 1.0f);
        a.set (s.id, psplug::sanitiseValue ((int) i, s.minValue + u * (s.maxValue - s.minValue)));
    }
    const std::vector<ps::ModuleType> order { ps::ModuleType::Maximizer, ps::ModuleType::Equalizer2,
                                              ps::ModuleType::Imager, ps::ModuleType::DynamicEq, ps::ModuleType::Dynamics,
                                              ps::ModuleType::BassShaper, ps::ModuleType::Smoother };
    a.proc.setChainOrder (order);
    a.proc.storeSnapshot (0);
    const auto snapA = currentValues (a.proc);
    a.set ("eq2.a.b5.gain", -7.5f);
    a.proc.setChainOrder ({ ps::ModuleType::Exciter });
    a.proc.storeSnapshot (2);
    const auto snapC = currentValues (a.proc);
    a.proc.setChainOrder (order);
    a.set ("max.gain", 3.5f);

    juce::ValueTree ui ("Options");
    ui.setProperty ("meterType", 3, nullptr);
    ui.setProperty ("selectedModule", 2, nullptr);
    ui.appendChild (juce::ValueTree ("Recent"), nullptr);
    a.proc.setUiState (ui);
    a.proc.setEditorSize (1300, 800);
    a.proc.flushPendingChanges (true);

    const auto values = currentValues (a.proc);
    const auto blob = stateOf (a.proc);

    Host b;
    b.prepare();
    b.proc.setStateInformation (blob.getData(), (int) blob.getSize());
    std::string first;
    const int diffs = countDifferences (values, currentValues (b.proc), &first);
    check (diffs == 0, str ("state: every parameter restored (", specs.size(), ")"), str (diffs, " differ, e.g. ", first));
    check (b.proc.getChainOrder() == order, "state: chain order restored", chainText (b.proc.getChainOrder()));
    check (b.proc.hasSnapshot (0) && ! b.proc.hasSnapshot (1) && b.proc.hasSnapshot (2) && ! b.proc.hasSnapshot (3),
           "state: snapshots A and C restored, B and D empty");
    check (b.proc.getUiState().getProperty ("meterType") == juce::var (3)
               && b.proc.getUiState().getProperty ("selectedModule") == juce::var (2)
               && b.proc.getUiState().getChildWithName ("Recent").isValid(),
           "state: UI state (Options) restored", b.proc.getUiState().toXmlString().toStdString());
    check (b.proc.getEditorSize() == juce::Point<int> (1300, 800), "state: editor size restored");
    check (b.proc.getHistory().size() == 1 && b.proc.getHistory()[0].description == "Initial state",
           "state: loading starts a fresh history");
    check (b.proc.getCurrentPresetName() == "Default" && b.proc.isPresetModified(),
           "state: preset name and modified flag restored");

    // save -> load -> save: identical apart from float round trips of the parameter values
    {
        const auto blob2 = stateOf (b.proc);
        auto t1 = juce::ValueTree::fromXml (*juce::AudioProcessor::getXmlFromBinary (blob.getData(), (int) blob.getSize()));
        auto t2 = juce::ValueTree::fromXml (*juce::AudioProcessor::getXmlFromBinary (blob2.getData(), (int) blob2.getSize()));
        t1.removeChild (t1.getChildWithName ("PARAMS"), nullptr);
        t2.removeChild (t2.getChildWithName ("PARAMS"), nullptr);
        check (t1.isValid() && t1.isEquivalentTo (t2), "state: save -> load -> save keeps chain, options, snapshots, extras",
               t2.toXmlString().substring (0, 300).toStdString());
    }

    b.proc.recallSnapshot (0);
    check (countDifferences (snapA, currentValues (b.proc)) == 0 && b.proc.getChainOrder() == order,
           "state: snapshot A content survives the round trip");
    b.proc.recallSnapshot (2);
    check (countDifferences (snapC, currentValues (b.proc)) == 0
               && b.proc.getChainOrder() == std::vector<ps::ModuleType> { ps::ModuleType::Exciter },
           "state: snapshot C content survives the round trip");

    // Robust loading
    {
        Host c;
        const auto before = stateOf (c.proc);
        const char junk[] = "this is not a state";
        c.proc.setStateInformation (junk, (int) sizeof (junk));
        c.proc.setStateInformation (nullptr, 0);
        check (stateOf (c.proc) == before, "state: garbage / empty data ignored");

        setStateXml (c.proc, "<SomethingElse a=\"1\"/>");
        check (stateOf (c.proc) == before, "state: foreign root tag ignored");

        // A bare APVTS tree (older / foreign wrapper): listed values applied, the rest at default, default chain.
        c.set ("eq1.a.b1.gain", 4.0f);
        setStateXml (c.proc, "<PARAMS><PARAM id=\"max.gain\" value=\"6.5\"/><PARAM id=\"dyn.bands\" value=\"3\"/></PARAMS>");
        check (approx (c, "max.gain", 6.5f) && approx (c, "dyn.bands", 3.0f) && approx (c, "eq1.a.b1.gain", 0.0f)
                   && c.proc.getChainOrder() == defaultChain(),
               "state: bare APVTS state -> listed values, others default, default chain");

        // Unknown ids, out of range, NaN, unknown and duplicate modules, the skeleton's UiState wrapper.
        setStateXml (c.proc,
                     "<PureSheen><PARAMS>"
                     "<PARAM id=\"max.gain\" value=\"99\"/><PARAM id=\"eq1.a.b2.freq\" value=\"5\"/>"
                     "<PARAM id=\"img.b1.width\" value=\"nan\"/><PARAM id=\"future.param\" value=\"1\"/>"
                     "<PARAM id=\"eq1.a.b3.shape\" value=\"7.4\"/><PARAM id=\"dyn.bands\" value=\"2.6\"/>"
                     "</PARAMS><Chain><Module type=\"exc\"/><Module type=\"warp\"/><Module type=\"exc\"/>"
                     "<Module type=\"eq2\"/></Chain>"
                     "<UiState><Options meterType=\"5\"/></UiState></PureSheen>");
        check (approx (c, "max.gain", 20.0f) && approx (c, "eq1.a.b2.freq", 20.0f) && approx (c, "img.b1.width", 0.0f)
                   && approx (c, "eq1.a.b3.shape", 7.0f) && approx (c, "dyn.bands", 3.0f),
               "state: out-of-range clamped, NaN -> default, choices / steps snapped, unknown ids ignored",
               str (c.get ("max.gain"), " ", c.get ("eq1.a.b2.freq"), " ", c.get ("img.b1.width"), " ",
                    c.get ("eq1.a.b3.shape"), " ", c.get ("dyn.bands")));
        check (c.proc.getChainOrder() == std::vector<ps::ModuleType> { ps::ModuleType::Exciter, ps::ModuleType::Equalizer2 },
               "state: unknown / duplicate modules dropped from the chain", chainText (c.proc.getChainOrder()));
        check (c.proc.getUiState().getProperty ("meterType") == juce::var ("5"), "state: skeleton UiState wrapper read");

        setStateXml (c.proc, "<PureSheen><Chain/></PureSheen>");
        check (c.proc.getChainOrder().empty() && approx (c, "max.gain", defaultOf ("max.gain")),
               "state: empty chain is kept empty, missing params -> defaults");
    }

    // Bypass and gain match are restored by the project state (they are only excluded from undo / presets).
    {
        Host d;
        d.set ("io.bypass", 1.0f);
        d.set ("io.gainMatch", 1.0f);
        const auto s = stateOf (d.proc);
        Host e;
        e.proc.setStateInformation (s.getData(), (int) s.getSize());
        check (approx (e, "io.bypass", 1.0f) && approx (e, "io.gainMatch", 1.0f), "state: bypass / gain match restored");
    }
}

//==============================================================================
void testUndo()
{
    std::cout << "\n-- Undo history and snapshots\n";
    Host h;
    h.prepare();
    auto& p = h.proc;

    check (p.getHistory().size() == 1 && p.getHistory()[0].description == "Initial state" && p.getHistoryPosition() == 0
               && ! p.canUndo() && ! p.canRedo(),
           "history: fresh instance has only \"Initial state\"");

    p.setParameterValue ("eq1.a.b3.gain", 2.5f);
    check (p.getHistory().size() == 2 && p.getHistory()[1].description == "EQ 1 Band 3 Gain (A): +2.5 dB",
           "history: programmatic edit -> one step with a readable description",
           p.getHistory().back().description.toStdString());

    p.setParameterValue ("eq1.a.b3.gain", 3.0f);
    check (p.getHistory().size() == 2 && p.getHistory()[1].description == "EQ 1 Band 3 Gain (A): +3.0 dB",
           "history: repeated edits of one parameter within 250 ms coalesce", str (p.getHistory().size(), " entries"));

    juce::Thread::sleep (300);
    p.setParameterValue ("eq1.a.b3.freq", 1250.0f);
    check (p.getHistory().size() == 3 && p.getHistory()[2].description == "EQ 1 Band 3 Freq (A): 1.25 kHz",
           "history: a new edit after 250 ms is a new step", p.getHistory().back().description.toStdString());

    p.setParameterValue ("eq1.a.b3.freq", 1250.0f);
    check (p.getHistory().size() == 3, "history: setting the same value records nothing");

    // Gestures (UI slider / host knob): committed after ~250 ms without a new gesture.
    h.gesture ("dyn.a.b1.comp.thr", -12.0f);
    p.flushPendingChanges (false);
    check (p.getHistory().size() == 3 && p.canUndo(), "history: a gesture waits ~250 ms before it is committed");
    juce::Thread::sleep (300);
    p.flushPendingChanges (false);
    check (p.getHistory().size() == 4 && p.getHistory()[3].description == "Dynamics Band 1 Comp Threshold (A): -12.0 dB",
           "history: gesture committed after 250 ms", p.getHistory().back().description.toStdString());

    // A drag = many values inside one gesture = one step; two quick gestures on different params = one step.
    if (auto* prm = p.getParameter ("max.gain"))
    {
        prm->beginChangeGesture();
        for (int k = 1; k <= 20; ++k)
            prm->setValueNotifyingHost (prm->convertTo0to1 (0.25f * (float) k));
        prm->endChangeGesture();
    }
    h.gesture ("max.ceiling", -0.5f);
    p.flushPendingChanges (true);
    check (p.getHistory().size() == 5 && p.getHistory()[4].description.startsWith ("Maximizer Output Level: -0.5 dB")
               && p.getHistory()[4].description.contains ("+1 more"),
           "history: a drag plus a quick second gesture -> one step", p.getHistory().back().description.toStdString());

    // Host automation without a gesture is not recorded.
    h.set ("exc.a.b1.drive", 40.0f);
    p.flushPendingChanges (true);
    check (p.getHistory().size() == 5, "history: automation without a gesture is not recorded");
    h.set ("exc.a.b1.drive", 0.0f);

    // Monitoring parameters are not recorded.
    p.setParameterValue ("io.bypass", 1.0f);
    p.setParameterValue ("io.gainMatch", 1.0f);
    check (p.getHistory().size() == 5, "history: bypass / gain match are not recorded");

    // Undo / redo / jump
    p.undo();
    check (p.getHistoryPosition() == 3 && approx (h, "max.gain", 0.0f) && approx (h, "max.ceiling", -1.0f)
               && approx (h, "io.bypass", 1.0f),
           "history: undo restores the previous state (bypass untouched)",
           str (h.get ("max.gain"), " ", h.get ("max.ceiling")));
    p.redo();
    check (p.getHistoryPosition() == 4 && approx (h, "max.gain", 5.0f) && approx (h, "max.ceiling", -0.5f), "history: redo");
    p.jumpToHistory (0);
    check (p.getHistoryPosition() == 0 && countDifferences (currentValues (p), psplug::defaultValues()) == 2   // bypass, gm
               && p.canRedo() && ! p.canUndo(),
           "history: jump to \"Initial state\" restores every default, later entries kept",
           str (countDifferences (currentValues (p), psplug::defaultValues())));
    p.jumpToHistory (4);
    check (approx (h, "max.gain", 5.0f) && approx (h, "eq1.a.b3.gain", 3.0f) && approx (h, "dyn.a.b1.comp.thr", -12.0f),
           "history: jump to the newest entry restores everything");
    p.jumpToHistory (2);
    p.setParameterValue ("img.b1.width", 20.0f);
    check (p.getHistory().size() == 4 && p.getHistoryPosition() == 3 && ! p.canRedo(),
           "history: an edit after jumping back drops the redo entries");
    p.setParameterValue ("io.bypass", 0.0f);
    p.setParameterValue ("io.gainMatch", 0.0f);

    // Chain edits
    p.clearHistory();
    check (p.getHistory().size() == 1 && p.getHistory()[0].description == "Initial state" && approx (h, "img.b1.width", 20.0f),
           "history: clear keeps the current state as \"Initial state\"",
           str (p.getHistory().size(), " ", h.get ("img.b1.width")));

    p.setParameterValue ("eq2.a.b1.gain", 5.0f);   // settings of a module that is not in the chain
    p.addModule (ps::ModuleType::Equalizer2);
    const std::vector<ps::ModuleType> withEq2 { ps::ModuleType::Equalizer1, ps::ModuleType::Dynamics, ps::ModuleType::Imager,
                                                ps::ModuleType::Exciter, ps::ModuleType::Equalizer2,
                                                ps::ModuleType::Maximizer };
    check (p.getChainOrder() == withEq2 && p.getHistory().back().description == "Added Equalizer 2"
               && approx (h, "eq2.a.b1.gain", 0.0f),
           "chain: addModule inserts before the Maximizer, at defaults, \"Added Equalizer 2\"", chainText (p.getChainOrder()));
    p.addModule (ps::ModuleType::Equalizer2);
    check (p.getHistory().size() == 3, "chain: adding a module twice does nothing");
    p.undo();
    check (p.getChainOrder() == defaultChain() && approx (h, "eq2.a.b1.gain", 5.0f), "chain: undo of add");
    p.redo();
    check (p.getChainOrder() == withEq2, "chain: redo of add");

    p.removeModule (ps::ModuleType::Exciter);
    check (! p.isModuleInChain (ps::ModuleType::Exciter) && p.getHistory().back().description == "Removed Exciter",
           "chain: removeModule, \"Removed Exciter\"");
    p.setChainOrder ({ ps::ModuleType::Maximizer, ps::ModuleType::Equalizer1 });
    check (p.getHistory().back().description == "Changed modules", "chain: several changes at once");
    p.setChainOrder ({ ps::ModuleType::Equalizer1, ps::ModuleType::Maximizer });
    check (p.getHistory().back().description == "Changed module order", "chain: reorder");
    p.addModule (ps::ModuleType::Imager, 0);
    check (p.getChainOrder().front() == ps::ModuleType::Imager, "chain: addModule at an explicit index");
    p.undo();
    p.undo();
    p.undo();
    p.undo();
    check (p.getChainOrder() == withEq2, "chain: four undos walk back through the chain edits", chainText (p.getChainOrder()));

    // Solo is cleared when its module leaves the chain.
    p.setSoloModule ((int) ps::ModuleType::Dynamics);
    check (p.getSoloModule() == (int) ps::ModuleType::Dynamics, "monitor: solo forwarded");
    p.removeModule (ps::ModuleType::Dynamics);
    check (p.getSoloModule() == -1, "monitor: removing the soloed module clears solo");
    p.undo();

    // Reset module
    p.setParameterValue ("eq1.on", 0.0f);
    p.setParameterValues ({ { "eq1.a.b2.on", 1.0f }, { "eq1.a.b2.gain", -4.0f }, { "eq1.mode", 1.0f } }, "Linked edit");
    check (p.getHistory().back().description == "Linked edit" && approx (h, "eq1.a.b2.gain", -4.0f),
           "history: setParameterValues = one step with the given description",
           str (p.getHistory().back().description, " ", h.get ("eq1.a.b2.gain")));
    const int sizeBefore = (int) p.getHistory().size();
    p.resetModule (ps::ModuleType::Equalizer1);
    check ((int) p.getHistory().size() == sizeBefore + 1 && p.getHistory().back().description == "Reset Equalizer 1"
               && approx (h, "eq1.a.b2.gain", 0.0f) && approx (h, "eq1.mode", 0.0f) && approx (h, "eq1.a.b3.gain", 0.0f)
               && approx (h, "eq1.on", 0.0f),
           "history: reset module = one step, power switch kept");
    p.undo();
    check (approx (h, "eq1.a.b2.gain", -4.0f) && approx (h, "eq1.a.b3.gain", 3.0f), "history: undo of reset module",
           str (h.get ("eq1.a.b2.gain"), " ", h.get ("eq1.a.b3.gain")));
    p.setParameterValue ("eq1.on", 1.0f);

    // Depth
    p.setHistoryDepth (10);
    for (int k = 0; k < 20; ++k)
        p.setParameterValue ("eq1.a.b" + juce::String (k % 8 + 1) + ".freq", 100.0f + 37.0f * (float) k);
    check (p.getHistory().size() == 11 && p.getHistoryPosition() == 10, "history: depth 10 keeps 10 undo steps",
           str (p.getHistory().size()));
    for (int k = 0; k < 12; ++k)
        p.undo();
    check (p.getHistoryPosition() == 0 && ! p.canUndo(), "history: undo stops at the oldest kept entry");
    p.setHistoryDepth (5);
    check (p.getHistory().size() <= 11, "history: lowering the depth below the position keeps the current entry");
    p.setHistoryDepth (3);
    p.setHistoryDepth (200);
    p.jumpToHistory ((int) p.getHistory().size() - 1);

    // Unique ids
    std::set<int> ids;
    for (const auto& e : p.getHistory())
        ids.insert (e.id);
    check (ids.size() == p.getHistory().size(), "history: entry ids unique");

    // Snapshots
    p.clearHistory();
    const auto s0 = currentValues (p);
    p.storeSnapshot (0);
    check (p.hasSnapshot (0) && p.getActiveSnapshot() == 0, "snapshot: store A -> active A");
    p.setParameterValue ("max.gain", 9.0f);
    p.removeModule (ps::ModuleType::Imager);
    check (p.getActiveSnapshot() == -1, "snapshot: edited since -> no active snapshot");
    p.storeSnapshot (1);
    const auto s1 = currentValues (p);
    p.recallSnapshot (0);
    check (countDifferences (s0, currentValues (p)) == 0 && p.isModuleInChain (ps::ModuleType::Imager)
               && p.getActiveSnapshot() == 0 && p.getHistory().back().description == "Recalled snapshot A",
           "snapshot: recall A restores params + chain, one step \"Recalled snapshot A\"");
    p.undo();
    check (countDifferences (s1, currentValues (p)) == 0, "snapshot: recall is undoable");
    p.storeSnapshotFromHistory (3, 0);
    p.recallSnapshot (3);
    check (countDifferences (s0, currentValues (p)) == 0, "snapshot: stored from a history entry");
    p.clearSnapshot (1);
    check (! p.hasSnapshot (1) && ! p.hasSnapshot (2) && ! p.hasSnapshot (-1) && ! p.hasSnapshot (4),
           "snapshot: clear, empty and out-of-range slots");
    const auto histSize = p.getHistory().size();
    p.recallSnapshot (2);
    check (p.getHistory().size() == histSize, "snapshot: recalling an empty slot does nothing");

    // Learn crossovers from a fed spectrum with dips at 180 Hz, 1.5 kHz, 7 kHz.
    {
        const double binHz = kRate / 4096.0;
        std::vector<float> spectrum (2049);
        for (size_t k = 0; k < spectrum.size(); ++k)
        {
            const double f = std::max (1.0, (double) k * binHz);
            double level = -30.0 - 3.0 * std::log2 (f / 1000.0);
            for (double dip : { 180.0, 1500.0, 7000.0 })
            {
                const double oct = std::log2 (f / dip);
                level -= 12.0 * std::exp (-0.5 * oct * oct / (0.15 * 0.15));
            }
            spectrum[k] = (float) level;
        }

        auto closeTo = [] (float got, double want) { return std::abs (std::log2 ((double) got / want)) < 1.0 / 6.0; };

        p.setParameterValue ("dyn.bands", 4.0f);
        p.feedLearnSpectrum (ps::ModuleType::Dynamics, spectrum, binHz);
        const auto before = p.getHistory().size();
        p.learnCrossovers (ps::ModuleType::Dynamics);
        check (closeTo (h.get ("dyn.xover1"), 180.0) && closeTo (h.get ("dyn.xover2"), 1500.0) && closeTo (h.get ("dyn.xover3"), 7000.0),
               "learn: 4 bands -> crossovers at the spectral dips (180 Hz, 1.5 kHz, 7 kHz)",
               str (h.get ("dyn.xover1"), " / ", h.get ("dyn.xover2"), " / ", h.get ("dyn.xover3")));
        check (p.getHistory().size() == before + 1 && p.getHistory().back().description == "Learned crossovers (Dynamics)",
               "learn: one undo step");

        p.setParameterValue ("img.bands", 2.0f);
        p.feedLearnSpectrum (ps::ModuleType::Imager, spectrum, binHz);
        p.learnCrossovers (ps::ModuleType::Imager);
        check (closeTo (h.get ("img.xover1"), 180.0), "learn: 2 bands -> the low dip", str (h.get ("img.xover1")));

        p.setParameterValue ("exc.bands", 3.0f);
        p.setParameterValue ("exc.xover1", 500.0f);
        p.learnCrossovers (ps::ModuleType::Exciter);   // nothing fed -> defaults
        auto isDefault = [&] (const char* id) { return psplug::sameValue (idx (id), h.get (id), defaultOf (id)); };
        check (isDefault ("exc.xover1") && isDefault ("exc.xover2"), "learn: no spectrum -> default crossovers",
               str (h.get ("exc.xover1"), " ", h.get ("exc.xover2")));

        std::vector<float> silent (2049, -200.0f);
        p.feedLearnSpectrum (ps::ModuleType::Dynamics, silent, binHz);
        p.learnCrossovers (ps::ModuleType::Dynamics);
        check (isDefault ("dyn.xover1") && isDefault ("dyn.xover3"), "learn: silent spectrum -> default crossovers",
               str (h.get ("dyn.xover1")));

        const auto n = p.getHistory().size();
        p.learnCrossovers (ps::ModuleType::Maximizer);
        check (p.getHistory().size() == n, "learn: not a multiband module -> nothing");
    }
}

//==============================================================================
void testMaximizerLearn()
{
    std::cout << "\n-- Maximizer learn\n";
    Host h;
    h.prepare();
    h.proc.setParameterValue ("max.learnTarget", -12.0f);
    // Pink-ish noise at roughly -26 LUFS.
    const auto input = noiseInput (0.08f, 99);
    const auto inputLoudness = [&]
    {
        const auto rec = recordInput (48000 * 3, noiseInput (0.08f, 99));
        return ps::measureIntegratedLufs (rec.l.data(), rec.r.data(), (long long) rec.l.size(), kRate);
    }();

    h.proc.startMaximizerLearn();
    const bool started = h.proc.isMaximizerLearning();
    float progress = 0.0f;
    for (int s = 0; s < 8 && h.proc.isMaximizerLearning(); ++s)
    {
        h.stream (48000, input);
        progress = juce::jmax (progress, h.proc.getMaximizerLearnProgress());
        h.proc.flushPendingChanges (true);
    }
    const float gain = h.get ("max.gain");
    const float expected = -12.0f - inputLoudness;
    check (started, "[dsp] max learn: isMaximizerLearning right after start");
    check (! h.proc.isMaximizerLearning(), "max learn: learning ends (result collected by the timer tick)");
    check (std::abs (gain - expected) < 3.0f && h.proc.getHistory().back().description.startsWith ("Learned Maximizer gain"),
           str ("[dsp] max learn: gain written as one step (input ", inputLoudness, " LUFS, target -12)"),
           str ("gain ", gain, " expected ~", expected, ", last step ", h.proc.getHistory().back().description));
    check (progress > 0.5f, "[dsp] max learn: progress reported", str (progress));

    // Cancel: listening stops at once, the gain stays, nothing is recorded.
    const auto steps = h.proc.getHistory().size();
    h.proc.startMaximizerLearn();
    h.stream (24000, input);
    h.proc.cancelMaximizerLearn();
    h.stream (48000 * 6, input);
    h.proc.flushPendingChanges (true);
    check (! h.proc.isMaximizerLearning() && h.get ("max.gain") == gain && h.proc.getHistory().size() == steps,
           "max learn: cancel stops listening and keeps the gain");
}

//==============================================================================
// The Maximizer's output contract through the whole default chain: pushed hard, the true peak of the processor
// output stays at the Output Level (+0.05 dB), in every mode.
void testMaximizerCeiling()
{
    std::cout << "\n-- Maximizer ceiling through the chain\n";
    bool ok = true;
    std::string detail;
    float worst = -200.0f;

    for (int mode = 0; mode < (int) ps::MaximizerMode::Count; ++mode)
    {
        Host h;
        h.prepare();
        h.set ("max.mode", (float) mode);
        h.set ("max.gain", 18.0f);
        h.set ("max.ceiling", -1.0f);
        h.stream (9600, musicLikeInput (0.25f));
        const auto out = h.stream (48000, musicLikeInput (0.25f, 21));
        ps::TruePeakMeter tp;
        tp.prepare (kRate);
        tp.process (out.l.data(), out.r.data(), (int) out.l.size());
        const float peak = tp.truePeakDb();
        worst = juce::jmax (worst, peak);
        if (! (peak <= -1.0f + 0.05f) || ! std::isfinite (peak))
        {
            ok = false;
            detail += str ("mode ", mode, ": ", peak, " dBTP; ");
        }
    }

    check (ok, str ("[dsp] maximizer: +18 dB into the default chain stays under -1 dBTP in every mode (worst ", worst,
                    " dBTP)"), detail);
}

//==============================================================================
void testPresets()
{
    std::cout << "\n-- Presets\n";
    const auto root = tempDir.getChildFile ("Presets");
    Host h;
    h.prepare();
    auto& p = h.proc;
    p.setPresetRoot (root);
    check (p.getPresetRoot() == root, "presets: root overridable");

    // Factory content
    {
        bool idsOk = true, rangeOk = true;
        std::string bad;
        for (const auto& f : psplug::factoryPresets())
        {
            for (const auto& [rawId, v] : f.values)
            {
                for (int eq = 0; eq < (f.scope == (int) ps::ModuleType::Equalizer1 ? 2 : 1); ++eq)
                {
                    auto id = rawId;
                    if (f.scope == (int) ps::ModuleType::Equalizer1 && id.rfind ("eq.", 0) == 0)
                        id = (eq == 0 ? "eq1" : "eq2") + id.substr (2);
                    const int i = ps::paramIndex (id);
                    if (i < 0)
                    {
                        idsOk = false;
                        bad += f.name.toStdString() + ":" + id + " ";
                        continue;
                    }
                    if (psplug::sanitiseValue (i, v) != v)
                    {
                        rangeOk = false;
                        bad += f.name.toStdString() + ":" + id + "=" + std::to_string (v) + " ";
                    }
                    if (! psplug::isInPresetScope (i, f.scope < 0 ? -1 : (eq == 0 ? f.scope : (int) ps::ModuleType::Equalizer2)))
                    {
                        idsOk = false;
                        bad += f.name.toStdString() + ":" + id + "(scope) ";
                    }
                }
            }
        }
        check (idsOk && rangeOk, "presets: every factory preset value names an existing parameter of its scope, in range",
               bad);

        const auto global = p.getPresets (-1);
        std::set<juce::String> categories, names;
        for (const auto& g : global)
        {
            categories.insert (g.category);
            names.insert (g.name);
        }
        check (global.size() >= 12 && global[0].name == "Default" && global[0].category == "Start Here"
                   && names.size() == global.size(),
               str ("presets: ", global.size(), " global factory presets, unique names, \"Default\" first"));
        check (categories.count ("Start Here") && categories.count ("EDM") && categories.count ("Gentle"),
               str ("presets: categories (", categories.size(), ")"));

        bool perModule = true;
        std::string counts;
        for (int t = 0; t < ps::kNumModuleTypes; ++t)
        {
            const auto n = p.getPresets (t).size();
            counts += str (ps::moduleInfo ((ps::ModuleType) t).idPrefix, "=", n, " ");
            perModule = perModule && n >= 2 && n <= 6;
        }
        check (perModule, "presets: 2-6 factory presets per module, phase 2 included (EQ 1 / EQ 2 share theirs)", counts);

        // Every factory preset loads and produces finite audio.
        bool allLoad = true, finite = true;
        for (int scope = -1; scope < ps::kNumModuleTypes; ++scope)
            for (const auto& info : p.getPresets (scope))
            {
                allLoad = allLoad && p.loadPreset (scope, info);
                finite = finite && allFinite (h.stream (2048, musicLikeInput (0.2f)));
            }
        check (allLoad && finite, "presets: every factory preset loads and plays finite audio");
    }

    // Every phase 2 module preset, with its module in the chain: it loads as one step, moves at least one of the
    // module's parameters off its default, and the module plays finite audio with it (in its preset channel mode).
    {
        Host m;
        m.prepare();
        for (auto t : phase2Types())
            m.proc.addModule (t);
        m.proc.flushPendingChanges (true);
        const auto defaults = currentValues (m.proc);
        bool ok = true;
        int count = 0;
        std::string bad;
        for (auto t : phase2Types())
            for (const auto& info : m.proc.getPresets ((int) t))
            {
                if (! info.factory)
                    continue;
                ++count;
                const bool loaded = m.proc.loadPreset ((int) t, info);
                m.proc.flushPendingChanges (true);
                const auto v = currentValues (m.proc);
                bool moved = false;
                const auto& specs = ps::paramSpecs();
                for (size_t i = 0; i < specs.size(); ++i)
                    if (specs[i].module == (int) t && ! psplug::sameValue ((int) i, v[i], defaults[i]))
                        moved = true;
                const bool finiteOut = allFinite (m.stream (12000, musicLikeInput (0.2f)));
                const bool oneStep = m.proc.getHistory().back().description.startsWith ("Loaded ")
                                     && m.proc.getHistoryPosition() + 1 == (int) m.proc.getHistory().size();
                if (! (loaded && moved && finiteOut && oneStep))
                {
                    ok = false;
                    bad += str (ps::moduleInfo (t).idPrefix, ":", info.name.toStdString(), " ");
                }
                m.proc.loadPreset (-1, m.proc.getPresets (-1)[0]);   // Default: back to neutral
                for (auto t2 : phase2Types())
                    m.proc.addModule (t2);
                m.proc.flushPendingChanges (true);
            }
        check (ok && count >= 4 * (int) phase2Types().size(),
               str ("presets: all ", count, " phase 2 module presets load, change their module and play finite audio"),
               bad);
    }

    // Load a factory preset
    p.loadPreset (-1, p.getPresets (-1)[0]);
    p.setParameterValue ("io.bypass", 1.0f);
    PureSheenProcessor::PresetInfo warm;
    for (const auto& g : p.getPresets (-1))
        if (g.name == "Warm Glue")
            warm = g;
    check (p.loadPreset (-1, warm), "presets: load \"Warm Glue\"");
    check (p.getCurrentPresetName() == "Warm Glue" && ! p.isPresetModified()
               && p.getHistory().back().description == "Loaded preset Warm Glue",
           "presets: name set, not modified, one step \"Loaded preset Warm Glue\"");
    check (approx (h, "dyn.a.b1.comp.thr", -20.0f) && h.get ("max.mode") == (float) ps::MaximizerMode::Smooth
               && p.getChainOrder() == defaultChain() && approx (h, "io.bypass", 1.0f),
           "presets: values and chain applied, bypass untouched");
    p.setParameterValue ("io.bypass", 0.0f);
    p.setParameterValue ("max.gain", 7.0f);
    check (p.isPresetModified(), "presets: an edit marks the preset modified");
    p.undo();
    check (! p.isPresetModified(), "presets: undoing the edit clears the modified flag");

    PureSheenProcessor::PresetInfo limitOnly;
    for (const auto& g : p.getPresets (-1))
        if (g.name == "Limit Only")
            limitOnly = g;
    p.loadPreset (-1, limitOnly);
    check (p.getChainOrder() == std::vector<ps::ModuleType> { ps::ModuleType::Maximizer }, "presets: preset chain applied");
    p.undo();
    check (p.getChainOrder() == defaultChain() && approx (h, "dyn.a.b1.comp.thr", -20.0f), "presets: preset load is undoable");

    // Save / load a user global preset
    p.setParameterValue ("eq1.a.b4.on", 1.0f);
    p.setParameterValue ("eq1.a.b4.gain", 3.5f);
    p.removeModule (ps::ModuleType::Exciter);
    const auto saved = currentValues (p);
    const auto savedOrder = p.getChainOrder();
    check (p.savePreset (-1, "My Master", "Club"), "presets: save a global user preset");
    const auto file = root.getChildFile ("Global/Club/My Master.pspreset");
    check (file.existsAsFile() && p.getCurrentPresetName() == "My Master" && ! p.isPresetModified(),
           "presets: file in <root>/Global/Club, current name set", file.getFullPathName().toStdString());

    PureSheenProcessor::PresetInfo mine;
    for (const auto& g : p.getPresets (-1))
        if (! g.factory && g.name == "My Master")
            mine = g;
    check (mine.file == file && mine.category == "Club", "presets: user preset listed with its category");

    p.loadPreset (-1, p.getPresets (-1)[0]);   // Default
    check (p.getChainOrder() == defaultChain() && approx (h, "eq1.a.b4.gain", 0.0f), "presets: Default resets everything");
    check (p.loadPreset (-1, mine), "presets: load the user preset");
    std::string first;
    check (countDifferences (saved, currentValues (p), &first) == 0 && p.getChainOrder() == savedOrder,
           "presets: user preset restores every parameter and the chain", first);

    // Module presets
    p.setParameterValue ("dyn.a.b1.comp.thr", -33.0f);
    p.setParameterValue ("dyn.bands", 2.0f);
    check (p.savePreset ((int) ps::ModuleType::Dynamics, "Glue Test"), "presets: save a module preset");
    check (root.getChildFile ("dyn/Glue Test.pspreset").existsAsFile(), "presets: module preset in <root>/dyn");
    PureSheenProcessor::PresetInfo glue;
    for (const auto& g : p.getPresets ((int) ps::ModuleType::Dynamics))
        if (! g.factory && g.name == "Glue Test")
            glue = g;
    check (glue.category == "User", "presets: file directly in the scope folder -> category \"User\"");

    p.setParameterValue ("dyn.a.b1.comp.thr", -5.0f);
    p.setParameterValue ("dyn.on", 0.0f);
    p.setParameterValue ("max.gain", 11.0f);
    const auto order = p.getChainOrder();
    const auto name = p.getCurrentPresetName();
    check (p.loadPreset ((int) ps::ModuleType::Dynamics, glue), "presets: load the module preset");
    check (approx (h, "dyn.a.b1.comp.thr", -33.0f) && approx (h, "dyn.bands", 2.0f) && approx (h, "dyn.on", 0.0f)
               && approx (h, "max.gain", 11.0f) && p.getChainOrder() == order && p.getCurrentPresetName() == name,
           "presets: module preset sets only its module (power, other modules, chain and global name untouched)");
    check (p.getHistory().back().description == "Loaded Dynamics preset Glue Test", "presets: module preset load is one step",
           p.getHistory().back().description.toStdString());
    p.setParameterValue ("dyn.on", 1.0f);

    // EQ presets are interchangeable between EQ 1 and EQ 2.
    p.setParameterValue ("eq1.a.b6.on", 1.0f);
    p.setParameterValue ("eq1.a.b6.freq", 3333.0f);
    p.savePreset ((int) ps::ModuleType::Equalizer1, "Cross EQ");
    PureSheenProcessor::PresetInfo cross;
    for (const auto& g : p.getPresets ((int) ps::ModuleType::Equalizer1))
        if (! g.factory && g.name == "Cross EQ")
            cross = g;
    check (p.loadPreset ((int) ps::ModuleType::Equalizer2, cross) && std::abs (h.get ("eq2.a.b6.freq") - 3333.0f) < 0.5f
               && approx (h, "eq2.a.b6.on", 1.0f),
           "presets: an EQ 1 user preset loads into EQ 2");
    PureSheenProcessor::PresetInfo airLift;
    for (const auto& g : p.getPresets ((int) ps::ModuleType::Equalizer2))
        if (g.factory && g.name == "Air Lift")
            airLift = g;
    check (p.loadPreset ((int) ps::ModuleType::Equalizer2, airLift) && approx (h, "eq2.a.b1.on", 1.0f)
               && h.get ("eq2.a.b1.shape") == (float) ps::EqShape::HighShelfBaxandall,
           "presets: factory EQ presets resolve to EQ 2");
    check (! p.loadPreset ((int) ps::ModuleType::Imager, glue), "presets: a Dynamics preset does not load into the Imager");

    // Robust reading
    {
        const auto foreign = root.getChildFile ("Global/Foreign.pspreset");
        foreign.replaceWithText ("<PureSheenPreset format=\"9\" scope=\"global\" name=\"Foreign\">"
                                 "<Chain><Module type=\"max\"/><Module type=\"zzz\"/></Chain>"
                                 "<Params><P id=\"max.gain\" v=\"55\"/><P id=\"new.thing\" v=\"1\"/>"
                                 "<P id=\"io.bypass\" v=\"1\"/></Params></PureSheenPreset>");
        PureSheenProcessor::PresetInfo info { "Foreign", "User", false, foreign };
        check (p.loadPreset (-1, info) && approx (h, "max.gain", 20.0f) && approx (h, "io.bypass", 0.0f)
                   && p.getChainOrder() == std::vector<ps::ModuleType> { ps::ModuleType::Maximizer }
                   && approx (h, "dyn.a.b1.comp.thr", defaultOf ("dyn.a.b1.comp.thr")),
               "presets: foreign file -> clamped, unknown ids / modules ignored, bypass never loaded, missing = default");

        const auto broken = root.getChildFile ("Global/Broken.pspreset");
        broken.replaceWithText ("<PureSheenPreset scope=\"global\"><Params><P id=");
        const auto before = currentValues (p);
        const auto historyBefore = p.getHistory().size();
        check (! p.loadPreset (-1, { "Broken", "User", false, broken }) && countDifferences (before, currentValues (p)) == 0
                   && p.getHistory().size() == historyBefore,
               "presets: malformed file -> false, nothing changed");
        check (! p.loadPreset (-1, { "Missing", "User", false, root.getChildFile ("Global/none.pspreset") }),
               "presets: missing file -> false");
        check (! p.loadPreset (-1, { "Nope", "Start Here", true, {} }), "presets: unknown factory preset -> false");
        broken.deleteFile();
        foreign.deleteFile();
    }

    // Names
    check (! p.savePreset (-1, "   "), "presets: empty name refused");
    check (p.savePreset (-1, "A/B:C*?"), "presets: illegal file name characters sanitised");
    bool sanitised = false;
    for (const auto& g : p.getPresets (-1))
        sanitised = sanitised || (! g.factory && g.file.existsAsFile() && g.file.getParentDirectory() == root.getChildFile ("Global"));
    check (sanitised, "presets: sanitised preset saved in the Global folder");

    // Delete
    const auto countBefore = p.getPresets (-1).size();
    check (p.deletePreset (mine) && ! file.existsAsFile() && p.getPresets (-1).size() == countBefore - 1,
           "presets: delete a user preset");
    check (! p.deletePreset (p.getPresets (-1)[0]), "presets: factory presets cannot be deleted");
    check (! p.deletePreset ({ "x", "User", false, tempDir.getChildFile ("outside.pspreset") }),
           "presets: files outside the preset root are never deleted");

    // Next / previous
    p.loadPreset (-1, p.getPresets (-1)[0]);
    p.loadNextPreset (1);
    const auto list = p.getPresets (-1);
    check (p.getCurrentPresetName() == list[1].name, "presets: next", p.getCurrentPresetName().toStdString());
    p.loadNextPreset (-1);
    p.loadNextPreset (-1);
    check (p.getCurrentPresetName() == list.back().name, "presets: previous wraps around",
           p.getCurrentPresetName().toStdString());

    // State keeps the preset name.
    p.loadPreset (-1, list[2]);
    p.setParameterValue ("max.gain", 1.5f);
    const auto s = stateOf (p);
    Host other;
    other.proc.setStateInformation (s.getData(), (int) s.getSize());
    check (other.proc.getCurrentPresetName() == list[2].name && other.proc.isPresetModified(),
           "presets: name + modified flag saved with the project");
}

//==============================================================================
void testMonitoring()
{
    std::cout << "\n-- Monitoring forwarding\n";
    Host h;
    h.prepare();
    auto& p = h.proc;
    p.setDelta (ps::ModuleType::Imager, true);
    p.setChannelSolo (ps::ModuleType::Equalizer1, 1);
    ps::ModuleMonitor m;
    m.soloBand = 2;
    m.auditionOn = true;
    m.auditionHz = 3150.0f;
    p.setMonitor (ps::ModuleType::Dynamics, m);
    check (p.getDelta (ps::ModuleType::Imager) && ! p.getDelta (ps::ModuleType::Exciter), "monitor: delta");
    check (p.getChannelSolo (ps::ModuleType::Equalizer1) == 1 && p.getChannelSolo (ps::ModuleType::Dynamics) == -1,
           "monitor: channel solo");
    check (p.getMonitor (ps::ModuleType::Dynamics).soloBand == 2 && p.getMonitor (ps::ModuleType::Dynamics).auditionHz == 3150.0f,
           "monitor: module monitor");
    p.setSoloModule (99);
    check (p.getSoloModule() == -1, "monitor: invalid solo -> off");
    const auto before = p.getHistory().size();
    p.setSoloModule ((int) ps::ModuleType::Exciter);
    check (p.getHistory().size() == before, "monitor: not recorded in the history");

    // Delta of a module that changes nothing at neutral settings is silence.
    p.setSoloModule (-1);
    p.setDelta (ps::ModuleType::Imager, true);
    const auto out = h.stream (24000, noiseInput (0.05f));
    float peak = 0.0f;
    for (size_t i = 4096; i < out.l.size(); ++i)
        peak = juce::jmax (peak, std::abs (out.l[i]), std::abs (out.r[i]));
    check (dbOf (peak) < -100.0f, "[dsp] monitor: delta of a neutral Imager is silent", str (dbOf (peak), " dBFS"));

    // ... and not silent once it does something (so the check above is not passing on a dead delta bus).
    h.set ("img.b1.width", 60.0f);
    const auto wide = h.stream (24000, noiseInput (0.05f));
    float widePeak = 0.0f;
    for (size_t i = 4096; i < wide.l.size(); ++i)
        widePeak = juce::jmax (widePeak, std::abs (wide.l[i]), std::abs (wide.r[i]));
    check (dbOf (widePeak) > -50.0f, "[dsp] monitor: delta of a widened Imager is audible", str (dbOf (widePeak), " dBFS"));
}

//==============================================================================
// Message-thread work (chain edits, presets, undo, state loads) racing the audio thread.
void testTsPanelBindings()
{
    struct Services final : ps::ui::EditorServices
    {
        ps::ui::UiOptions options;
        ps::ui::SpectrumAnalyser spectrum;
        const ps::ui::UiOptions& getOptions() const override { return options; }
        ps::ui::SpectrumAnalyser& getModulePreSpectrum() override { return spectrum; }
        ps::ui::SpectrumAnalyser& getModulePostSpectrum() override { return spectrum; }
        ps::ui::SpectrumAnalyser& getOutputSpectrum() override { return spectrum; }
        void showHint (const juce::String&) override {}
    } services;
    PureSheenProcessor p;
    p.setParameterValue ("img.chMode", 1.0f);
    p.setParameterValue ("img.link", 0.0f);
    ps::ui::ImagerPanel panel (p, services);
    panel.setSize (1000, 500);
    panel.setChannelSet (1);
    auto& display = panel.getBandDisplay();
    const auto eventAt = [] (juce::Component& c, juce::Point<float> pos)
    {
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos,
                                juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                                1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, juce::Time(), pos, juce::Time(), 1, false);
    };
    const auto bandEvent = eventAt (display, display.getBandArea (0).getCentre());
    p.setParameterValue ("img.b1.width", 45.0f);
    p.setParameterValue ("img.b.b1.width", -55.0f);
    display.onBandDoubleClick (0, bandEvent);
    check (p.getParameterValue ("img.b1.width") == 45.0f && p.getParameterValue ("img.b.b1.width") == 0.0f,
           "UI: Imager Sustain width edit affects only set B when unlinked");

    p.setParameterValue ("img.link", 1.0f);
    panel.refresh();
    p.setParameterValue ("img.b.b1.width", -55.0f);
    display.onBandDoubleClick (0, bandEvent);
    check (p.getParameterValue ("img.b1.width") == 0.0f && p.getParameterValue ("img.b.b1.width") == 0.0f,
           "UI: Imager linked width edit updates both T/S sets");

    p.setParameterValue ("img.link", 0.0f);
    panel.refresh();
    p.setParameterValue ("img.stz.amount", 33.0f);
    p.setParameterValue ("img.b.stz.amount", 66.0f);
    std::function<ps::ui::ParamKnob* (juce::Component&)> findKnob = [&] (juce::Component& c) -> ps::ui::ParamKnob*
    {
        if (auto* knob = dynamic_cast<ps::ui::ParamKnob*> (&c); knob != nullptr && knob->getParamId() == "img.b.stz.amount")
            return knob;
        for (auto* child : c.getChildren())
            if (auto* knob = findKnob (*child))
                return knob;
        return nullptr;
    };
    auto* knob = findKnob (panel);
    if (knob != nullptr)
        knob->mouseDoubleClick (eventAt (*knob, knob->getLocalBounds().toFloat().getCentre()));
    check (knob != nullptr && p.getParameterValue ("img.stz.amount") == 33.0f
           && p.getParameterValue ("img.b.stz.amount") == 0.0f,
           "UI: Imager Sustain Stereoize control binds to set B");

    const auto clickSolo = [&] (ps::ui::MultibandDisplay& d)
    {
        const auto header = d.getBandHeaderArea (0);
        const auto e = eventAt (d, { header.getRight() - 13.0f, header.getCentreY() });
        d.mouseDown (e);
        d.mouseUp (e);
    };
    clickSolo (display);
    const auto imagerMonitor = p.getMonitor (ps::ModuleType::Imager);
    check (imagerMonitor.soloBand == 0 && imagerMonitor.soloChannelSet == 1,
           "UI: Imager band solo targets the selected Sustain part");

    p.setParameterValue ("exc.ts.on", 1.0f);
    ps::ui::ExciterPanel exciter (p, services);
    exciter.setSize (1000, 500);
    exciter.setChannelSet (1);
    clickSolo (exciter.getBandDisplay());
    const auto exciterMonitor = p.getMonitor (ps::ModuleType::Exciter);
    check (exciterMonitor.soloBand == 0 && exciterMonitor.soloChannelSet == 1,
           "UI: Exciter band solo targets the selected Sustain part");
}

template <typename Component>
Component* findPanelComponent (juce::Component& parent)
{
    if (auto* found = dynamic_cast<Component*> (&parent))
        return found;
    for (auto* child : parent.getChildren())
        if (auto* found = findPanelComponent<Component> (*child))
            return found;
    return nullptr;
}

void testPhase2PanelRouting()
{
    struct Services final : ps::ui::EditorServices
    {
        ps::ui::UiOptions options;
        ps::ui::SpectrumAnalyser spectrum;
        const ps::ui::UiOptions& getOptions() const override { return options; }
        ps::ui::SpectrumAnalyser& getModulePreSpectrum() override { return spectrum; }
        ps::ui::SpectrumAnalyser& getModulePostSpectrum() override { return spectrum; }
        ps::ui::SpectrumAnalyser& getOutputSpectrum() override { return spectrum; }
        void showHint (const juce::String&) override {}
    } services;
    const auto eventAt = [] (juce::Component& c, juce::Point<float> pos,
                            int modifiers = juce::ModifierKeys::leftButtonModifier)
    {
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos,
                                juce::ModifierKeys (modifiers),
                                1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, juce::Time(), pos, juce::Time(), 1, false);
    };

    PureSheenProcessor p;
    for (const auto type : { ps::ModuleType::Definition, ps::ModuleType::Smoother, ps::ModuleType::LowContrast })
    {
        for (const auto& [id, value] : ps::channelModeParamValues (type, ps::ChannelMode::TransientSustain))
            p.setParameterValue (id, value);
        auto panel = ps::ui::createModulePanel (p, services, type);
        panel->setSize (1000, 500);
        panel->setChannelSet (1);
        bool found = false;
        if (auto* spectral = findPanelComponent<ps::ui::SpectralDisplay> (*panel))
        {
            const auto e = eventAt (*spectral, spectral->getSoloButtonArea().getCentre());
            spectral->mouseDown (e);
            spectral->mouseUp (e);
            found = true;
        }
        else if (auto* low = findPanelComponent<ps::ui::LowRegionDisplay> (*panel))
        {
            low->setRegionSolo (true);
            found = true;
        }
        const auto sustain = p.getMonitor (type);
        panel->setChannelSet (0);
        const auto transient = p.getMonitor (type);
        panel.reset();
        check (found && sustain.soloRegion && sustain.soloChannelSet == 1
                   && transient.soloRegion && transient.soloChannelSet == 0 && ! p.getMonitor (type).soloRegion,
               str ("UI: ", ps::moduleInfo (type).name,
                    " region solo follows the selected T/S part and clears when the panel closes"));
    }

    struct Gestures final : juce::AudioProcessorParameter::Listener
    {
        int begins = 0, ends = 0;
        void parameterValueChanged (int, float) override {}
        void parameterGestureChanged (int, bool starting) override
        {
            if (starting) ++begins;
            else ++ends;
        }
    } a, b;
    p.setParameterValues ({ { "deq.chMode", 1.0f }, { "deq.link", 0.0f },
                           { "deq.a.b1.on", 1.0f }, { "deq.b.b1.on", 1.0f },
                           { "deq.a.b1.freq", 1000.0f }, { "deq.b.b1.freq", 1000.0f },
                           { "deq.a.b1.q", 1.0f }, { "deq.b.b1.q", 5.0f } }, "Routing setup");
    auto* pa = p.getParameter ("deq.a.b1.q");
    auto* pb = p.getParameter ("deq.b.b1.q");
    pa->addListener (&a);
    pb->addListener (&b);
    bool wheelRouting = false;
    {
        ps::ui::DynamicEqPanel panel (p, services);
        panel.setSize (1000, 500);
        if (auto* display = findPanelComponent<ps::ui::DynamicEqDisplay> (panel))
        {
            juce::MouseWheelDetails wheel {};
            wheel.deltaY = 0.2f;
            display->mouseWheelMove (eventAt (*display, display->getNodePosition (0)), wheel);
            const float first = p.getParameterValue ("deq.a.b1.q");
            panel.setChannelSet (1);
            display->mouseWheelMove (eventAt (*display, display->getNodePosition (0)), wheel);
            wheelRouting = first > 1.0f && p.getParameterValue ("deq.a.b1.q") == first
                           && p.getParameterValue ("deq.b.b1.q") > 5.0f
                           && a.begins == 1 && a.ends == 1 && b.begins == 1;
            p.setParameterValue ("deq.link", 1.0f);
            panel.refresh();
            wheelRouting = wheelRouting && b.ends == 1;
        }
    }
    check (wheelRouting && a.begins == a.ends && b.begins == b.ends,
           "UI: Dynamic EQ wheel bursts close on channel or Link changes and start from the new set's Q",
           str ("A Q=", p.getParameterValue ("deq.a.b1.q"), " B Q=", p.getParameterValue ("deq.b.b1.q"),
                " gestures=", a.begins, "/", a.ends, " ", b.begins, "/", b.ends));
    pa->removeListener (&a);
    pb->removeListener (&b);

    p.setParameterValues ({ { "pnch.a.b1.amount", 10.0f }, { "pnch.a.b2.amount", -10.0f },
                           { "pnch.b.b1.amount", 40.0f }, { "pnch.b.b2.amount", -40.0f } }, "Punch setup");
    a.begins = a.ends = b.begins = b.ends = 0;
    pa = p.getParameter ("pnch.a.b1.amount");
    pb = p.getParameter ("pnch.b.b1.amount");
    pa->addListener (&a);
    pb->addListener (&b);
    bool punchRouting = false;
    {
        ps::ui::PunchBandCard card (p, 0, ps::ui::Colours::accent);
        card.setSize (200, 200);
        card.bind (0, -1, ps::ui::Colours::accent);
        auto& knob = card.getKnob();
        knob.getGroup = [&] (std::vector<juce::String>& ids, std::vector<juce::String>& twins)
        {
            const auto prefix = knob.getParamId().startsWith ("pnch.a.") ? "pnch.a." : "pnch.b.";
            ids = { juce::String (prefix) + "b1.amount", juce::String (prefix) + "b2.amount" };
            twins.resize (ids.size());
        };
        juce::MouseWheelDetails wheel {};
        wheel.deltaY = 0.2f;
        const auto e = eventAt (knob, knob.getLocalBounds().toFloat().getCentre());
        knob.mouseWheelMove (e, wheel);
        const float first = p.getParameterValue ("pnch.a.b1.amount");
        const float second = p.getParameterValue ("pnch.a.b2.amount");
        card.bind (1, -1, ps::ui::Colours::sideChannel);
        knob.mouseWheelMove (e, wheel);
        punchRouting = first > 10.0f && p.getParameterValue ("pnch.a.b1.amount") == first
                       && p.getParameterValue ("pnch.a.b2.amount") == second
                       && p.getParameterValue ("pnch.b.b1.amount") > 40.0f
                       && p.getParameterValue ("pnch.b.b2.amount") > -40.0f
                       && a.begins == 1 && a.ends == 1 && b.begins == 1;
    }
    check (punchRouting && a.begins == a.ends && b.begins == b.ends,
           "UI: Punch linked-band wheel edits close on channel changes and edit only the new set",
           str ("A=", p.getParameterValue ("pnch.a.b1.amount"), " B=", p.getParameterValue ("pnch.b.b1.amount"),
                " gestures=", a.begins, "/", a.ends, " ", b.begins, "/", b.ends));
    pa->removeListener (&a);
    pb->removeListener (&b);

    p.setParameterValues ({ { "meq.chMode", 1.0f }, { "meq.link", 0.0f },
                           { "meq.a.lo", 100.0f }, { "meq.b.lo", 500.0f } }, "Match setup");
    const float originalSideLow = p.getParameterValue ("meq.b.lo");
    bool cutoffRouting = false;
    {
        ps::ui::MatchEqPanel panel (p, services);
        panel.setSize (1000, 500);
        if (auto* display = findPanelComponent<ps::ui::MatchDisplay> (panel))
        {
            const juce::Point<float> start (display->getCutoffX (false), display->getPlotArea().getCentreY());
            display->mouseDown (eventAt (*display, start));
            display->mouseDrag (eventAt (*display, start.translated (20.0f, 0.0f)));
            const float first = p.getParameterValue ("meq.a.lo");
            panel.setChannelSet (1);
            display->mouseDrag (eventAt (*display, start.translated (50.0f, 0.0f)));
            display->mouseUp (eventAt (*display, start.translated (50.0f, 0.0f)));
            cutoffRouting = first > 100.0f && p.getParameterValue ("meq.a.lo") == first
                            && p.getParameterValue ("meq.b.lo") == originalSideLow;
        }
    }
    check (cutoffRouting, "UI: switching Match EQ channel tabs cancels an active cutoff drag",
           str ("A low=", p.getParameterValue ("meq.a.lo"), " B low=", p.getParameterValue ("meq.b.lo")));

    for (const auto type : { ps::ModuleType::LowContrast, ps::ModuleType::Punch })
    {
        auto panel = ps::ui::createModulePanel (p, services, type);
        panel->setSize (1000, 500);
        ps::ModuleMonitor monitor;
        monitor.soloRegion = type == ps::ModuleType::LowContrast;
        monitor.soloBand = type == ps::ModuleType::Punch ? 0 : -1;
        p.setMonitor (type, monitor);
        const int alt = juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::altModifier;
        bool auditioned = false;
        if (auto* low = findPanelComponent<ps::ui::LowRegionDisplay> (*panel))
        {
            low->mouseDown (eventAt (*low, low->getPlotArea().getCentre(), alt));
            auditioned = true;
        }
        else if (auto* bands = findPanelComponent<ps::ui::MultibandDisplay> (*panel))
        {
            bands->mouseDown (eventAt (*bands, bands->getBandArea (0).getCentre(), alt));
            auditioned = true;
        }
        panel.reset();
        const auto after = p.getMonitor (type);
        check (auditioned && ! after.soloRegion && after.soloBand < 0,
               str ("UI: closing ", ps::moduleInfo (type).name,
                    " during Alt audition cannot restore a previously latched solo"));
    }
}

void testBassScopeFeed()
{
    PureSheenProcessor p;
    auto& bass = static_cast<ps::BassShaper&> (p.getChain().getModule (ps::ModuleType::BassShaper));
    bass.getScopeFifo().prepare (65536);
    bass.getMaskFifo().prepare (65536);
    ps::ui::BassScope scope (p);
    scope.setSize (400, 200);
    scope.update();
    ps::ui::BassScope shared (p);
    shared.setSize (400, 200);
    std::vector<float> lo (65537, -0.1f), hi (65537, 0.1f), mask (65536, 0.2f);
    lo[65536] = -0.7f;
    hi[65536] = 0.7f;
    // The audio producer can push a new scope frame between the two UI pulls.
    bass.getScopeFifo().push (lo.data(), hi.data(), 65537);
    bass.getMaskFifo().push (mask.data(), mask.data(), 65536);
    scope.update();
    float mn = 0.0f, mx = 0.0f, weight = 0.0f;
    check (scope.getLatestValues (mn, mx, weight) && mn == -0.1f && mx == 0.1f && weight == 0.2f,
           "UI: Bass Scope aligns independently overrun scope and mask FIFOs by sequence");
    const float lastMask = 0.9f;
    bass.getMaskFifo().push (&lastMask, &lastMask, 1);
    shared.update();
    check (scope.getLatestValues (mn, mx, weight) && mn == -0.7f && mx == 0.7f && weight == 0.9f,
           "UI: shared Bass Scopes retain an unmatched trailing frame until its mask arrives");
}

void testPromptLifetime()
{
    const auto pump = []
    {
       #if JUCE_WINDOWS || JUCE_LINUX
        const auto end = juce::Time::getMillisecondCounter() + 50u;
        while (juce::Time::getMillisecondCounter() < end)
            if (! juce::detail::dispatchNextMessageOnSystemQueue (true))
                juce::Thread::sleep (1);
       #endif
    };
    int calls = 0;
    auto parent = std::make_unique<juce::Component>();
    parent->setSize (400, 200);
    ps::ui::promptForText (*parent, "Value", "Enter a value", "12", [&] (juce::String) { ++calls; });
    auto* modalManager = juce::ModalComponentManager::getInstance();
    juce::Component::SafePointer<juce::Component> prompt (modalManager->getModalComponent (0));
    const bool opened = prompt != nullptr && prompt->isCurrentlyModal();
    parent.reset();
    const bool cancelled = prompt == nullptr || (! prompt->isCurrentlyModal() && ! prompt->isVisible());
    pump();
    check (opened && cancelled && calls == 0 && prompt == nullptr,
           "UI: closing a prompt's parent cancels the modal window and drops its callback");

    parent = std::make_unique<juce::Component>();
    parent->setSize (400, 200);
    ps::ui::promptForText (*parent, "Value", "Enter a value", "12", [&] (juce::String text)
    {
        if (text == "12") ++calls;
    });
    prompt = modalManager->getModalComponent (0);
    if (prompt != nullptr)
        prompt->exitModalState (1);
    pump();
    check (calls == 1 && prompt == nullptr, "UI: accepting a prompt with a live parent calls back once");
}

void testPanelGestures()
{
    struct Gestures final : juce::AudioProcessorParameter::Listener
    {
        int begins = 0, ends = 0;
        void parameterValueChanged (int, float) override {}
        void parameterGestureChanged (int, bool starting) override
        {
            if (starting) ++begins;
            else ++ends;
        }
    } a, b;

    PureSheenProcessor p;
    auto* pa = p.getParameter ("lowc.a.contrast");
    auto* pb = p.getParameter ("lowc.b.contrast");
    pa->addListener (&a);
    pb->addListener (&b);
    const auto eventAt = [] (juce::Component& c, float y)
    {
        const juce::Point<float> pos (40.0f, y);
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos,
                                juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                                1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, juce::Time(),
                                { 40.0f, 70.0f }, juce::Time(), 1, true);
    };
    {
        ps::ui::ParamKnob knob (p, "lowc.a.contrast");
        knob.setSize (80, 100);
        knob.setLinkedParamId ("lowc.b.contrast");
        knob.mouseDown (eventAt (knob, 70.0f));
        knob.mouseDrag (eventAt (knob, 60.0f));
        // Panel refreshes reapply these bindings after unrelated processor notifications.
        knob.setParamId ("lowc.a.contrast");
        knob.setLinkedParamId ("lowc.b.contrast");
        p.flushPendingChanges (true);
        const bool stillDragging = p.getHistory().size() == 1 && a.ends == 0 && b.ends == 0;
        knob.mouseDrag (eventAt (knob, 40.0f));
        knob.mouseUp (eventAt (knob, 40.0f));
        p.flushPendingChanges (true);
        const float edited = p.getParameterValue ("lowc.a.contrast");
        check (stillDragging && a.begins == 1 && a.ends == 1 && b.begins == 1 && b.ends == 1
                   && p.getHistory().size() == 2 && edited > 0.0f
                   && p.getParameterValue ("lowc.b.contrast") == edited,
               "UI: refreshing a linked knob during a drag keeps one host gesture and one undo step");
        p.undo();
        check (p.getParameterValue ("lowc.a.contrast") == 0.0f
                   && p.getParameterValue ("lowc.b.contrast") == 0.0f,
               "UI: one undo restores both sets after a refreshed linked drag");

        knob.mouseDown (eventAt (knob, 70.0f));
        knob.mouseDrag (eventAt (knob, 60.0f));
        knob.setLinkedParamId ({});
        check (a.begins == a.ends && b.begins == b.ends,
               "UI: removing Link during a drag closes both original host gestures");
        knob.mouseUp (eventAt (knob, 60.0f));

        knob.mouseDown (eventAt (knob, 70.0f));
        knob.mouseDrag (eventAt (knob, 60.0f));
        knob.setParamId ("lowc.b.contrast");
        knob.setLinkedParamId ({});
        const float sideBefore = p.getParameterValue ("lowc.b.contrast");
        knob.mouseDrag (eventAt (knob, 30.0f));
        knob.mouseUp (eventAt (knob, 30.0f));
        check (p.getParameterValue ("lowc.b.contrast") == sideBefore
                   && a.begins == a.ends && b.begins == b.ends,
               "UI: rebinding a knob cancels the previous drag before it can edit the new channel set");

        knob.setParamId ("lowc.a.contrast");
        knob.setLinkedParamId ("lowc.b.contrast");
        knob.mouseDown (eventAt (knob, 70.0f));
        knob.mouseDrag (eventAt (knob, 50.0f));
    }
    check (a.begins == a.ends && b.begins == b.ends,
           "UI: destroying a linked knob mid drag closes both host gestures");
    pa->removeListener (&a);
    pb->removeListener (&b);
}

//==============================================================================
// (phase 3) The Assistant: an allocation-free capture, the analysis on the worker, the default card applied as one undo
// step, macro drags as one step, undo restoring macro positions, loudness matching, the state child round trip, and
// an old state (without the child) resetting it.
bool waitForAssistant (Host& h, double seconds)
{
    auto& a = h.proc.getAssistant();
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - t0 < seconds * 1000.0)
    {
        h.proc.flushPendingChanges (false);
        if (a.getPhase() == psplug::Assistant::Phase::Ready && ! a.isWorking())
            return true;
        if (a.getPhase() == psplug::Assistant::Phase::Failed)
            return false;
        juce::Thread::sleep (20);
    }
    return false;
}

void testAssistant()
{
    std::cout << "\n-- Assistant\n";
    using psplug::Assistant;
    using ps::assist::Macro;

    Host h;
    h.prepare();
    auto& a = h.proc.getAssistant();
    auto settings = a.getSettings();
    settings.analysisSeconds = 5.0f;
    a.setSettings (settings);

    // Silence does not count: the capture waits for playing audio.
    a.startListening();
    h.stream ((long long) kRate, {});
    check (a.getPhase() == Assistant::Phase::Listening && a.getProgress() == 0.0f,
           "assistant: silence is not counted while listening");

    const auto before = psdev::getAllocationCounts();
    h.stream ((long long) (5.5 * kRate), musicLikeInput (0.25f), { 512, 100, 1, 1024, 63 });
    const auto after = psdev::getAllocationCounts();
    check (after.allocations == before.allocations && after.frees == before.frees,
           "realtime: the Assistant's capture while listening is allocation free");

    const int stepsBefore = (int) h.proc.getHistory().size();
    const bool ready = waitForAssistant (h, 240.0);
    check (ready && a.getAppliedIndex() == 0 && a.getSuggestions().size() >= 5,
           "assistant: listen, analyse, the default card applied",
           str ((int) a.getPhase(), " / ", a.getSuggestions().size(), " cards / ", a.getStatusText()));
    if (! ready)
        return;

    const auto history = h.proc.getHistory();
    check ((int) history.size() == stepsBefore + 1 && history.back().description.startsWith ("Assistant: Suggested"),
           "assistant: the applied card is one undo step", history.back().description.toStdString());

    const std::vector<ps::ModuleType> expectedOrder { ps::ModuleType::Equalizer1, ps::ModuleType::Punch, ps::ModuleType::Imager,
                                                      ps::ModuleType::Definition, ps::ModuleType::ToneBalance,
                                                      ps::ModuleType::DynamicEq, ps::ModuleType::Maximizer };
    check (h.proc.getChainOrder() == expectedOrder, "assistant: the default card builds the reference-like chain");

    const auto card = a.getSuggestions().front();
    bool same = true;
    for (size_t i = 0; i < card.values.size(); ++i)
        if (psplug::isInPresetScope ((int) i, -1)
            && ! psplug::sameValue ((int) i, h.proc.getParameterValue (ps::paramSpecs()[i].id), card.values[i]))
            same = false;
    check (same, "assistant: every value of the applied card reached the parameters");
    check (card.loudnessMeasured && card.tablesReady, "assistant: the default card's gain and loudness tables are measured");

    // A macro drag is one step; undo brings the macro position back (positions are read from the parameters).
    const float widthBase = a.getMacros()[(size_t) Macro::Width];
    const int steps = (int) h.proc.getHistory().size();
    a.setMacro (Macro::Width, 60.0f, false);
    a.setMacro (Macro::Width, 70.0f, false);
    h.proc.flushPendingChanges (true);
    const bool noStepYet = (int) h.proc.getHistory().size() == steps;
    a.setMacro (Macro::Width, 75.0f, true);
    h.proc.flushPendingChanges (true);
    check (noStepYet && (int) h.proc.getHistory().size() == steps + 1 && std::abs (h.get ("img.amount") - 75.0f) < 0.01f,
           "assistant: a macro drag is one undo step", h.proc.getHistory().back().description.toStdString());
    h.proc.undo();
    check (std::abs (a.getMacros()[(size_t) Macro::Width] - widthBase) < 0.01f, "assistant: undo restores the macro position");
    h.proc.redo();

    // Loudness matching: a tone-match move corrects the Maximizer gain (on) or leaves it alone (off).
    const float gain0 = h.get ("max.gain");
    a.setLoudnessMatched (false);
    a.setMacro (Macro::ToneMatch, 180.0f, true);
    const float gainOff = h.get ("max.gain");
    a.setMacro (Macro::ToneMatch, 100.0f, true);
    a.setLoudnessMatched (true);
    a.setMacro (Macro::ToneMatch, 180.0f, true);
    const float gainOn = h.get ("max.gain");
    a.setMacro (Macro::ToneMatch, 100.0f, true);
    check (std::abs (gainOff - gain0) < 0.01f && std::abs (gainOn - gain0) > 0.01f && std::abs (h.get ("max.gain") - gain0) < 0.05f,
           "assistant: loudness matching corrects the Maximizer gain only when on",
           str (gain0, " / off ", gainOff, " / on ", gainOn, " / back ", h.get ("max.gain")));

    // Compress inserts a glue Dynamics before Tone Balance and Loudness moves the target.
    a.setMacro (Macro::Compress, 50.0f, true);
    const auto order = h.proc.getChainOrder();
    const auto dyn = std::find (order.begin(), order.end(), ps::ModuleType::Dynamics);
    check (dyn != order.end() && dyn + 1 != order.end() && *(dyn + 1) == ps::ModuleType::ToneBalance
               && std::abs (a.getMacros()[(size_t) Macro::Compress] - 50.0f) < 0.5f,
           "assistant: Compress inserts a glue Dynamics before Tone Balance");
    const float target0 = h.get ("max.learnTarget");
    a.setMacro (Macro::Loudness, target0 + 1.0f, true);
    check (std::abs (h.get ("max.learnTarget") - target0 - 1.0f) < 0.01f && h.get ("max.gain") > gain0,
           "assistant: the Loudness macro raises the target and the gain");

    // Another card = one step; the state child round-trips.
    const int steps2 = (int) h.proc.getHistory().size();
    check (a.apply (1) && (int) h.proc.getHistory().size() == steps2 + 1 && a.getAppliedIndex() == 1,
           "assistant: applying another card is one undo step");

    juce::MemoryBlock state;
    h.proc.getStateInformation (state);
    Host h2;
    h2.prepare();
    h2.proc.setStateInformation (state.getData(), (int) state.getSize());
    auto& a2 = h2.proc.getAssistant();
    const auto cards1 = a.getSuggestions(), cards2 = a2.getSuggestions();
    bool cardsSame = cards2.size() == cards1.size() && a2.getAppliedIndex() == 1;
    for (size_t k = 0; cardsSame && k < cards1.size(); ++k)
    {
        const auto& x = cards1[k];
        const auto& y = cards2[k];
        cardsSame = x.name == y.name && x.order == y.order && x.tablesReady == y.tablesReady;
        for (size_t i = 0; cardsSame && i < x.values.size(); ++i)
            cardsSame = psplug::sameValue ((int) i, x.values[i], y.values[i]);
    }
    const auto m1 = a.getMacros(), m2 = a2.getMacros();
    bool macrosSame = true;
    for (int k = 0; k < ps::assist::kNumMacros; ++k)
        macrosSame = macrosSame && std::abs (m1[(size_t) k] - m2[(size_t) k]) < 0.05f;
    check (cardsSame && macrosSame && a2.getPhase() == Assistant::Phase::Ready,
           "assistant: the state child round-trips (cards, applied card, macro positions)");

    // An older state without the child resets the Assistant.
    auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
    if (xml != nullptr)
        if (auto* child = xml->getChildByName ("Assistant"))
            xml->removeChildElement (child, true);
    juce::MemoryBlock old;
    if (xml != nullptr)
        juce::AudioProcessor::copyXmlToBinary (*xml, old);
    h2.proc.setStateInformation (old.getData(), (int) old.getSize());
    check (a2.getSuggestions().empty() && a2.getPhase() == Assistant::Phase::Idle && a2.getAppliedIndex() == -1,
           "assistant: a state without the Assistant child resets it");

    // A host restoring from another thread: queued, saves meanwhile return the restored tree, the next tick applies it.
    std::thread loader ([&] { h2.proc.setStateInformation (state.getData(), (int) state.getSize()); });
    loader.join();
    juce::MemoryBlock during;
    h2.proc.getStateInformation (during);
    const auto duringXml = juce::AudioProcessor::getXmlFromBinary (during.getData(), (int) during.getSize());
    const auto* duringChild = duringXml != nullptr ? duringXml->getChildByName ("Assistant") : nullptr;
    const bool queued = a2.getSuggestions().empty() && duringChild != nullptr && duringChild->getNumChildElements() > 0;
    h2.proc.flushPendingChanges (false);
    check (queued && a2.getSuggestions().size() == cards1.size(),
           "assistant: a restore from another thread is applied at the next tick (saves meanwhile keep it)");

    // Malformed numbers (NaN / inf) fall back to defaults; a listen still works afterwards.
    juce::ValueTree bad ("Assistant");
    bad.setProperty ("version", 1, nullptr);
    bad.setProperty ("seconds", "nan", nullptr);
    bad.setProperty ("loudness", "inf", nullptr);
    bad.setProperty ("rmsDb", "nan", nullptr);
    a2.restoreStateTree (bad);
    const auto badSettings = a2.getSettings();
    check (std::isfinite (badSettings.analysisSeconds) && badSettings.analysisSeconds >= Assistant::kMinSeconds
               && std::isfinite (badSettings.engine.loudnessLufs),
           "assistant: non-finite numbers in a state fall back to defaults");

    // Cancel and listen again at once (the capture buffer is replaced while the audio thread runs).
    for (int i = 0; i < 20; ++i)
    {
        a2.startListening();
        h2.stream (300, musicLikeInput (0.25f), { 64, 7, 128 });
        a2.cancel();
    }
    a2.startListening();
    h2.stream ((long long) kRate, musicLikeInput (0.25f));
    check (a2.getPhase() == Assistant::Phase::Listening && a2.getProgress() > 0.1f,
           "assistant: listening again right after a cancel captures audio");
    a2.cancel();
}

void testThreads()
{
    std::cout << "\n-- Threads\n";
    Host h;
    h.prepare();
    h.proc.setPresetRoot (tempDir.getChildFile ("Presets"));
    const auto state = stateOf (h.proc);

    std::atomic<bool> stop { false }, finite { true };
    std::atomic<int> blocks { 0 };
    std::thread audio ([&]
    {
        juce::AudioBuffer<float> buffer (2, 512);
        Lcg lcg { 5 };
        juce::MidiBuffer midi;
        while (! stop.load())
        {
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 512; ++i)
                    buffer.setSample (c, i, 0.3f * lcg.next());
            psdev::setAllocationTracking (true);
            h.proc.processBlock (buffer, midi);
            psdev::setAllocationTracking (false);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 512; ++i)
                    if (! std::isfinite (buffer.getSample (c, i)))
                        finite.store (false);
            blocks.fetch_add (1);
        }
    });

    const auto global = h.proc.getPresets (-1);
    int ops = 0;
    const double end = juce::Time::getMillisecondCounterHiRes() + 2000.0;
    while (juce::Time::getMillisecondCounterHiRes() < end)
    {
        switch (ops % 8)
        {
            case 0: h.proc.loadPreset (-1, global[(size_t) ops % global.size()]); break;
            case 1: h.proc.addModule (ps::ModuleType::Equalizer2); break;
            case 2: h.proc.removeModule ((ps::ModuleType) (ops % ps::kNumModuleTypes)); break;
            case 3: h.proc.undo(); break;
            case 4: h.proc.setParameterValue ("max.mode", (float) (ops % 6)); break;
            case 5: h.proc.setSoloModule (ops % 7 - 1); break;
            case 6: h.proc.setStateInformation (state.getData(), (int) state.getSize()); break;
            default: h.proc.redo(); h.proc.flushPendingChanges (true); break;
        }
        ++ops;
        if (ops % 4 == 0)
            juce::Thread::sleep (1);
    }
    stop.store (true);
    audio.join();

    check (finite.load() && blocks.load() > 50,
           str ("threads: audio kept running with finite output (", blocks.load(), " blocks, ", ops, " edits)"));
}
} // namespace

//==============================================================================
//==============================================================================
// Track Referencing (phase 3): the reference list, playback, level matching, loops, state and the Assistant link.
struct RefFile
{
    juce::File file;
    std::vector<float> l, r;
};

long long floorMod (long long value, long long modulus)
{
    const auto r = value % modulus;
    return r < 0 ? r + modulus : r;
}

RefFile writeReference (const juce::String& name, double rate, double seconds, const InputFn& fn)
{
    RefFile rf;
    rf.file = tempDir.getChildFile (name + ".wav");
    const auto n = (long long) std::llround (seconds * rate);
    for (long long i = 0; i < n; ++i)
    {
        float l = 0.0f, r = 0.0f;
        fn (i, l, r);
        rf.l.push_back (l);
        rf.r.push_back (r);
    }

    rf.file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> out (rf.file.createOutputStream().release());
    const auto options = juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (32)
                             .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    if (auto w = wav.createWriterFor (out, options))
    {
        const float* ch[2] { rf.l.data(), rf.r.data() };
        w->writeFromFloatArrays (ch, 2, (int) rf.l.size());
    }
    return rf;
}

InputFn sineInput (double hz, float amp, double phase = 0.0)
{
    return [hz, amp, phase] (long long n, float& l, float& r)
    {
        const double t = juce::MathConstants<double>::twoPi * hz * (double) n / kRate + phase;
        l = amp * (float) std::sin (t);
        r = amp * (float) std::sin (t + 0.3);
    };
}

// Pumps the processor's housekeeping (Track Referencing's tick) until the loader is idle and nothing is loading.
bool waitForReferences (Host& h, double seconds)
{
    auto& refs = h.proc.getTrackReferencing();
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - t0 < seconds * 1000.0)
    {
        h.proc.flushPendingChanges (false);
        if (refs.waitUntilIdle (0))
        {
            h.proc.flushPendingChanges (false);
            bool loading = false;
            for (const auto& e : refs.getInfo().entries)
                loading = loading || e.state == psplug::TrackReferencing::State::Loading;
            if (! loading)
                return true;
        }
        juce::Thread::sleep (10);
    }
    return false;
}

const psplug::TrackReferencing::Entry* entryOf (const psplug::TrackReferencing::Info& info, int id)
{
    for (const auto& e : info.entries)
        if (e.id == id)
            return &e;
    return nullptr;
}

// Largest |out[n] - ref[pos(n)]| over [from, to) of the output.
float refErrorDb (const Output& out, const RefFile& ref, size_t from, size_t to,
                  const std::function<long long (size_t)>& pos)
{
    float m = 0.0f;
    bool reported = false;
    for (size_t n = from; n < to && n < out.l.size(); ++n)
    {
        const auto p = pos (n);
        if (p == -2)
            continue;   // excluded (a crossfade)
        if (p < 0 || p >= (long long) ref.l.size())
            return 0.0f;
        const float e = juce::jmax (std::abs (out.l[n] - ref.l[(size_t) p]), std::abs (out.r[n] - ref.r[(size_t) p]));
        if (e > 1.0e-5f && ! reported)
        {
            reported = true;
            // where the output is: search the reference for out[n] around p
            long long found = -1;
            for (long long q = 0; q < (long long) ref.l.size() && found < 0; ++q)
                if (std::abs (ref.l[(size_t) q] - out.l[n]) < 1.0e-7f && std::abs (ref.r[(size_t) q] - out.r[n]) < 1.0e-7f)
                    found = q;
            std::cout << "    (debug) first mismatch at n=" << n << " want pos " << p << " out " << out.l[n]
                      << " ref " << ref.l[(size_t) p] << " found at " << found << ")\n";
        }
        m = juce::jmax (m, e);
    }
    return dbOf (m);
}

// Whole-track playback position at output sample n (host position t0 at n = 0). Around the end of the file the
// reference fades out and its start fades in (8 ms each): those samples are excluded (-2).
std::function<long long (size_t)> cyclic (long long t0, long long len)
{
    return [t0, len] (size_t n)
    {
        const auto k = floorMod ((long long) n + t0, len);
        return k < 400 || k >= len - 400 ? -2LL : k;
    };
}

float maxStepOf (const Output& o, size_t from = 0)
{
    float m = 0.0f;
    for (size_t n = juce::jmax<size_t> (from, 1); n < o.l.size(); ++n)
    {
        m = juce::jmax (m, std::abs (o.l[n] - o.l[n - 1]));
        m = juce::jmax (m, std::abs (o.r[n] - o.r[n - 1]));
    }
    return m;
}

float loudnessOf (const Output& o, size_t from, size_t to)
{
    ps::LoudnessMeter m;
    m.prepare (kRate);
    to = juce::jmin (to, o.l.size());
    if (to > from)
        m.process (o.l.data() + from, o.r.data() + from, (int) (to - from));
    return m.integratedLufs();
}

void testTrackReferencing()
{
    std::cout << "\n-- Track Referencing\n";
    using TR = psplug::TrackReferencing;

    const auto refNoise = writeReference ("ref-noise", kRate, 6.0, noiseInput (0.5f, 11));
    const auto refSine = writeReference ("ref-sine", kRate, 6.0, sineInput (220.0, 0.5f));
    const auto refQuiet = writeReference ("ref-quiet-44k", 44100.0, 5.0, noiseInput (0.05f, 12));

    Host h;
    h.prepare();
    h.proc.setChainOrder ({});   // an empty chain: the master is the input, so "untouched" can be checked exactly
    auto& refs = h.proc.getTrackReferencing();
    const auto master = noiseInput (0.1f, 3);

    // ---- nothing loaded: listening is ignored, the master is untouched
    refs.setListening (true);
    {
        const auto in = recordInput (24000, master);
        const auto out = h.stream (24000, replay (in));
        check (nullDb (in, out, 0) <= -200.0f, "reference: with no reference, Listen leaves the master untouched");
    }
    refs.setListening (false);

    // ---- add, analyse once
    const int idNoise = refs.addReference (refNoise.file);
    const bool loaded = waitForReferences (h, 60.0);
    auto info = refs.getInfo();
    const auto* e = entryOf (info, idNoise);
    check (loaded && e != nullptr && e->state == TR::State::Ready && e->resident
               && std::abs (e->lengthSeconds - 6.0) < 1.0e-6 && ps::hasValue (e->integratedLufs)
               && info.selected == 0,
           "reference: a file loads, is analysed and selected",
           e != nullptr ? str ((int) e->state, " ", e->lengthSeconds, " s ", e->integratedLufs, " LUFS ", e->error.toStdString())
                        : std::string ("no entry"));
    check (refs.getDrawData (idNoise) != nullptr && refs.getDrawData (idNoise)->overview.minimum.size() == 2048,
           "reference: the overview for drawing is built");
    check (refs.addReference (refNoise.file) == idNoise && refs.getInfo().entries.size() == 1,
           "reference: adding the same file again selects it instead of a duplicate");
    check (refs.getAssistantTarget (idNoise).valid && refs.getAssistantTarget (idNoise).path
                                                          == refNoise.file.getFullPathName().toStdString(),
           "reference: the Assistant's target comes from the same analysis");

    // ---- on the master: bit-exact
    {
        const auto in = recordInput (48000, master);
        const auto out = h.stream (48000, replay (in));
        check (nullDb (in, out, 0) <= -200.0f, "reference: Listen on the master leaves it bit-exact (reference loaded)");
    }

    // ---- level match Off: the reference itself, synced to the host position
    auto settings = refs.getSettings();
    settings.levelMatch = ps::LevelMatch::Off;
    refs.setSettings (settings);
    h.stream (24000, master);   // the gain settles at 0 dB
    refs.setListening (true);
    {
        const long long t0 = h.head.time;
        const auto out = h.stream (48000, master);
        const auto len = (long long) refNoise.l.size();
        const float err = refErrorDb (out, refNoise, 4800, 48000,
                                      cyclic (t0, len));
        check (err < -120.0f, "reference: Listen plays the reference sample-exact at the host position (match Off)",
               str (err, " dB"));
        check (refs.isReferenceSounding() && refs.getListenPosition() == 1.0f, "reference: the UI sees it playing");
    }

    // ---- loops: B = 2..3 s plays that second, looped, from the host position
    refs.setLoop (idNoise, 1, 2.0, 3.0);
    info = refs.getInfo();
    check (entryOf (info, idNoise)->activeLoop == 1 && entryOf (info, idNoise)->loops[1].set,
           "reference: drawing loop B makes it the active loop");
    h.stream (9600, master);
    {
        const long long t0 = h.head.time;
        const auto out = h.stream (96000, master);
        // (the 8 ms wrap crossfade after each region start mixes in the continuation: excluded)
        const float err = refErrorDb (out, refNoise, 0, 96000, [t0] (size_t n)
                                      {
                                          const auto k = floorMod ((long long) n + t0, 48000);
                                          return k < 400 ? -2LL : 96000 + k;
                                      });
        check (err < -120.0f, "reference: loop B plays its region, looped, synced to the host", str (err, " dB"));
    }
    refs.setActiveLoop (idNoise, 3);   // D is empty: ignored
    check (refs.getInfo().entries[0].activeLoop == 1, "reference: an empty loop letter cannot be activated");
    refs.setLoop (idNoise, 0, 0.5, 0.6);   // shorter than 0.5 s: clamped to 0.5 s
    info = refs.getInfo();
    check (entryOf (info, idNoise)->loops[0].set && std::abs (entryOf (info, idNoise)->loops[0].end - 1.0) < 1.0e-9,
           "reference: a loop shorter than half a second is widened to 0.5 s");
    refs.setActiveLoop (idNoise, -1);
    h.stream (9600, master);
    {
        const long long t0 = h.head.time;
        const auto out = h.stream (24000, master);
        const auto len = (long long) refNoise.l.size();
        const float err = refErrorDb (out, refNoise, 0, 24000,
                                      cyclic (t0, len));
        check (err < -120.0f, "reference: back to the whole track", str (err, " dB"));
    }

    // ---- seek while playing
    refs.seek (1.0);
    h.stream (2400, master);
    {
        const auto out = h.stream (24000, master);
        const float err = refErrorDb (out, refNoise, 0, 24000,
                                      [] (size_t n) { return 48000 + 2400 + (long long) n; });
        check (err < -120.0f, "reference: seek plays from the clicked point", str (err, " dB"));
    }

    // ---- host stopped: play when stopped on / off; sync off ignores host jumps
    h.head.playing = false;
    settings.playWhenStopped = false;
    refs.setSettings (settings);
    h.stream (4800, master);
    {
        const auto out = h.stream (9600, master);
        float peak = 0.0f;
        for (size_t i = 0; i < out.l.size(); ++i)
            peak = juce::jmax (peak, std::abs (out.l[i]), std::abs (out.r[i]));
        check (peak == 0.0f, "reference: silent while the host is stopped (Play when stopped off)");
    }
    settings.playWhenStopped = true;
    settings.sync = false;
    refs.setSettings (settings);
    h.head.playing = true;
    h.stream (4800, master);
    {
        const double before = refs.getPlayheadSeconds();
        h.head.time += 123457;   // a host jump
        h.stream (4800, master);
        const double moved = refs.getPlayheadSeconds() - before;
        check (std::abs (moved - 0.1) < 1.0e-6, "reference: with Sync off the reference runs on its own",
               str (moved, " s"));
    }
    settings.sync = true;
    refs.setSettings (settings);

    // ---- switching references and loops crossfades (no click on a sine reference)
    const int idSine = refs.addReference (refSine.file);
    waitForReferences (h, 60.0);
    h.stream (9600, master);
    {
        const long long t0 = h.head.time;
        const auto out = h.stream (24000, master);
        const auto len = (long long) refSine.l.size();
        const float err = refErrorDb (out, refSine, 0, 24000,
                                      cyclic (t0, len));
        check (err < -120.0f, "reference: selecting another reference plays it", str (err, " dB"));
    }
    {
        refs.setLoop (idSine, 2, 1.003, 2.517);   // a loop whose wrap is not at a zero crossing
        auto out = h.stream (4800 * 30, InputFn(), { 512 }, [&] (int block)   // a silent master: only the reference moves
        {
            if (block == 40)
                refs.setActiveLoop (idSine, -1);
            if (block == 80)
                refs.setActiveLoop (idSine, 2);
            if (block == 120)
                refs.seek (4.4);
            if (block == 160)
                refs.setListening (false);
            if (block == 200)
                refs.setListening (true);
        });
        // A sine of 0.5 at 220 Hz moves at most 0.0145 per sample; a hard cut jumps up to 1.
        const float step = maxStepOf (out, 512);
        check (step < 0.06f, "reference: loop wraps, loop / whole-track switches, seeks and A/B fades have no clicks",
               str ("largest step ", step));
    }

    // ---- level match: Integrated and Short-term, within 0.2 LU of the master
    {
        h.head.playing = true;
        refs.select (idNoise);
        refs.setActiveLoop (idNoise, -1);
        refs.resetMixAnalysis();
        settings.levelMatch = ps::LevelMatch::Integrated;
        refs.setSettings (settings);
        refs.setListening (false);
        const auto masterIn = noiseInput (0.08f, 5);
        h.stream ((long long) (6.0 * kRate), masterIn);
        refs.runAnalysisNow();
        const float mixLufs = refs.getMixProfile().integratedLufs;
        const float refLufs = refs.getReferenceProfile().integratedLufs;
        const float want = mixLufs - refLufs;
        check (std::abs (refs.getMatchGainDb() - want) < 0.05f,
               "reference: Integrated level match turns the reference to the master's loudness",
               str ("gain ", refs.getMatchGainDb(), " want ", want, " (", mixLufs, " vs ", refLufs, ")"));
        refs.setListening (true);
        const auto out = h.stream ((long long) (6.0 * kRate), masterIn);
        const float heard = loudnessOf (out, 4800, out.l.size());
        check (std::abs (heard - mixLufs) < 0.2f, "reference: what you hear on the reference is as loud as the master",
               str (heard, " vs ", mixLufs, " LUFS"));

        settings.levelMatch = ps::LevelMatch::ShortTerm;
        refs.setSettings (settings);
        const auto out2 = h.stream ((long long) (8.0 * kRate), masterIn);
        refs.runAnalysisNow();
        const float heard2 = loudnessOf (out2, (size_t) (5.0 * kRate), out2.l.size());
        check (std::abs (heard2 - mixLufs) < 0.2f, "reference: Short-term level match within 0.2 LU",
               str (heard2, " vs ", mixLufs, " LUFS, gain ", refs.getMatchGainDb()));
        check (refs.getComparison().valid() && ! refs.getComparison().hints.empty(),
               "reference: the master-vs-reference comparison is published");
        settings.levelMatch = ps::LevelMatch::Off;
        refs.setSettings (settings);
    }

    // ---- mono / swap of the I/O panel apply to the reference
    {
        h.set ("io.swap", 1.0f);
        h.stream (24000, master);   // the level-match gain glides back to 0 dB (250 ms)
        const long long t0 = h.head.time;
        auto out = h.stream (24000, master);
        std::swap (out.l, out.r);
        const auto len = (long long) refNoise.l.size();
        const float err = refErrorDb (out, refNoise, 0, 24000,
                                      cyclic (t0, len));
        check (err < -120.0f, "reference: Swap applies to the reference", str (err, " dB"));
        h.set ("io.swap", 0.0f);
        h.set ("io.mono", 1.0f);
        const auto mono = h.stream (24000, master);
        float diff = 0.0f;
        for (size_t i = 2400; i < mono.l.size(); ++i)
            diff = juce::jmax (diff, std::abs (mono.l[i] - mono.r[i]));
        check (diff < 1.0e-6f, "reference: Mono applies to the reference");
        h.set ("io.mono", 0.0f);
    }

    // ---- offline render: always the master
    {
        h.proc.setNonRealtime (true);
        const auto in = recordInput (24000, master);
        const auto out = h.stream (24000, replay (in));
        check (nullDb (in, out, 0, 4800) <= -200.0f, "reference: an offline render always renders the master",
               str (nullDb (in, out, 0, 4800), " dB"));   // (the first 100 ms: the master's own Mono fade-out)
        h.proc.setNonRealtime (false);
    }

    // ---- realtime: loads, switches and loop edits while audio runs
    {
        const auto before = psdev::getAllocationCounts();
        int idQuiet = 0;
        h.stream ((long long) (4.0 * kRate), master, { 512, 64, 1024, 1 }, [&] (int block)
        {
            if (block == 5)
                idQuiet = refs.addReference (refQuiet.file);
            if (block % 37 == 0)
                refs.select (block % 74 == 0 ? idNoise : idSine);
            if (block % 23 == 0)
                refs.setLoop (idSine, block % 5, 0.1 * (block % 17), 0.1 * (block % 17) + 0.7);
            if (block % 29 == 0)
                refs.setListening (block % 58 != 0);
        });
        const auto after = psdev::getAllocationCounts();
        check (after.allocations == before.allocations && after.frees == before.frees,
               "realtime: Track Referencing playback, switching and loop edits are allocation free");
        waitForReferences (h, 60.0);
        info = refs.getInfo();
        const auto* q = entryOf (info, idQuiet);
        check (q != nullptr && q->state == TR::State::Ready && std::abs (q->lengthSeconds - 5.0) < 1.0e-3
                   && q->fileSampleRate == 44100.0,
               "reference: a 44.1 kHz file is resampled to the 48 kHz session",
               q != nullptr ? str (q->lengthSeconds, " s at ", q->fileSampleRate) : std::string ("missing"));
        check (refs.getDropoutCount() == 0, "reference: no slot dropouts on the audio thread",
               str (refs.getDropoutCount()));
    }

    // ---- ten references at most; eviction keeps the selected one
    {
        std::vector<int> ids;
        for (int i = 0; i < 7; ++i)
            ids.push_back (refs.addReference (writeReference ("ref-extra-" + juce::String (i), kRate, 2.0,
                                                              noiseInput (0.2f, 100u + (juce::uint32) i)).file,
                                              false));
        const auto extra = writeReference ("ref-eleven", kRate, 1.0, noiseInput (0.2f, 99));
        check (refs.addReference (extra.file) == 0 && refs.getInfo().entries.size() == TR::kMaxReferences,
               "reference: the list holds ten references at most");
        waitForReferences (h, 120.0);

        refs.select (idSine);
        const long long oneTrack = (long long) refSine.l.size() * 2 * (long long) sizeof (float);
        refs.setMemoryBudget (oneTrack + oneTrack / 2);
        info = refs.getInfo();
        int resident = 0;
        for (const auto& en : info.entries)
            resident += en.resident ? 1 : 0;
        check (entryOf (info, idSine)->resident && refs.getResidentBytes() <= refs.getMemoryBudget(),
               "reference: over the memory budget, the others are evicted, the selected one stays",
               str (resident, " resident, ", refs.getResidentBytes(), " bytes"));
        refs.select (idNoise);   // evicted: decoded again
        refs.setListening (true);
        const bool back = waitForReferences (h, 60.0);
        info = refs.getInfo();
        check (back && entryOf (info, idNoise)->resident && entryOf (info, idNoise)->state == TR::State::Ready,
               "reference: selecting an evicted reference decodes it again");
        h.stream (9600, master);
        const long long t0 = h.head.time;
        const auto out = h.stream (24000, master);
        const auto len = (long long) refNoise.l.size();
        const float err = refErrorDb (out, refNoise, 0, 24000,
                                      cyclic (t0, len));
        check (err < -120.0f, "reference: and it plays again", str (err, " dB"));
        refs.setMemoryBudget (TR::kDefaultMemoryBudget);
    }

    // ---- state round trip
    {
        refs.select (idSine);
        refs.setActiveLoop (idSine, 2);
        settings.levelMatch = ps::LevelMatch::ShortTerm;
        settings.playWhenStopped = false;
        refs.setSettings (settings);
        refs.setListening (true);

        juce::MemoryBlock saved;
        h.proc.getStateInformation (saved);
        const auto tree = refs.createStateTree();
        check (tree.getNumChildren() == TR::kMaxReferences && tree.getChild (1).getProperty ("loopC").toString().isNotEmpty(),
               "reference: the state lists the references and their loops");

        Host h2;
        h2.prepare();
        h2.proc.setStateInformation (saved.getData(), (int) saved.getSize());
        auto& refs2 = h2.proc.getTrackReferencing();
        waitForReferences (h2, 120.0);
        const auto i1 = refs.getInfo(), i2 = refs2.getInfo();
        bool same = i1.entries.size() == i2.entries.size() && i1.selected == i2.selected;
        for (size_t k = 0; same && k < i1.entries.size(); ++k)
        {
            same = i1.entries[k].file == i2.entries[k].file && i1.entries[k].activeLoop == i2.entries[k].activeLoop;
            for (int lp = 0; same && lp < TR::kNumLoops; ++lp)
                same = std::abs (i1.entries[k].loops[(size_t) lp].start - i2.entries[k].loops[(size_t) lp].start) < 1.0e-5
                       && std::abs (i1.entries[k].loops[(size_t) lp].end - i2.entries[k].loops[(size_t) lp].end) < 1.0e-5
                       && i1.entries[k].loops[(size_t) lp].set == i2.entries[k].loops[(size_t) lp].set;
        }
        const auto s2 = refs2.getSettings();
        check (same && s2.levelMatch == ps::LevelMatch::ShortTerm && ! s2.playWhenStopped && ! refs2.isListening(),
               "reference: a restored project has the same list, loops, selection and settings; Listen is on the master");

        // a missing file stays in the list until relinked (loops kept)
        auto t = refs.createStateTree();
        t.getChild (1).setProperty ("path", tempDir.getChildFile ("moved-away.wav").getFullPathName(), nullptr);
        refs2.restoreStateTree (t);
        waitForReferences (h2, 60.0);
        auto i3 = refs2.getInfo();
        const auto& missing = i3.entries[1];
        check (missing.state == TR::State::Missing && missing.loops[2].set,
               "reference: a missing file is kept in the list (state Missing, loops kept)");
        refs2.relink (missing.id, refSine.file);
        waitForReferences (h2, 60.0);
        i3 = refs2.getInfo();
        check (i3.entries[1].state == TR::State::Ready && i3.entries[1].loops[2].set && i3.entries[1].activeLoop == 2,
               "reference: relinking loads the file and keeps its loops");

        // a restore from a host thread is applied at the next tick; saves meanwhile return it
        juce::ValueTree empty ("TrackReferencing");
        std::thread host ([&] { refs2.restoreStateTree (empty); });
        host.join();
        check (refs2.createStateTree().getNumChildren() == 0, "reference: a host-thread restore is what the next save returns");
        h2.proc.flushPendingChanges (false);
        check (refs2.getInfo().entries.empty(), "reference: and it is applied at the next tick");
    }

    // ---- the Assistant uses the same list and analysis
    {
        auto& a = h.proc.getAssistant();
        const auto extra = writeReference ("ref-assistant", kRate, 4.0, noiseInput (0.3f, 77));
        a.awaitReference (extra.file.getFullPathName(), "ref-assistant");
        refs.removeReference (refs.getInfo().entries.back().id);   // make room
        const int id = refs.addReference (extra.file, false);
        check (a.isLoadingReference(), "assistant reference: waits for Track Referencing's analysis");
        waitForReferences (h, 60.0);
        const auto target = refs.getAssistantTarget (id);
        check (! a.isLoadingReference() && a.getReference().valid
                   && a.getReference().path == extra.file.getFullPathName().toStdString()
                   && a.getReference().integratedLufs == target.integratedLufs,
               "assistant reference: the target is Track Referencing's analysis of the file");
        refs.removeReference (id);
        check (! a.getReference().valid, "assistant reference: removing the reference returns the Assistant to Auto");
    }

    // ---- removing everything returns to the master, bit-exact
    {
        for (const auto& en : refs.getInfo().entries)
            refs.removeReference (en.id);
        check (refs.getInfo().entries.empty() && ! refs.isListening(), "reference: removing every reference ends Listen");
        h.stream (4800, master);
        const auto in = recordInput (24000, master);
        const auto out = h.stream (24000, replay (in));
        check (nullDb (in, out, 0) <= -200.0f, "reference: and the master is bit-exact again");
    }

    // ---- a sample-rate change decodes the selected reference again at the new rate
    {
        Host h3 (44100.0);
        h3.prepare();
        auto& r3 = h3.proc.getTrackReferencing();
        const int id = r3.addReference (refNoise.file);
        waitForReferences (h3, 60.0);
        h3.sampleRate = 96000.0;
        h3.head.sampleRate = 96000.0;
        h3.prepare();
        const bool ok = waitForReferences (h3, 60.0);
        const auto i = r3.getInfo();
        check (ok && entryOf (i, id)->resident && std::abs (entryOf (i, id)->lengthSeconds - 6.0) < 1.0e-3,
               "reference: after a sample-rate change the reference is decoded again at the new rate");
    }

    // ---- races the review found: overlapping host restores, a rate change during the first load, relink during a
    // load, a seek while an evicted reference decodes again, relinking the Assistant's reference
    {
        Host h4;
        h4.prepare();
        auto& r4 = h4.proc.getTrackReferencing();
        auto& a4 = h4.proc.getAssistant();
        const auto longRef = writeReference ("ref-long", kRate, 60.0, musicLikeInput (0.3f, 21));
        const auto otherRef = writeReference ("ref-other", kRate, 8.0, noiseInput (0.2f, 22));

        // overlapping host-thread restores while the message thread ticks: the newest one wins, nothing is lost
        r4.addReference (refNoise.file);
        r4.addReference (refSine.file);
        waitForReferences (h4, 60.0);
        const auto two = r4.createStateTree();
        std::atomic<bool> done { false };
        std::thread host ([&]
        {
            for (int i = 0; i < 300; ++i)
                r4.restoreStateTree (two);
            done.store (true);
        });
        while (! done.load())
            h4.proc.flushPendingChanges (false);
        host.join();
        h4.proc.flushPendingChanges (false);
        h4.proc.flushPendingChanges (false);
        check (r4.getInfo().entries.size() == 2 && r4.createStateTree().getNumChildren() == 2,
               "reference: overlapping host-thread restores keep the newest state",
               str (r4.getInfo().entries.size(), " entries"));

        // relink while the first load runs: the new file loads (the running job is stale, not merged)
        const int idLong = r4.addReference (longRef.file, false);
        r4.relink (idLong, otherRef.file);
        waitForReferences (h4, 60.0);
        auto i4 = r4.getInfo();
        const auto* relinked = entryOf (i4, idLong);
        check (relinked != nullptr && relinked->state == TR::State::Ready && relinked->file == otherRef.file
                   && std::abs (relinked->lengthSeconds - 8.0) < 1.0e-3,
               "reference: relinking during a load loads the new file");

        // the Assistant waits for a reference whose first load meets a sample-rate change
        r4.removeReference (idLong);
        a4.awaitReference (longRef.file.getFullPathName(), "ref-long");
        const int idWait = r4.addReference (longRef.file, false);
        h4.sampleRate = 44100.0;
        h4.head.sampleRate = 44100.0;
        h4.prepare();
        waitForReferences (h4, 60.0);
        check (! a4.isLoadingReference() && a4.getReference().valid
                   && a4.getReference().path == longRef.file.getFullPathName().toStdString(),
               "assistant reference: a sample-rate change during the first load still delivers the target");

        // relinking the Assistant's reference keeps the Assistant on it; removing it then returns to Auto
        r4.relink (idWait, otherRef.file);
        check (a4.getReference().path == otherRef.file.getFullPathName().toStdString(),
               "assistant reference: a relinked reference stays the Assistant's target");
        waitForReferences (h4, 60.0);
        r4.removeReference (idWait);
        check (! a4.getReference().valid, "assistant reference: and removing it returns the Assistant to Auto");

        // a seek made while an evicted reference decodes again is applied once it is back
        const int idA = r4.getInfo().entries[0].id, idB = r4.getInfo().entries[1].id;
        auto s4 = r4.getSettings();
        s4.levelMatch = ps::LevelMatch::Off;
        r4.setSettings (s4);
        r4.select (idB);
        r4.setMemoryBudget (1);   // only the selected reference stays decoded
        r4.select (idA);          // evicted: decodes again
        r4.seek (2.0);
        r4.setListening (true);
        waitForReferences (h4, 60.0);
        r4.setMemoryBudget (TR::kDefaultMemoryBudget);
        h4.stream (2400, InputFn());
        h4.stream (24000, InputFn());
        const double rate = h4.sampleRate;
        // The reference was written at 48 kHz and plays resampled at 44.1 kHz here, so compare the playhead instead.
        const double head = r4.getPlayheadSeconds();
        const double want = 2.0 + (2400.0 + 24000.0) / rate;
        check (std::abs (head - want) < 0.002, "reference: a seek during the re-decode is applied when it is back",
               str (head, " s, want ", want));
    }
}

int runSelfTest()
{
    tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("PureSheenSelfTest-" + juce::Uuid().toDashedString());
    tempDir.createDirectory();
    std::cout << "selftest: temporary files in " << tempDir.getFullPathName() << "\n";
    std::cout << "selftest: checks marked [dsp] measure the DSP modules / chain\n";

    const double start = juce::Time::getMillisecondCounterHiRes();

    testAllocationCounter();
    testParameters();
    testNullAndBypass();
    testLatency();
    testRealtime();
    testPhase2();
    testState();
    testUndo();
    testMaximizerLearn();
    testMaximizerCeiling();
    testPresets();
    testMonitoring();
    testTsPanelBindings();
    testPhase2PanelRouting();
    testPanelGestures();
    testBassScopeFeed();
    testPromptLifetime();
    testThreads();
    testAssistant();
    testTrackReferencing();

    std::cout << "\n-- Match EQ state, undo and file loader\n";
    const int matchFailures = psplug::runMatchEqStateTests();
    check (matchFailures == 0, "match eq: state, undo and file loader checks", str (matchFailures, " failed"));

    std::cout << "\n-- Summary checks\n";
    std::string unknown;
    for (const auto& id : unknownIds)
        unknown += id + " ";
    check (unknownIds.empty(), "selftest: every parameter id used by the tests exists", unknown);

    const auto counts = psdev::getAllocationCounts();
    check (counts.allocations == 0 && counts.frees == 0,
           "realtime: no allocation / free (operator new or CRT heap) inside processBlock in any test",
           str (counts.allocations, " allocation calls, ", counts.frees, " frees"));

    tempDir.deleteRecursively();

    std::cout << "\nselftest: " << numPassed << " passed, " << numFailed << " failed ("
              << juce::String ((juce::Time::getMillisecondCounterHiRes() - start) / 1000.0, 1) << " s)\n";
    return numFailed == 0 ? 0 : 1;
}
