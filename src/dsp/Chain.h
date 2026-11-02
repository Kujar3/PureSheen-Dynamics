#pragma once

#include "dsp/Dither.h"
#include "dsp/Module.h"

#include <memory>
#include <vector>

// The signal chain host (pure C++17, unit-tested in CoreTests). Owner: chain. Implementation: Chain.cpp.
//
// Per block (audio thread):
//   1. read IoParams, SlotParams of every module, DitherParams (ParamValues)
//   2. input gain (L, R) -> input taps (FIFO + meters)
//   3. modules in chain order, each through its slot (see Module.h): channel mode encode/decode, channel on/off,
//      power (latency-preserving), solo (only the soloed module processes, the others pass their latency-aligned dry
//      signal), delta, difference meter, module tap FIFOs for the selected module (pre / post)
//      DC filter (DitherParams::dcFilter): 1 Hz high-pass right before the Maximizer when it is in the chain
//   4. output gain (L, R), Sum to Mono, Swap Channels
//   5. gain match (IoParams::gainMatch): K-weighted short-term (3 s, smoothed ~1 s) loudness of the chain input and the
//      output; classic: the bypassed signal (global bypass, or a bypassed module while it is soloed) is levelled to
//      the processed loudness; modern: the processed output is levelled to the input loudness. The match gain is
//      frozen while either side is below -60 LUFS short-term, smoothed, limited to +-24 dB
//   6. global bypass: output = latency-aligned input (* match gain in classic gain-match mode)
//   7. dither (last)
//   8. output taps (FIFO + meters: peak, true peak, LUFS M / S / I, LRA)
// Latency = sum of the slot latencies of the modules in the chain (power on or off): a module's getLatencySamples(),
// or in Transient/Sustain the splitter latency + max (latency of instance A, of the twin). A phase 2 parameter
// change that alters latency is committed after the slot and monitoring buses fade to silence; getLatencySamples()
// reports the committed audio latency so the processor can update the host (setLatencySamples on the message thread).
//
// Transient/Sustain (phase 2): every type that offers T/S gets a second instance (the twin, getModule (type, 1),
// paramSet 1). While a slot runs in T/S: x -> ps::TransientSplitter -> T (instance A, set A) and S (twin, set B), both
// processed as Stereo, padded to a common latency, then
//   channel power (a.on = Transient, b.on = Sustain): that part passes unprocessed (latency-aligned dry part)
//   channel solo (setChannelSolo 0 / 1): only that processed part is heard (Delta then plays that part's change)
//   power off / solo-out / mode fade: the latency-aligned dry signal, as in the other modes
//   monitors: band solo / detector solo / region solo go to both instances; a band solo with soloChannelSet >= 0
//   or an EQ audition goes only to the selected instance, and the other part is muted while it is on
//   difference meter: T + S vs the aligned dry input; IoMeters::moduleTsActivity / moduleTsShareDb from the splitter
// A channel-mode change that changes the slot latency (into or out of T/S) also fades the slot output to silence
// (20 ms), switches at silence and fades back in, so the latency jump never clicks.
//
// Chain order changes (message thread, setOrder) are published lock-free (double-buffered, atomic index); a module
// added to the chain is reset() first, a module removed stops processing. Max one instance per ModuleType.
namespace ps
{
struct HostInfo
{
    double bpm = 120.0;
    bool isPlaying = false;
};

struct IoMeters
{
    AtomicFloat inputPeak[2], outputPeak[2];           // linear, max since the UI last exchanged them
    AtomicFloat outputTruePeak;                        // linear, max over channels since last exchange
    AtomicFloat inputTruePeak;
    AtomicFloat outputMomentaryLufs { -200.0f }, outputShortTermLufs { -200.0f }, outputIntegratedLufs { -200.0f };
    AtomicFloat outputLra { 0.0f };
    AtomicFloat inputMomentaryLufs { -200.0f }, inputShortTermLufs { -200.0f }, inputIntegratedLufs { -200.0f };
    AtomicFloat gainMatchDb { 0.0f };                  // currently applied match gain
    AtomicFloat inputClip[2], outputClip[2];           // 1 once a sample > 0 dBFS was seen (UI resets to 0)
    AtomicFloat moduleDifferenceDb[kNumModuleTypes];   // slot difference meters (RMS out - in, smoothed ~300 ms)
    // (phase 2) Transient/Sustain slots: splitter mask activity 0..1 (max since the UI exchanged it) and the transient
    // part's energy share 10 log10 (E_T / E_x) (one-pole ~300 ms, -60 floor; -60 while the slot is not in T/S).
    AtomicFloat moduleTsActivity[kNumModuleTypes];
    AtomicFloat moduleTsShareDb[kNumModuleTypes];
};

// (added, phase 3) A new, unprepared instance of a module type (the chain's own factory); the Assistant's offline
// renderer builds the few modules it needs with it. nullptr for ModuleType::Count.
std::unique_ptr<Module> createModule (ModuleType);

class Chain
{
public:
    Chain();
    ~Chain();

