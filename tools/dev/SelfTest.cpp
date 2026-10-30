#include "AllocationTracking.h"
#include "core/Loudness.h"
#include "dsp/Maximizer.h"
#include "plugin/FactoryPresets.h"
#include "plugin/PluginProcessor.h"
#include "plugin/PluginState.h"

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
// Checks marked [dsp] in their name depend on the DSP modules / chain doing real work (they pass trivially or fail
// while those are skeleton stubs); every other check tests the processor itself.

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
        sameOrder = p != nullptr && p->getParameterID() == juce::String (specs[(size_t) i].id)
                    && p->getVersionHint() == 1 && p->getName (256) == juce::String (specs[(size_t) i].name);
        if (! sameOrder)
            first = specs[(size_t) i].id;
    }
    check (sameOrder, str ("params: one host parameter per paramSpecs() entry, same order / id / name, version 1 (",
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
int impulseDelay (Host& h, int length = 32768)
{
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
std::vector<float> currentValues (const PureSheenProcessor& p)
{
    std::vector<float> v;
    for (const auto& s : ps::paramSpecs())
        v.push_back (p.getParameterValue (s.id));
    return v;
}

int countDifferences (const std::vector<float>& a, const std::vector<float>& b, std::string* first = nullptr)
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
                                              ps::ModuleType::Imager, ps::ModuleType::Dynamics };
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
        check (perModule, "presets: 2-4 factory presets per module (EQ 1 / EQ 2 share theirs)", counts);

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
}

//==============================================================================
// Message-thread work (chain edits, presets, undo, state loads) racing the audio thread.
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
int runSelfTest()
{
    tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("PureSheenSelfTest-" + juce::Uuid().toDashedString());
    tempDir.createDirectory();
    std::cout << "selftest: temporary files in " << tempDir.getFullPathName() << "\n";
    std::cout << "selftest: checks marked [dsp] need the real DSP modules / chain\n";

    const double start = juce::Time::getMillisecondCounterHiRes();

    testAllocationCounter();
    testParameters();
    testNullAndBypass();
    testLatency();
    testRealtime();
    testState();
    testUndo();
    testMaximizerLearn();
    testPresets();
    testMonitoring();
    testThreads();

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
