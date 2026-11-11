#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "plugin/Assistant.h"
#include "plugin/Bridge.h"
#include "plugin/PluginProcessor.h"

#include <atomic>
#include <iostream>
#include <thread>

// PureSheenDev bridgehost <folder> [seconds] [stop file name]
//   Runs one suite instance with its bridge on, on <folder>, playing test audio, so tools/bridge/test_bridge.py can drive
//   the real helper program against it. Prints "ready" once it listens, and stops after <seconds> or when the stop file
//   (default "stop") appears in <folder>.
// PureSheenDev bridgetest
//   The connection itself, in this process: refusals, and what a command does to the processor.
#include "MessagePump.h"

namespace
{
void pump (int milliseconds) { ps::dev::pumpMessages (milliseconds); }

// Plays a quiet stereo tone through the processor, as a host would (this thread stands in for the audio thread).
class FakeAudio final : private juce::Thread
{
public:
    explicit FakeAudio (PureSheenProcessor& p) : juce::Thread ("fake audio"), processor (p)
    {
        processor.setPlayConfigDetails (2, 2, 48000.0, 512);
        processor.prepareToPlay (48000.0, 512);
        startThread();
    }

    ~FakeAudio() override
    {
        stopThread (2000);
        processor.releaseResources();
    }

private:
    void run() override
    {
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        double phase = 0.0;
        while (! threadShouldExit())
        {
            for (int i = 0; i < 512; ++i)
            {
                const float v = 0.2f * (float) std::sin (phase);
                phase += juce::MathConstants<double>::twoPi * 220.0 / 48000.0;
                buffer.setSample (0, i, v);
                buffer.setSample (1, i, v);
            }
            processor.processBlock (buffer, midi);
            juce::Thread::sleep (10);
        }
    }

    PureSheenProcessor& processor;
};

// A plain client on a thread of its own, as the message thread must stay free to run the commands.
struct Client
{
    juce::StreamingSocket socket;
    std::string pending;

    bool connect (int port) { return socket.connect ("127.0.0.1", port, 1000); }

    bool send (const juce::String& line)
    {
        const auto text = line + "\n";
        return socket.write (text.toRawUTF8(), (int) text.getNumBytesAsUTF8()) > 0;
    }

    // Empty: closed, or nothing within the time.
    juce::String receive (int timeoutMs)
    {
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        char chunk[4096];
        for (;;)
        {
            const auto newline = pending.find ('\n');
            if (newline != std::string::npos)
            {
                const auto line = pending.substr (0, newline);
                pending.erase (0, newline + 1);
                return juce::String::fromUTF8 (line.data(), (int) line.size());
            }
            const auto now = juce::Time::getMillisecondCounter();
            if (now >= end || socket.waitUntilReady (true, (int) (end - now)) <= 0)
                return {};
            const int got = socket.read (chunk, (int) sizeof (chunk), false);
            if (got <= 0)
                return {};
            pending.append (chunk, (size_t) got);
        }
    }

    bool closedByOtherEnd (int timeoutMs)
    {
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        char byte;
        while (juce::Time::getMillisecondCounter() < end)
        {
            const int ready = socket.waitUntilReady (true, 50);
            if (ready < 0)
                return true;
            if (ready > 0)
                return socket.read (&byte, 1, false) <= 0;
        }
        return false;
    }
};

int checks = 0, failures = 0;

void check (bool ok, const juce::String& what)
{
    ++checks;
    if (! ok)
    {
        ++failures;
        std::cout << "FAILED: " << what << "\n";
    }
}

// Runs `body` on a thread while the message thread pumps.
template <typename Body>
void onClientThread (Body body)
{
    std::atomic<bool> done { false };
    std::thread thread ([&] { body(); done.store (true); });
    const auto end = juce::Time::getMillisecondCounter() + 60000u;
    while (! done.load() && juce::Time::getMillisecondCounter() < end)
        pump (5);
    check (done.load(), "the client finished in time");
    thread.join();
}

juce::var call (Client& client, const juce::String& command, const juce::String& argsJson = "{}")
{
    static int nextId = 0;
    client.send ("{\"id\":" + juce::String (++nextId) + ",\"cmd\":\"" + command + "\",\"args\":" + argsJson + "}");
    return juce::JSON::parse (client.receive (20000));
}
} // namespace