    // Message thread. Allocates; prepares every module (in or out of the chain) and the FIFOs.
    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;

    //==============================================================================
    // Message thread
    void setOrder (const std::vector<ModuleType>&);    // duplicates / unknown types dropped
    std::vector<ModuleType> getOrder() const;
    bool isInChain (ModuleType) const;

    void setSolo (int moduleTypeOrMinus1) noexcept;    // only that module processes
    int getSolo() const noexcept;
    void setDelta (ModuleType, bool) noexcept;         // hear only what the module changes
    bool getDelta (ModuleType) const noexcept;
    void setChannelSolo (ModuleType, int channelSetOrMinus1) noexcept;   // M/S, L/R: hear only Mid / Side ...
    void setMonitor (ModuleType, const ModuleMonitor&) noexcept;         // band solo, EQ audition, ...
    void setTapModule (int moduleTypeOrMinus1) noexcept;                 // which module feeds the module taps
    void resetIntegratedLoudness() noexcept;                             // I / LRA readouts (UI "reset")

    //==============================================================================
    // Audio thread
    void process (float* const* channels, int numSamples, ParamValues, const HostInfo&) noexcept;

    //==============================================================================
    // Any thread
    int getLatencySamples() const noexcept;
    double getSampleRate() const noexcept;

    Module& getModule (ModuleType) noexcept;           // e.g. static_cast<Maximizer&> (getModule (Maximizer))
    // (phase 2) part 0 = the normal instance, 1 = the T/S sustain twin (the normal instance when the type has no twin).
    // The UI reads the meters of the instance of the selected T/S tab.
    Module& getModule (ModuleType, int part) noexcept;
    bool hasTwin (ModuleType) const noexcept;
    const IoMeters& getIoMeters() const noexcept;
    IoMeters& getIoMeters() noexcept;                  // (added) for the UI's exchange() / clip resets
    const DitherMeters& getDitherMeters() const noexcept;
    void resetDitherPeakBits() noexcept;               // (added) bit meter peak-hold reset (UI)

    // (added) Test / tooling hook, message thread, before prepare(): replaces the module instance that serves the
    // slot of `type` (tests inject small gain / latency modules). nullptr is ignored.
    void setModule (ModuleType type, std::unique_ptr<Module> module);
    // (phase 2) the same for one part: part 1 replaces the twin (only for types that offer T/S; setParamSet (1) is
    // called on it), part 0 the normal instance.
    void setModule (ModuleType type, std::unique_ptr<Module> module, int part);

    // (phase 2) T/S slots: what the module taps carry: the full slot input / output (-1, default), or the transient
    // (0) / sustain (1) part, pre (latency aligned to the post tap) and post the module. Other modes ignore it.
    void setTapPart (int partOrMinus1) noexcept;

    // UI taps (single consumer each: the editor). Capacity 2^15 samples.
    StereoSampleFifo& getInputFifo() noexcept;         // after input gain
    StereoSampleFifo& getOutputFifo() noexcept;        // final output (vectorscope, spectrum, meters)
    StereoSampleFifo& getModulePreFifo() noexcept;     // tap module input (in its channel domain: L/R)
    StereoSampleFifo& getModulePostFifo() noexcept;    // tap module output

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ps
