#pragma once

#include <juce_core/juce_core.h>

// What the suite (plugin/Bridge.h) and the helper program (tools/bridge/BridgeMain.cpp) agree on. juce_core only: the
// helper links nothing else.
//
//   Folder:      %APPDATA%\PureSheen Dynamics\Claude bridge (PURESHEEN_BRIDGE_DIR overrides it, for tests)
//   "allowed":   an empty file there is the user's "Allow Claude to connect" switch, shared by every instance
//   <id>.json:   one per running instance that allows connections: { id, port, token, pid, product, version }
//
//   Connection:  TCP to 127.0.0.1:<port>, one JSON object per line (UTF-8), the helper speaks first.
//     -> { "protocol": 1, "token": "<token>" }
//     <- { "ok": true, "id": "<instance id>" }   (the connection just closes when the token or the protocol is wrong)
//     -> { "id": 1, "cmd": "get_status", "args": { } }
//     <- { "id": 1, "ok": true, "result": { } }   or   { "id": 1, "ok": false, "error": "plain words" }
namespace psplug::bridge
{
inline constexpr int kProtocolVersion = 1;
inline constexpr int kMaxLineBytes = 2 * 1024 * 1024;   // a request; the suite closes a connection that goes past it
inline constexpr int kMaxHelloBytes = 1024;             // the first line, before the token is known to be right
inline constexpr int kMaxJsonDepth = 32;

// juce::JSON::parse recurses, so deep nesting from anyone who can reach the port would overflow the stack and take the
// host with it. Nothing here legitimately nests this deep: this runs on every line before it is parsed.
inline bool jsonNestingIsSafe (const char* data, size_t size)
{
    int depth = 0;
    bool inString = false, escaped = false;
    for (size_t i = 0; i < size; ++i)
    {
        const char c = data[i];
        if (inString)
        {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                inString = false;
        }
        else if (c == '"')
        {
            inString = true;
        }
        else if (c == '[' || c == '{')
        {
            if (++depth > kMaxJsonDepth)
                return false;
        }
        else if (c == ']' || c == '}')
        {
            --depth;
        }
    }
    return true;
}

inline juce::File folder()
{
    const auto overridden = juce::SystemStats::getEnvironmentVariable ("PURESHEEN_BRIDGE_DIR", {});
    if (overridden.isNotEmpty())
        return juce::File (overridden);
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("PureSheen Dynamics")
        .getChildFile ("Claude bridge");
}

inline juce::File allowedFlag (const juce::File& root) { return root.getChildFile ("allowed"); }
} // namespace psplug::bridge
