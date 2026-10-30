#pragma once

#include "dsp/Params.h"
#include "dsp/Fifo.h"

// Base class of every chain module. The Chain (dsp/Chain.h) owns one instance per ModuleType and wraps each in a slot
// that applies, around Module::process:
//   input -> [M/S encode when channelMode == MidSide] -> module.process -> [M/S decode]
//         -> channelOn[c] == false: channel c replaced by the (latency-aligned) dry signal of that channel
//         -> power off: dry signal, latency kept (the slot delays it by the module's latency)
//         -> delta: processed - latency-aligned dry (unless handlesDelta())
//         -> difference meter (RMS out - RMS in, dB)
// So a module only ever sees two channels already in the domain its channel mode asks for, and only implements DSP.
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
};

// Monitoring requests from the UI (not parameters, not saved). One per module, set by the chain from the UI.
struct ModuleMonitor
{
    int soloBand = -1;              // multiband modules: only this band is heard (-1 = off)
    int soloChannelSet = -1;        // reserved (the slot handles channel solo)
    bool auditionOn = false;        // EQ "Alt-solo": band-pass audition around auditionHz
    float auditionHz = 1000.0f;
    float auditionQ = 4.0f;         // 0.2..12
    int auditionChannelSet = 0;
    bool soloDetectionFilter = false;   // Dynamics: hear the detector signal
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

    // true: the module computes its own Delta signal into ProcessContext::deltaOut (the Maximizer excludes Gain and
    // Output Level from Delta: delta = what the limiting stages removed / added). Its normal output is unaffected.
    virtual bool handlesDelta() const noexcept { return false; }
};

} // namespace ps