int runBridgeHost (const juce::File& folder, int seconds, const juce::String& stopName)
{
    folder.createDirectory();
    psplug::bridge::allowedFlag (folder).create();

    PureSheenProcessor processor;
    FakeAudio audio (processor);
    processor.getBridge()->useFolder (folder);
    if (! processor.getBridge()->isListening())
    {
        std::cout << "could not listen\n";
        return 1;
    }
    std::cout << "ready" << std::endl;

    const auto stopFile = folder.getChildFile (stopName);
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) (seconds * 1000);
    while (juce::Time::getMillisecondCounter() < end && ! stopFile.existsAsFile())
        pump (20);
    return 0;
}

int runBridgeTests()
{
    const auto folder = juce::File::createTempFile ("").getSiblingFile ("puresheen-bridge-test-" + juce::String (juce::Random::getSystemRandom().nextInt (1000000)));
    folder.createDirectory();

    PureSheenProcessor processor;
    processor.setPresetRoot (folder.getChildFile ("presets"));
    auto& bridge = *processor.getBridge();

    // Off without the flag: no port, no file.
    bridge.useFolder (folder);
    check (! bridge.isListening(), "off until allowed");
    check (folder.findChildFiles (juce::File::findFiles, false, "*.json").isEmpty(), "no registration while off");

    psplug::bridge::allowedFlag (folder).create();
    bridge.pollNow();
    check (bridge.isListening(), "listens once allowed");

    const auto files = folder.findChildFiles (juce::File::findFiles, false, "*.json");
    check (files.size() == 1, "one registration file");
    const auto info = juce::JSON::parse (files[0]);
    const int port = (int) info["port"];
    const auto token = info["token"].toString();
    check (port > 0 && token.length() == 48, "port and a long token");

    const auto hello = "{\"protocol\":1,\"token\":\"" + token + "\"}";

    onClientThread ([&]
    {
        {
            Client c;
            check (c.connect (port), "connects");
            c.send ("{\"protocol\":1,\"token\":\"wrong\"}");
            check (c.closedByOtherEnd (3000), "a wrong token closes the connection");
        }
        {
            Client c;
            c.connect (port);
            c.send ("{\"id\":1,\"cmd\":\"get_status\",\"args\":{}}");
            check (c.closedByOtherEnd (3000), "a command before the token closes the connection");
        }
        {
            Client c;
            c.connect (port);
            c.send ("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
            check (c.closedByOtherEnd (3000), "a web request closes the connection");
        }
        {
            Client c;
            c.connect (port);
            check (c.closedByOtherEnd (5000), "silence closes the connection after a few seconds");
        }
        {
            // Deep nesting would overflow the stack in the JSON parser and take the host with it, token or not.
            Client c;
            c.connect (port);
            c.send (juce::String::repeatedString ("[", 1000));
            check (c.closedByOtherEnd (3000), "deep nesting before the token closes the connection");
        }
        {
            Client c;
            c.connect (port);
            juce::String nested = "{\"protocol\":1,\"token\":\"" + token + "\",\"x\":" + juce::String::repeatedString ("[", 40);
            c.send (nested.substring (0, 1000));
            check (c.closedByOtherEnd (3000), "nesting past the limit in a hello closes the connection");
        }
        {
            Client c;
            c.connect (port);
            c.send (juce::String::repeatedString ("x", 100000));
            check (c.closedByOtherEnd (3000), "a long first line closes the connection");
        }
        {
            Client c;
            c.connect (port);
            c.send (hello);
            check ((bool) juce::JSON::parse (c.receive (3000))["ok"], "connects after the refusals");
            c.send (juce::String::repeatedString ("{\"a\":", 100000));
            check (c.closedByOtherEnd (3000), "deep nesting after the token closes the connection too");
        }
    });

    onClientThread ([&]
    {
        Client c;
        c.connect (port);
        c.send (hello);
        const auto reply = juce::JSON::parse (c.receive (3000));
        check ((bool) reply["ok"] && reply["id"].toString() == info["id"].toString(), "the right token is accepted");

        auto r = call (c, "get_status");
        check ((bool) r["ok"] && r["result"]["product"].toString() == "PureSheen Dynamics", "get_status");

        // A fresh instance: what counts as changed. Skewed parameters do not round trip exactly, and that is no change.
        r = call (c, "get_parameters", "{\"changed_only\":true,\"limit\":5}");
        const int changedAtStart = (int) r["result"]["total"];
        juce::String firstChanged;
        if (r["result"]["parameters"].size() > 0)
            firstChanged = r["result"]["parameters"][0]["id"].toString() + " = " + r["result"]["parameters"][0]["value"].toString()
                         + ", default " + r["result"]["parameters"][0]["default"].toString();
        check (changedAtStart == 0, "nothing counts as changed on a fresh instance: " + juce::String (changedAtStart) + " (" + firstChanged + ")");

        r = call (c, "no_such_command");
        check (! (bool) r["ok"] && r["error"].toString().contains ("Unknown command"), "an unknown command is an error");

        // One undo step, labelled, limited to the range; the plugin's own values.
        const int stepsBefore = (int) processor.getHistory().size();
        r = call (c, "set_parameters", "{\"changes\":[{\"id\":\"max.on\",\"value\":true},{\"id\":\"max.gain\",\"value\":9999}],\"description\":\"test edit\"}");
        check ((bool) r["ok"], "set_parameters works: " + r["error"].toString());
        const float gain = processor.getParameterValue ("max.gain");
        check (gain < 9999.0f && gain > 0.0f, "a value past the range stops at the range");
        const auto history = processor.getHistory();
        check ((int) history.size() == stepsBefore + 1, "one undo step");
        check (history.back().description == "Claude: test edit", "the step is labelled: " + history.back().description);

        // Claude naming itself in the description does not double the label.
        r = call (c, "set_parameters", "{\"changes\":[{\"id\":\"max.gain\",\"value\":1.0}],\"description\":\"Claude: own label\"}");
        check (processor.getHistory().back().description == "Claude: own label", "one prefix: " + processor.getHistory().back().description);
        r = call (c, "undo");

        // A bad entry changes nothing.
        r = call (c, "set_parameters", "{\"changes\":[{\"id\":\"max.gain\",\"value\":1.0},{\"id\":\"nope\",\"value\":1}]}");
        check (! (bool) r["ok"] && processor.getParameterValue ("max.gain") == gain, "a bad entry changes nothing");
        r = call (c, "set_parameters", "{\"changes\":[{\"id\":\"max.gain\",\"value\":\"loud\"}]}");
        check (! (bool) r["ok"], "a word for a number is an error");
        r = call (c, "set_parameters", "{\"changes\":[]}");
        check (! (bool) r["ok"], "no changes is an error");

        // A number past float's range ends at the top of the range, not at the default.
        const float top = ps::paramSpecs()[(size_t) ps::paramIndex ("max.gain")].maxValue;
        r = call (c, "set_parameters", "{\"changes\":[{\"id\":\"max.gain\",\"value\":1e100}]}");
        check ((bool) r["ok"] && processor.getParameterValue ("max.gain") == top, "a huge number stops at the range");
        check (r["result"]["changes"][0]["limited"].toString().isNotEmpty(), "and the reply says so");
        r = call (c, "set_parameters", "{\"changes\":[{\"id\":\"max.gain\",\"value\":" + juce::String (gain) + "}]}");
        r = call (c, "get_history", "{\"limit\":1.5}");
        check (! (bool) r["ok"], "a fraction for a whole number is an error");
        r = call (c, "get_history", "{\"limit\":1e100}");
        check (! (bool) r["ok"], "a huge whole number is an error");

        // Monitoring switches: undo does not restore them, so they stay with the user.
        for (const auto* id : { "io.bypass", "io.gainMatch", "io.gainMatchModern" })
        {
            const float was = processor.getParameterValue (id);
            r = call (c, "set_parameters", "{\"changes\":[{\"id\":\"" + juce::String (id) + "\",\"value\":" + (was >= 0.5f ? "false" : "true") + "}]}");
            check (! (bool) r["ok"] && processor.getParameterValue (id) == was, juce::String (id) + " is refused");
        }

        r = call (c, "undo");
        check ((bool) r["ok"] && processor.getParameterValue ("max.gain") != gain, "undo reverts it");
        r = call (c, "redo");
        check ((bool) r["ok"] && processor.getParameterValue ("max.gain") == gain, "redo brings it back");

        // The chain.
        r = call (c, "remove_module", "{\"module\":\"exc\"}");
        check ((bool) r["ok"] && ! processor.isModuleInChain (ps::ModuleType::Exciter), "remove_module");
        r = call (c, "remove_module", "{\"module\":\"exc\"}");
        check (! (bool) r["ok"], "removing what is not there is an error");
        r = call (c, "add_module", "{\"module\":\"Exciter\"}");
        check ((bool) r["ok"] && processor.isModuleInChain (ps::ModuleType::Exciter), "add_module by name");
        check (processor.getHistory().back().description.startsWith ("Claude: "), "chain edits are labelled too");
        r = call (c, "set_chain", "{\"order\":[\"eq1\",\"eq1\"]}");
        check (! (bool) r["ok"], "a module twice is an error");
        r = call (c, "add_module", "{\"module\":\"nope\"}");
        check (! (bool) r["ok"], "an unknown module is an error");

        // Reading.
        r = call (c, "get_parameters", "{\"module\":\"max\",\"limit\":3}");
        check ((bool) r["ok"] && r["result"]["parameters"].size() == 3 && (int) r["result"]["total"] > 3, "get_parameters pages");
        r = call (c, "get_parameters", "{\"ids\":[\"max.gain\"]}");
        check ((bool) r["ok"] && r["result"]["parameters"].size() == 1 && r["result"]["parameters"][0]["text"].toString().isNotEmpty(), "get_parameters by id");
        r = call (c, "get_meters");
        check ((bool) r["ok"], "get_meters");
        r = call (c, "list_modules");
        check ((bool) r["ok"] && r["result"]["modules"].size() == (int) ps::ModuleType::Count, "list_modules");

        // Presets: never replaced without saying so, and a name cannot climb out of the folder.
        r = call (c, "save_preset", "{\"name\":\"Test Preset\"}");
        check ((bool) r["ok"], "save_preset");
        r = call (c, "save_preset", "{\"name\":\"Test Preset\"}");
        check (! (bool) r["ok"], "an existing preset is not replaced");
        r = call (c, "save_preset", "{\"name\":\"Test Preset\",\"overwrite\":true}");
        check ((bool) r["ok"], "unless asked");
        r = call (c, "save_preset", "{\"name\":\"..\\\\..\\\\..\\\\escaped\"}");
        check (! folder.getSiblingFile ("escaped.pspreset").existsAsFile() && ! folder.getChildFile ("escaped.pspreset").existsAsFile(), "a preset name stays in the preset folder");
        for (const auto* category : { "..", "../..", "a\\\\..\\\\.." })
        {
            r = call (c, "save_preset", "{\"name\":\"Climber\",\"category\":\"" + juce::String (category) + "\"}");
            check (! (bool) r["ok"], "a category cannot climb out: " + juce::String (category));
        }
        r = call (c, "save_preset", "{\"name\":\"..\"}");
        check (! (bool) r["ok"], "a preset cannot be named ..");
        r = call (c, "load_preset", "{\"name\":\"Test Preset\"}");
        check ((bool) r["ok"], "load_preset");
        r = call (c, "load_preset", "{\"name\":\"nope\"}");
        check (! (bool) r["ok"], "an unknown preset is an error");

        // Snapshots: the same promise.
        r = call (c, "store_snapshot", "{\"slot\":\"A\"}");
        check ((bool) r["ok"] && processor.hasSnapshot (0), "store_snapshot");
        r = call (c, "store_snapshot", "{\"slot\":\"A\"}");
        check (! (bool) r["ok"], "a snapshot in use is not replaced");
        r = call (c, "recall_snapshot", "{\"slot\":\"B\"}");
        check (! (bool) r["ok"], "an empty snapshot is an error");

        // Files: only existing audio files.
        r = call (c, "add_reference", "{\"path\":\"C:\\\\no\\\\such\\\\file.wav\"}");
        check (! (bool) r["ok"], "a missing reference file is an error");
        r = call (c, "add_reference", "{\"path\":\"relative.wav\"}");
        check (! (bool) r["ok"], "a relative path is an error");
        r = call (c, "get_comparison");
        check ((bool) r["ok"] && ! (bool) r["result"]["ready"], "get_comparison without a reference");
        r = call (c, "get_assistant");
        check ((bool) r["ok"] && r["result"]["phase"].toString() == "idle", "get_assistant");

        // Oversize and garbage on an authenticated connection.
        c.send ("this is not json");
        check (c.closedByOtherEnd (3000), "garbage closes the connection");
    });

    // The user's own edit, finished just before a command, keeps its own step and name.
    if (auto* parameter = processor.getParameter ("max.gain"))
    {
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (1.5f));
        parameter->endChangeGesture();
    }

    onClientThread ([&]
    {
        Client c;
        c.connect (port);
        c.send (hello);
        c.receive (3000);
        auto r = call (c, "set_parameters", "{\"changes\":[{\"id\":\"max.gain\",\"value\":2.5}],\"description\":\"after the user\"}");
        check ((bool) r["ok"], "a command right after a user edit");
        const auto history = processor.getHistory();
        check (history.size() >= 2 && history.back().description == "Claude: after the user", "the command is labelled");
        check (history.size() >= 2 && ! history[history.size() - 2].description.startsWith ("Claude"),
               "the user's edit is not: " + history[history.size() - 2].description);

        for (int i = 0; i < 40; ++i)
        {
            r = call (c, "get_status");
            check ((bool) r["ok"], "many calls in a row");
        }
    });

    check (processor.getBridge()->getNumClients() <= psplug::Bridge::kMaxClients, "at most the client limit");

    // The user's macro drag in progress: a command closes it as the user's own step, it does not take it along.
    {
        const auto wav = folder.getChildFile ("assistant.wav");
        {
            juce::WavAudioFormat format;
            std::unique_ptr<juce::OutputStream> stream (wav.createOutputStream().release());
            auto options = juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (2).withBitsPerSample (24);
            auto writer = format.createWriterFor (stream, options);
            juce::Random random (1);
            juce::AudioBuffer<float> audio (2, 48000 * 10);
            for (int i = 0; i < audio.getNumSamples(); ++i)
            {
                const float tone = 0.3f * (float) std::sin (0.03 * i) * (0.5f + 0.5f * (float) std::sin (0.0005 * i));
                audio.setSample (0, i, tone + 0.05f * (random.nextFloat() - 0.5f));
                audio.setSample (1, i, tone + 0.05f * (random.nextFloat() - 0.5f));
            }
            writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
        }

        auto& assistant = processor.getAssistant();
        onClientThread ([&]
        {
            Client c;
            c.connect (port);
            c.send (hello);
            c.receive (3000);
            auto r = call (c, "assistant_set_macro", "{\"macro\":\"Width\",\"value\":50}");
            check (! (bool) r["ok"], "a macro without an applied suggestion is an error: " + r["error"].toString());
        });

        check (assistant.analyseFile (wav), "the Assistant takes the file");
        const auto end = juce::Time::getMillisecondCounter() + 60000u;
        while ((assistant.getAppliedIndex() < 0 || assistant.isWorking()) && juce::Time::getMillisecondCounter() < end)
            pump (20);
        check (assistant.getAppliedIndex() >= 0, "the Assistant applied a suggestion");

        assistant.setMacro (ps::assist::Macro::Width, 60.0f, false);   // a drag, not finished
        const int stepsBefore = (int) processor.getHistory().size();

        onClientThread ([&]
        {
            Client c;
            c.connect (port);
            c.send (hello);
            c.receive (3000);
            auto r = call (c, "set_parameters", "{\"changes\":[{\"id\":\"max.gain\",\"value\":1.25}],\"description\":\"during a drag\"}");
            check ((bool) r["ok"], "a command during a macro drag");
        });

        const auto history = processor.getHistory();
        check ((int) history.size() == stepsBefore + 2, "the drag and the command are two steps");
        check (history.back().description == "Claude: during a drag", "the command is labelled");
        check (history.size() >= 2 && ! history[history.size() - 2].description.startsWith ("Claude"),
               "the drag is the user's: " + (history.size() >= 2 ? history[history.size() - 2].description : juce::String()));
        processor.undo();
        check (std::abs (processor.getParameterValue ("img.amount") - 60.0f) < 0.01f, "undoing the command keeps the user's drag");
    }

    // Off again: the file goes, the port closes.
    psplug::bridge::allowedFlag (folder).deleteFile();
    bridge.pollNow();
    check (! bridge.isListening(), "stops when no longer allowed");
    check (folder.findChildFiles (juce::File::findFiles, false, "*.json").isEmpty(), "the registration file goes");
    onClientThread ([&]
    {
        Client c;
        check (! c.connect (port), "the port is closed");
    });

    // Back on, and closed by the destructor while a client is connected.
    psplug::bridge::allowedFlag (folder).create();
    bridge.pollNow();
    const auto again = juce::JSON::parse (folder.findChildFiles (juce::File::findFiles, false, "*.json")[0]);
    {
        std::atomic<bool> connected { false }, closed { false };
        std::thread thread ([&]
        {
            Client c;
            c.connect ((int) again["port"]);
            c.send ("{\"protocol\":1,\"token\":\"" + again["token"].toString() + "\"}");
            c.receive (3000);
            connected.store (true);
            closed.store (c.closedByOtherEnd (20000));
        });
        while (! connected.load())
            pump (5);
        processor.getBridge()->useFolder (folder.getChildFile ("elsewhere"));   // stops the server
        thread.join();
        check (closed.load(), "a connected client is closed when the server stops");
    }

    // The single-module plugins have no bridge.
    {
        PureSheenProcessor single (psplug::productById ("eq"));
        check (single.getBridge() == nullptr, "a single-module plugin has no bridge");
    }

    folder.deleteRecursively();
    std::cout << "bridgetest: " << checks - failures << " passed, " << failures << " failed" << std::endl;
    return failures == 0 ? 0 : 1;
}
