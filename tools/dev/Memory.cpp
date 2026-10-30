#include "plugin/PluginProcessor.h"

#include <iomanip>
#include <iostream>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <psapi.h>
 #pragma comment (lib, "psapi.lib")
#endif

// PureSheenDev memory <sampleRate> [ts] [editor] [core] (owner: integration)
//
// Measures the process's private memory (commit charge) before and after building a processor with all 14 modules
// in the chain, prepared at <sampleRate> with a 512-sample block, after 2 s of audio. "ts" switches every module that
// offers the Transient/Sustain mode into it first; "editor" also opens the editor; "core" keeps the default chain.
// Run one configuration per process so the numbers do not include the heap left over from a previous one.
// The chain host prepares every module type (in the chain or not) and every T/S twin, so the chain contents and the
// channel modes change the numbers very little; the sample rate scales them.
namespace
{
double privateMegabytes()
{
   #if JUCE_WINDOWS
    PROCESS_MEMORY_COUNTERS_EX pmc {};
    pmc.cb = sizeof (pmc);
    if (GetProcessMemoryInfo (GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*> (&pmc), sizeof (pmc)))
        return (double) pmc.PrivateUsage / (1024.0 * 1024.0);
   #endif
    return 0.0;
}
} // namespace

int runMemory (const juce::StringArray& args)
{
    const double rate = args.size() > 0 ? juce::jlimit (8000.0, 384000.0, args[0].getDoubleValue()) : 48000.0;
    const bool ts = args.contains ("ts");
    const bool withEditor = args.contains ("editor");
    const bool coreOnly = args.contains ("core");   // keep the default chain (the chain host prepares every type anyway)

    const double base = privateMegabytes();
    double afterPrepare = 0.0, afterAudio = 0.0, afterEditor = 0.0;
    {
        PureSheenProcessor proc;
        if (! coreOnly)
        {
            for (int t = ps::kNumCoreModuleTypes; t < ps::kNumModuleTypes; ++t)
                proc.addModule ((ps::ModuleType) t);
            proc.addModule (ps::ModuleType::Equalizer2);
        }
        if (ts)
            for (int t = 0; t < ps::kNumModuleTypes; ++t)
                if (ps::supportsTransientSustain ((ps::ModuleType) t))
                    for (const auto& [id, v] : ps::channelModeParamValues ((ps::ModuleType) t, ps::ChannelMode::TransientSustain))
                        proc.setParameterValue (id, v);
        proc.flushPendingChanges (true);
        proc.prepareToPlay (rate, 512);
        afterPrepare = privateMegabytes();

        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        unsigned seed = 1;
        for (int block = 0; block < (int) (rate * 2.0 / 512.0); ++block)
        {
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 512; ++i)
                {
                    seed = seed * 1664525u + 1013904223u;
                    buffer.setSample (c, i, 0.2f * ((float) (seed >> 8) / 8388608.0f - 1.0f));
                }
            proc.processBlock (buffer, midi);
            if (block % 16 == 0)
                proc.flushPendingChanges (true);
        }
        proc.flushPendingChanges (true);
        afterAudio = privateMegabytes();

        if (withEditor)
        {
            std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
            afterEditor = privateMegabytes();
        }

        std::cout << std::fixed << std::setprecision (1)
                  << "memory: " << (int) proc.getChainOrder().size() << " modules at " << rate << " Hz, T/S "
                  << (ts ? "on" : "off") << "\n"
                  << "  process private before the processor: " << base << " MB\n"
                  << "  after prepare: " << afterPrepare << " MB (+" << afterPrepare - base << ")\n"
                  << "  after 2 s of audio: " << afterAudio << " MB (+" << afterAudio - base << ")\n";
        if (withEditor)
            std::cout << "  with the editor open: " << afterEditor << " MB (+" << afterEditor - base << ")\n";
        std::cout << "  latency " << proc.getLatencySamples() << " samples\n";
    }
    return 0;
}
