#pragma once

#include "dsp/Params.h"
#include "dsp/Fifo.h"

// Base class of every chain module. The Chain (dsp/Chain.h) owns one instance per ModuleType (plus a "twin" for every
// type that offers Transient/Sustain, see below) and wraps each in a slot that applies, around Module::process:
//   input -> [M/S encode when channelMode == MidSide] -> module.process -> [M/S decode]
//         -> channelOn[c] == false: channel c replaced by the (latency-aligned) dry signal of that channel
//         -> power off: dry signal, latency kept (the slot delays it by the module's latency)
//         -> delta: processed - latency-aligned dry (unless handlesDelta())
//         -> difference meter (RMS out - RMS in, dB)
// So a module only ever sees two channels already in the domain its channel mode asks for, and only implements DSP.
//
// Transient/Sustain (phase 2): the slot splits its input into a transient part T and a sustain part S (both stereo,
// ps::TransientSplitter). The normal instance processes T with channelMode Stereo and paramSet 0; the twin (a second,
// fully independent instance of the same class, setParamSet (1) called once before prepare) processes S with
// channelMode Stereo and paramSet 1. The slot sums the parts (channel on / solo per part, as in M/S). Rules for a
// module that offers T/S:
//   1. Stereo with paramSet 1 (getParamSet() == 1): read parameter set B for both channels (curves, meters, learn).
//      Simplest implementation: in setParams, after read*Params, copy set B over set A when getParamSet() == 1.
//   2. Latency depends only on shared parameters (never on per-set ones), so both instances report the same latency.
//   3. No static / shared mutable state between instances (each owns its buffers, worker threads, meters).
//   4. Both instances receive the same setParams (values) array every block while the slot runs in T/S.
//
// Threading:
//   prepare()                         message thread (may allocate; the chain is not processing during prepare)
//   setParams / setMonitor / process  audio thread: allocation-free, lock-free, no logging
//   getLatencySamples()               any thread (atomic or plain int read)
//   meters                            written by the audio thread (AtomicFloat), read by the UI
namespace ps
{
struct ProcessContext
{
    ChannelMode channelMode = ChannelMode::Stereo;  // Stereo: ch0/ch1 use set A, linked. MidSide/LeftRight: ch0 = A,
                                                    // ch1 = B, unlinked
    bool delta = false;                             // only passed to modules whose handlesDelta() is true
    // handlesDelta() modules only: when non-null, the module writes its own Delta signal here (2 channels, same
    // latency and channel domain as its output) and keeps its NORMAL output in `channels`. The chain may pass it
    // whether or not delta is audible (it keeps the delta bus running so enabling Delta needs no warm-up and its
    // crossfades have real signal on both sides). Null = no delta wanted.
    float* const* deltaOut = nullptr;
    double bpm = 120.0;                             // host tempo (Punch sync etc.; 120 when unknown)
    bool isPlaying = false;
    // (phase 2) channelMode == Stereo: the parameter set both channels use (0 = A, 1 = B). The chain's T/S path runs
    // the twin instance with paramSet 1 on the sustain part. Always equal to the instance's getParamSet().
    int paramSet = 0;
};

// Monitoring requests from the UI (not parameters, not saved). One per module, set by the chain from the UI.
struct ModuleMonitor
{
    int soloBand = -1;              // multiband modules: only this band is heard (-1 = off)
    int soloChannelSet = -1;        // T/S band / region solo: -1 = both parts, 0/1 = only that part (other modes ignore it)
    bool auditionOn = false;        // EQ "Alt-solo": band-pass audition around auditionHz
    float auditionHz = 1000.0f;
    float auditionQ = 4.0f;         // 0.2..12
    int auditionChannelSet = 0;
    bool soloDetectionFilter = false;   // Dynamics: hear the detector signal
    // (phase 2)
    bool soloRegion = false;        // Smoother / Low Contrast / Definition: hear only the input inside the action
                                    // region (before processing, same latency)
    bool learnRegion = false;       // Smoother "Learn": accumulate where the harshness is (no effect on the audio)
};

class Module
{
public:
    virtual ~Module() = default;

    virtual ModuleType getType() const noexcept = 0;

    virtual void prepare (double sampleRate, int maxBlockSize) = 0;
    virtual void reset() noexcept = 0;

    // Reads this module's parameters from the flat value array (calls its read*Params). Once per block, before
    // process. Must be cheap when nothing changed.
    virtual void setParams (ParamValues) noexcept = 0;
    virtual void setMonitor (const ModuleMonitor&) noexcept {}

    // In place, two channels.
    virtual void process (float* const* channels, int numSamples, const ProcessContext&) noexcept = 0;

    // Latency of the current settings (after the last setParams). Must be identical for both channels.
    virtual int getLatencySamples() const noexcept = 0;

    // Audio thread, before setParams: predict a latency-changing configuration without mutating DSP or publishing
    // worker requests. The chain can then fade every aligned path before committing the change. Modules with
    // fixed latency use the default; variable-latency modules may override this with their shared-parameter rule.
    virtual int getLatencySamplesForParams (ParamValues) const noexcept { return getLatencySamples(); }

    // true: the module computes its own Delta signal into ProcessContext::deltaOut (the Maximizer excludes Gain and
    // Output Level from Delta: delta = what the limiting stages removed / added). Its normal output is unaffected.
    virtual bool handlesDelta() const noexcept { return false; }

    // (phase 2) Transient/Sustain twin: the chain calls setParamSet (1) once on the sustain instance, before prepare().
    // The value never changes afterwards. See the rules at the top of this file.
    void setParamSet (int set) noexcept { moduleParamSet = set == 1 ? 1 : 0; }
    int getParamSet() const noexcept { return moduleParamSet; }

private:
    int moduleParamSet = 0;
};

} // namespace ps
