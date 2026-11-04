// PureSheenBridge: lets Claude (Claude Desktop, Claude Code) control PureSheen Dynamics. Claude starts this program and
// talks MCP to it over stdin / stdout (JSON-RPC, one message per line). The program finds the running instances of the
// suite that allow connections (plugin/BridgeProtocol.h) and passes each tool call on to the chosen one. It installs
// nothing, keeps no state of its own, and reaches nothing but 127.0.0.1.
//
//   PureSheenBridge            run as an MCP server (what Claude starts)
//   PureSheenBridge --version  print the version

#include <juce_core/juce_core.h>

#include "plugin/BridgeProtocol.h"

#include <cstdio>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#if JUCE_WINDOWS
 #include <fcntl.h>
 #include <io.h>
 #include <windows.h>
#endif

namespace
{
using juce::var;

constexpr const char* kServerName = "puresheen-dynamics";
constexpr const char* kLatestMcpVersion = "2025-06-18";

constexpr const char* kInstructions =
    "This controls PureSheen Dynamics, a mastering suite plugin running in the user's DAW. You cannot hear the audio and "
    "you cannot start or stop playback: the meters, the Assistant and the learn functions only have something to work "
    "with while the user plays their track, so ask them to press play when a step needs audio. Decide from measurements, "
    "and let the user's ears make the final call. Start with get_status. Each change to settings or the chain is one undo "
    "step, shown in the plugin's history, and undo reverts it. Prefer small moves, say what you changed, and check "
    "get_meters afterwards. get_meters says whether the Maximizer's ceiling applies to true peak or to sample peak. With "
    "True Peak off (max.truePeak), the reported true peak can pass the ceiling without anything being wrong: that is how "
    "the Assistant's suggestions start, on purpose. Turn it on when the master is for streaming or the user asks, and say "
    "that you did. With True Peak on, the true peak should stay at the ceiling. Bypass and Gain Match stay with the user.";

var makeObject (std::initializer_list<std::pair<const char*, var>> properties)
{
    auto object = std::make_unique<juce::DynamicObject>();
    for (const auto& [name, value] : properties)
        object->setProperty (name, value);
    return var (object.release());
}

//==============================================================================
// The tools. Names (other than list_instances and select_instance) are the suite's commands, BridgeCommands.cpp.
struct Tool
{
    const char* name;
    const char* description;
    const char* schema;      // the properties of the input object, as JSON
    const char* required;    // JSON list
    bool readOnly;
    bool idempotent;
};

const std::vector<Tool>& tools()
{
    static const std::vector<Tool> list {
        { "list_instances", "Lists the running instances of PureSheen Dynamics that allow connections: id, track, preset, chain.",
          R"({})", "[]", true, true },
        { "select_instance", "Chooses the instance later calls go to. Only needed when several are running.",
          R"({"id":{"type":"string","description":"An id from list_instances."}})", R"(["id"])", false, true },
        { "get_status", "The overview: version, host, track, sample rate, latency, preset, bypass, the module chain, undo state.",
          R"({})", "[]", true, true },
        { "get_meters", "Loudness now (momentary, short-term, integrated LUFS, loudness range) at the input and output, the Maximizer's gain reduction, and each module's level change. True peak and the Maximizer's peak gain reduction are the highest since the last get_meters call, so call it once to clear old peaks and again after the audio you care about. The gain reduction 'now' values are one audio block, usually 0 between peaks: use the peak value to judge limiting. ceiling_applies_to says which peak the Maximizer's ceiling limits: with 'sample peak', the true peak can read above the ceiling and that is expected. Null until there is audio.",
          R"({})", "[]", false, false },
        { "reset_loudness", "Restarts the integrated loudness and loudness range measurements, for instance when the user plays the track from the top.",
          R"({})", "[]", false, true },
        { "list_modules", "Every module type: its id (the prefix of its parameter ids), name, whether it is in the chain and on, and how many parameters it has.",
          R"({})", "[]", true, true },
        { "get_parameters", "Reads parameters: value, text, default, range, unit or choices. Use a filter: there are over a thousand. A module's power switch is <module>.on.",
          R"({"module":{"type":"string","description":"A module id, or 'global' for input, output and bypass."},"ids":{"type":"array","items":{"type":"string"}},"query":{"type":"string","description":"Text in the id or name."},"changed_only":{"type":"boolean","description":"Only those away from their default."},"offset":{"type":"integer"},"limit":{"type":"integer","description":"Default 200, at most 1000."}})",
          "[]", true, true },
        { "set_parameters", "Changes parameters as one undo step. Numbers are limited to the parameter's range, and the reply says when that happened. Switches take true or false, choices their label or index. Bypass and Gain Match are refused: they stay with the user. If any entry is invalid, nothing changes.",
          R"({"changes":{"type":"array","items":{"type":"object","properties":{"id":{"type":"string"},"value":{"type":["number","boolean","string"]}},"required":["id","value"]}},"description":{"type":"string","description":"What this does, for the history list, in a few words."}})",
          R"(["changes"])", false, true },
        { "add_module", "Adds a module to the chain at its default settings. By default it goes before the Maximizer.",
          R"({"module":{"type":"string"},"position":{"type":"integer","description":"0 is first."}})", R"(["module"])", false, false },
        { "remove_module", "Removes a module from the chain. Undo brings it back with its settings.",
          R"({"module":{"type":"string"}})", R"(["module"])", false, false },
        { "set_chain", "Sets the whole chain: the modules in this order, each at most once. Modules left out are removed.",
          R"({"order":{"type":"array","items":{"type":"string"}}})", R"(["order"])", false, true },
        { "reset_module", "Sets every parameter of a module back to its default.",
          R"({"module":{"type":"string"}})", R"(["module"])", false, true },
        { "get_history", "The undo history, newest last.",
          R"({"limit":{"type":"integer","description":"Default 30."}})", "[]", true, true },
        { "undo", "Undoes the last change, whoever made it.", R"({})", "[]", false, false },
        { "redo", "Redoes the last undone change.", R"({})", "[]", false, false },
        { "store_snapshot", "Stores the current settings in snapshot A, B, C or D, for comparing versions. Refuses to replace a snapshot in use unless overwrite is true.",
          R"({"slot":{"type":"string","enum":["A","B","C","D"]},"overwrite":{"type":"boolean","description":"Only when the user agreed."}})", R"(["slot"])", false, false },
        { "recall_snapshot", "Loads a stored snapshot, as one undo step.",
          R"({"slot":{"type":"string","enum":["A","B","C","D"]}})", R"(["slot"])", false, true },
        { "list_presets", "Lists presets: for the whole chain by default, or for one module.",
          R"({"scope":{"type":"string","description":"'global' or a module id."}})", "[]", true, true },
        { "load_preset", "Loads a preset by name, as one undo step.",
          R"({"name":{"type":"string"},"scope":{"type":"string"},"category":{"type":"string","description":"When two presets share a name."}})", R"(["name"])", false, true },
        { "save_preset", "Saves the current settings as a user preset. Refuses to replace an existing preset unless overwrite is true.",
          R"({"name":{"type":"string"},"scope":{"type":"string"},"category":{"type":"string"},"overwrite":{"type":"boolean","description":"Only when the user agreed."}})", R"(["name"])", false, false },
        { "learn", "Starts, cancels or checks a learn function while the user plays the loudest part: 'maximizer' sets the Maximizer gain, 'de_limiter' sets the De-limiter threshold.",
          R"({"target":{"type":"string","enum":["maximizer","de_limiter"]},"action":{"type":"string","enum":["start","cancel","status"]}})", R"(["target"])", false, false },
        { "get_assistant", "The Assistant: phase, progress, suggestions, the applied one, macros, settings. Suggestions are ready to use once they are listed. 'working' can stay true for a minute or two while it refines their loudness in the background: no need to wait for it. 'applied' is the suggestion applied last: undo restores the settings but does not change it. Suggestions start with True Peak off and a -0.1 dB ceiling on purpose, so their true peak can read above the ceiling. That is not a fault to fix unprompted.",
          R"({})", "[]", true, true },
        { "assistant_listen", "Makes the Assistant listen to the input while the user plays their track (silence does not count), then build suggestions. It applies the default suggestion by itself. Poll get_assistant. action 'cancel' stops it.",
          R"({"action":{"type":"string","enum":["start","cancel"]}})", "[]", false, false },
        { "assistant_analyse_file", "Makes the Assistant analyse an audio file instead of listening, so no playback is needed. Poll get_assistant.",
          R"({"path":{"type":"string","description":"Full path."}})", R"(["path"])", false, false },
        { "assistant_apply", "Applies one of the Assistant's suggestions: a whole chain, as one undo step.",
          R"({"index":{"type":"integer"}})", R"(["index"])", false, true },
        { "assistant_set_macro", "Moves one of the Assistant's macros, as one undo step. With loudness matching on (the default) a macro also moves the Maximizer gain to keep the loudness steady, replacing a gain set by hand. The reply says when it did.",
          R"({"macro":{"type":"string"},"value":{"type":"number"}})", R"(["macro","value"])", false, true },
        { "assistant_settings", "Changes what the Assistant aims for. Give only what changes. These apply to the next suggestions, not to the current settings. Pass rebuild: true to build suggestions from the audio it has: that applies a new suggestion and replaces changes made since, though undo brings them back. loudness_matched applies straight away.",
          R"({"profile":{"type":"string","description":"'detect' or a genre name such as Rock, Hip-Hop, EDM."},"auto_loudness":{"type":"boolean"},"loudness_lufs":{"type":"number","description":"-20 to -5."},"intensity":{"type":"string","enum":["gentle","balanced","strong"]},"use_maximizer":{"type":"boolean"},"ceiling_db":{"type":"number","description":"-3 to 0."},"true_peak":{"type":"boolean"},"analysis_seconds":{"type":"number","description":"5 to 30."},"loudness_matched":{"type":"boolean"},"rebuild":{"type":"boolean","description":"Default false."}})", "[]", false, true },
        { "assistant_use_reference", "Makes a reference from get_references the Assistant's target, or returns to its own target with id 0. Setting a reference rebuilds the suggestions, which applies a new one over the current settings. Undo brings them back.",
          R"({"id":{"type":"integer"}})", R"(["id"])", false, true },
        { "get_references", "The reference track list: id, file, state, loudness, true peak, loops.",
          R"({})", "[]", true, true },
        { "add_reference", "Adds an audio file to the reference list, up to ten. It loads in the background: poll get_references. Comparing needs the user's master playing.",
          R"({"path":{"type":"string","description":"Full path."},"select":{"type":"boolean","description":"Default true."}})", R"(["path"])", false, true },
        { "remove_reference", "Removes a reference from the list. The file stays on disk.",
          R"({"id":{"type":"integer"}})", R"(["id"])", false, true },
        { "select_reference", "Chooses which reference the comparison uses.",
          R"({"id":{"type":"integer"}})", R"(["id"])", false, true },
        { "set_reference_loop", "Chooses the part of a reference to compare with. With start and end: sets that loop letter and makes it active. Letter alone: makes it active. With clear: clears it. No letter: the whole track.",
          R"({"id":{"type":"integer"},"letter":{"type":"string","enum":["A","B","C","D","E"]},"start_seconds":{"type":"number"},"end_seconds":{"type":"number"},"clear":{"type":"boolean"}})", R"(["id"])", false, true },
        { "get_comparison", "The master against the selected reference: loudness, true peak, dynamics, tonal balance and width per band, and plain-language hints. The master is only analysed while a reference is in the list, so its width and mono compatibility need one too, and they fill in while the user's master plays.",
          R"({})", "[]", true, true },
    };
    return list;
}

//==============================================================================
struct InstanceFile
{
    juce::File file;
    juce::String id, token, product, version;
    int port = 0, pid = 0;
};

bool processIsRunning (int pid)
{
   #if JUCE_WINDOWS
    const auto handle = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD) pid);
    if (handle == nullptr)
        return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code = 0;
    const bool running = GetExitCodeProcess (handle, &code) != 0 && code == STILL_ACTIVE;
    CloseHandle (handle);
    return running;
   #else
    juce::ignoreUnused (pid);
    return true;
   #endif
}

