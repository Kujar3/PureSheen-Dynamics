#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdlib>
#include <iostream>

// PureSheenDev: developer tool built from the same sources as the plugin.
//   PureSheenDev selftest                 -> processor-level integration tests (realtime allocation checks, state,
//                                            latency, chain edits, undo/redo, presets), exit code 0 on success
//   PureSheenDev snapshot <outDir>        -> renders the editor in several states to PNG files in outDir
//   PureSheenDev render <in.wav> <out.wav> [--preset <name>] [--module-preset <prefix>:<name>] [--bits 16|24|32]
//                       [--block <n>] [--chain <prefix,...>] [id=value ...]
//                                         -> plays <in.wav> through the processor like a host would, writes the
//                                            latency-compensated result and prints loudness / true peak / LRA
//   PureSheenDev icon <out.png> [size]    -> renders the app / plugin icon (resources/icon/logo_512.png)
//   PureSheenDev memory <rate> [ts] [editor]
//                                         -> private memory with all 14 modules prepared at <rate> (Memory.cpp)
//   PureSheenDev import <file-or-folder> ... [--verbose] [--values] [--no-audio] | import --defaults
//                                         -> preset importer over .xml files (ImportTool.cpp)
//   PureSheenDev importkey "<name>" ...  -> the importer's template / key of a ParamID or section name
//   PureSheenDev importtest              -> importer text-path and processor checks

int runSelfTest();                                                    // SelfTest.cpp
int runSnapshot (const juce::File& outputDir);                        // Snapshot.cpp
int runRender (const juce::File& input, const juce::File& output,     // Render.cpp
               const juce::StringArray& options);
int runIcon (const juce::File& output, int size);                     // Icon.cpp
int runMemory (const juce::StringArray& args);                        // Memory.cpp
int runImport (const juce::StringArray& args);                        // ImportTool.cpp
int runImportKey (const juce::StringArray& args);                     // ImportTool.cpp
int runImportTests();                                                 // ImportTool.cpp

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

    if (command == "icon" && argc > 2)
        return runIcon (cwd.getChildFile (juce::String::fromUTF8 (argv[2])), argc > 3 ? std::atoi (argv[3]) : 512);

    if (command == "memory")
    {
        juce::StringArray args;
        for (int i = 2; i < argc; ++i)
            args.add (juce::String::fromUTF8 (argv[i]));
        return runMemory (args);
    }

    if (command == "import" || command == "importkey")
    {
        juce::StringArray args;
        for (int i = 2; i < argc; ++i)
            args.add (juce::String::fromUTF8 (argv[i]));
        return command == "import" ? runImport (args) : runImportKey (args);
    }

    if (command == "importtest")
        return runImportTests();

    std::cout << "usage: PureSheenDev selftest | snapshot <outDir> | render <in.wav> <out.wav> [--preset <name>]\n"
                 "       [--module-preset <prefix>:<name>] [--bits 16|24|32] [--block <n>] [--chain <prefix,...>]\n"
                 "       [--import <file.xml>] [id=value ...]\n"
                 "       icon <out.png> [size]\n"
                 "       import <file-or-folder> ... [--verbose] [--values] [--no-audio] | import --defaults\n"
                 "       importkey \"<ParamID>\" ... | importtest\n";
    return 2;
}
