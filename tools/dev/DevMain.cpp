#include <juce_gui_basics/juce_gui_basics.h>

#include <iostream>

// PureSheenDev: developer tool built from the same sources as the plugin.
//   PureSheenDev selftest                 -> processor-level integration tests (realtime allocation checks, state,
//                                            latency, chain edits, undo/redo, presets), exit code 0 on success
//   PureSheenDev snapshot <outDir>        -> renders the editor in several states to PNG files in outDir
//   PureSheenDev render <in.wav> <out.wav> [--preset <name>] [--module-preset <prefix>:<name>] [--bits 16|24|32]
//                       [--block <n>] [id=value ...]
//                                         -> plays <in.wav> through the processor like a host would, writes the
//                                            latency-compensated result and prints loudness / true peak / LRA

int runSelfTest();                                                    // SelfTest.cpp
int runSnapshot (const juce::File& outputDir);                        // Snapshot.cpp
int runRender (const juce::File& input, const juce::File& output,     // Render.cpp
               const juce::StringArray& options);

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const juce::String command = argc > 1 ? juce::String (argv[1]) : juce::String();
    const auto cwd = juce::File::getCurrentWorkingDirectory();

    if (command == "selftest")
        return runSelfTest();

    if (command == "snapshot")
        return runSnapshot (cwd.getChildFile (argc > 2 ? argv[2] : "out/snapshots"));

    if (command == "render" && argc > 3)
    {
        juce::StringArray options;
        for (int i = 4; i < argc; ++i)
            options.add (juce::String::fromUTF8 (argv[i]));
        return runRender (cwd.getChildFile (juce::String::fromUTF8 (argv[2])),
                          cwd.getChildFile (juce::String::fromUTF8 (argv[3])), options);
    }

    std::cout << "usage: PureSheenDev selftest | snapshot <outDir> | render <in.wav> <out.wav> [--preset <name>]\n"
                 "       [--module-preset <prefix>:<name>] [--bits 16|24|32] [--block <n>] [id=value ...]\n";
    return 2;
}