// The instances that say they are there. A file left by a crashed host is removed.
std::vector<InstanceFile> readInstances()
{
    std::vector<InstanceFile> found;
    for (const auto& file : psplug::bridge::folder().findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        const auto json = juce::JSON::parse (file);
        InstanceFile instance;
        instance.file = file;
        instance.id = json["id"].toString();
        instance.token = json["token"].toString();
        instance.product = json["product"].toString();
        instance.version = json["version"].toString();
        instance.port = (int) json["port"];
        instance.pid = (int) json["pid"];
        if (instance.id.isEmpty() || instance.port <= 0)
            continue;
        if (! processIsRunning (instance.pid))
        {
            file.deleteFile();
            continue;
        }
        found.push_back (instance);
    }
    return found;
}

class Connection
{
public:
    bool open (const InstanceFile& instance)
    {
        socket = std::make_unique<juce::StreamingSocket>();
        if (! socket->connect ("127.0.0.1", instance.port, 1000))
            return false;
        if (! writeLine (makeObject ({ { "protocol", psplug::bridge::kProtocolVersion }, { "token", instance.token } })))
            return false;
        const auto reply = readLine (3000);
        return (bool) reply["ok"];
    }

    bool isOpen() const { return socket != nullptr && socket->isConnected(); }

    // Throws juce::String with plain words.
    var call (const juce::String& command, const var& args, int timeoutMs = 30000)
    {
        const int id = ++nextId;
        if (! writeLine (makeObject ({ { "id", id }, { "cmd", command }, { "args", args } })))
        {
            socket.reset();
            throw juce::String ("The connection to PureSheen Dynamics was lost. Was the plugin closed?");
        }
        const auto reply = readLine (timeoutMs);
        if (! reply.isObject())
        {
            socket.reset();
            throw juce::String ("PureSheen Dynamics did not answer. The change may or may not have been made: check get_history.");
        }
        if (! (bool) reply["ok"])
            throw reply["error"].toString();
        return reply["result"];
    }

private:
    bool writeLine (const var& message)
    {
        const auto text = juce::JSON::toString (message, true) + "\n";
        const char* data = text.toRawUTF8();
        int left = (int) text.getNumBytesAsUTF8();
        while (left > 0)
        {
            if (socket->waitUntilReady (false, 5000) <= 0)
                return false;
            const int written = socket->write (data, left);
            if (written <= 0)
                return false;
            data += written;
            left -= written;
        }
        return true;
    }

    var readLine (int timeoutMs)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        char chunk[8192];
        for (;;)
        {
            const auto end = pending.find ('\n');
            if (end != std::string::npos)
            {
                const auto line = pending.substr (0, end);
                pending.erase (0, end + 1);
                return juce::JSON::parse (juce::String::fromUTF8 (line.data(), (int) line.size()));
            }
            const auto now = juce::Time::getMillisecondCounter();
            if (now >= deadline)
                return {};
            const int ready = socket->waitUntilReady (true, (int) (deadline - now));
            if (ready <= 0)
                return {};
            const int got = socket->read (chunk, (int) sizeof (chunk), false);
            if (got <= 0)
                return {};
            pending.append (chunk, (size_t) got);
        }
    }

    std::unique_ptr<juce::StreamingSocket> socket;
    std::string pending;
    int nextId = 0;
};

//==============================================================================
class Server
{
public:
    void run()
    {
        std::string line;
        while (std::getline (std::cin, line))
        {
            while (! line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (! line.empty())
                handle (line);
        }
    }

private:
    static void send (const var& message)
    {
        const auto text = juce::JSON::toString (message, true) + "\n";
        std::fwrite (text.toRawUTF8(), 1, text.getNumBytesAsUTF8(), stdout);
        std::fflush (stdout);
    }

    static void reply (const var& id, const var& result)
    {
        send (makeObject ({ { "jsonrpc", "2.0" }, { "id", id }, { "result", result } }));
    }

    static void replyError (const var& id, int code, const juce::String& message)
    {
        send (makeObject ({ { "jsonrpc", "2.0" }, { "id", id }, { "error", makeObject ({ { "code", code }, { "message", message } }) } }));
    }

    void handle (const std::string& bytes)
    {
        var message;
        if (! psplug::bridge::jsonNestingIsSafe (bytes.data(), bytes.size())
            || juce::JSON::parse (juce::String::fromUTF8 (bytes.data(), (int) bytes.size()), message).failed())
        {
            replyError ({}, -32700, "Parse error");
            return;
        }

        if (message.getDynamicObject() == nullptr || message["jsonrpc"].toString() != "2.0")
        {
            replyError ({}, -32600, "Invalid request");
            return;
        }

        const auto id = message["id"];
        const bool hasId = message.hasProperty ("id");

        if (! message.hasProperty ("method"))
        {
            if (! message.hasProperty ("result") && ! message.hasProperty ("error"))
                replyError (id, -32600, "Invalid request");
            return;   // else a response to something we never asked
        }
        if (! message["method"].isString())
        {
            replyError (id, -32600, "Invalid request");
            return;
        }

        const auto method = message["method"].toString();
        if (! hasId)
            return;   // notifications/initialized and the like need no answer

        if (method == "initialize")
            reply (id, initialize (message["params"]));
        else if (method == "ping")
            reply (id, makeObject ({}));
        else if (method == "tools/list")
            reply (id, listTools());
        else if (method == "tools/call")
            callTool (id, message["params"]);
        else
            replyError (id, -32601, "Method not found: " + method);
    }

    static var initialize (const var& params)
    {
        // Answer in the client's version when we know it, else in ours.
        const auto asked = params["protocolVersion"].toString();
        const juce::StringArray known { "2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25" };
        return makeObject ({
            { "protocolVersion", known.contains (asked) ? asked : juce::String (kLatestMcpVersion) },
            { "capabilities", makeObject ({ { "tools", makeObject ({}) } }) },
            { "serverInfo", makeObject ({ { "name", kServerName }, { "title", "PureSheen Dynamics" }, { "version", PURESHEEN_VERSION } }) },
            { "instructions", kInstructions } });
    }

    static var listTools()
    {
        const auto instanceProperty = makeObject ({ { "type", "string" }, { "description", "Which instance, when several are running. list_instances has the ids." } });

        juce::Array<var> list;
        for (const auto& tool : tools())
        {
            auto properties = juce::JSON::parse (juce::String (tool.schema));
            const juce::String name (tool.name);
            if (name != "list_instances" && name != "select_instance")
                properties.getDynamicObject()->setProperty ("instance", instanceProperty);

            list.add (makeObject ({
                { "name", tool.name },
                { "description", tool.description },
                { "inputSchema", makeObject ({ { "type", "object" }, { "properties", properties }, { "required", juce::JSON::parse (juce::String (tool.required)) } }) },
                { "annotations", makeObject ({ { "readOnlyHint", tool.readOnly }, { "destructiveHint", false }, { "idempotentHint", tool.idempotent }, { "openWorldHint", false } }) } }));
        }
        return makeObject ({ { "tools", list } });
    }

    void callTool (const var& id, const var& params)
    {
        const auto name = params["name"].toString();
        const auto args = params["arguments"].getDynamicObject() != nullptr ? params["arguments"] : makeObject ({});

        bool known = false;
        for (const auto& tool : tools())
            known = known || name == tool.name;
        if (! known)
        {
            replyError (id, -32602, "Unknown tool: " + name);
            return;
        }

        try
        {
            var result;
            if (name == "list_instances")
                result = listInstances();
            else if (name == "select_instance")
                result = selectInstance (args["id"].toString());
            else
                result = forward (name, args);

            reply (id, makeObject ({ { "content", juce::Array<var> { textContent (juce::JSON::toString (result, true)) } } }));
        }
        catch (const juce::String& error)
        {
            reply (id, makeObject ({ { "content", juce::Array<var> { textContent (error) } }, { "isError", true } }));
        }
    }

    static var textContent (const juce::String& text) { return makeObject ({ { "type", "text" }, { "text", text } }); }

    Connection& connectionTo (const InstanceFile& instance)
    {
        auto& connection = connections[instance.id];
        if (connection == nullptr || ! connection->isOpen())
        {
            connection = std::make_unique<Connection>();
            if (! connection->open (instance))
            {
                connections.erase (instance.id);
                throw juce::String ("PureSheen Dynamics (" + instance.id + ") did not accept the connection. It may be closing.");
            }
        }
        return *connection;
    }

    var listInstances()
    {
        juce::Array<var> list;
        for (const auto& instance : readInstances())
        {
            try
            {
                const auto status = connectionTo (instance).call ("get_status", makeObject ({}), 5000);
                list.add (makeObject ({ { "id", instance.id }, { "selected", instance.id == selected }, { "version", instance.version },
                                        { "host", status["host"] }, { "track", status["track"] }, { "preset", status["preset"] }, { "chain", status["chain"] } }));
            }
            catch (const juce::String&)
            {
                // Closing, or its host is busy: it is not one to talk to right now.
            }
        }
        return makeObject ({ { "instances", list }, { "note", list.isEmpty() ? juce::var (noInstanceMessage()) : juce::var() } });
    }

    var selectInstance (const juce::String& id)
    {
        for (const auto& instance : readInstances())
            if (instance.id == id)
            {
                selected = id;
                return makeObject ({ { "selected", id } });
            }
        throw juce::String ("No instance with id '" + id + "'. list_instances has the ids.");
    }

    static juce::String noInstanceMessage()
    {
        return "No PureSheen Dynamics is allowing connections. Ask the user to open PureSheen Dynamics in their DAW, click Options "
               "(the sliders button at the top of the plugin) and turn on Claude > Allow Claude to connect.";
    }

    var forward (const juce::String& command, const var& args)
    {
        const auto instances = readInstances();
        if (instances.empty())
            throw noInstanceMessage();

        auto wanted = args["instance"].toString();
        if (wanted.isEmpty())
            wanted = selected;

        const InstanceFile* target = nullptr;
        for (const auto& instance : instances)
            if (instance.id == wanted)
                target = &instance;

        if (target == nullptr)
        {
            // Never another instance instead: that would change the wrong track.
            if (wanted.isNotEmpty())
                throw juce::String ("Instance '" + wanted + "' is not running any more, so nothing was done. Use list_instances, then select_instance.");
            if (instances.size() > 1)
                throw juce::String ("Several instances of PureSheen Dynamics are running. Use list_instances, then select_instance.");
            target = &instances.front();
        }

        // The instance id is for this program, not a command argument.
        auto forwarded = juce::JSON::parse (juce::JSON::toString (args, true));
        if (auto* object = forwarded.getDynamicObject())
            object->removeProperty ("instance");

        return connectionTo (*target).call (command, forwarded);
    }

    std::map<juce::String, std::unique_ptr<Connection>> connections;
    juce::String selected;
};
} // namespace

int main (int argc, char** argv)
{
    if (argc > 1 && std::string (argv[1]) == "--version")
    {
        std::cout << "PureSheenBridge " << PURESHEEN_VERSION << "\n";
        return 0;
    }
    if (argc > 1 && std::string (argv[1]) == "--tools")   // for the tests: the tool names
    {
        for (const auto& tool : tools())
            std::cout << tool.name << "\n";
        return 0;
    }

   #if JUCE_WINDOWS
    _setmode (_fileno (stdin), _O_BINARY);
    _setmode (_fileno (stdout), _O_BINARY);
   #endif

    Server().run();
    return 0;
}
