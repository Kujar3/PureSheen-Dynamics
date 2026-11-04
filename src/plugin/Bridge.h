#pragma once

#include <juce_events/juce_events.h>

#include "plugin/BridgeProtocol.h"

#include <atomic>
#include <memory>
#include <vector>

class PureSheenProcessor;

// The suite's end of the connection to Claude (owner: bridge). The processor of the suite owns one; the single-module
// plugins have none. Protocol: BridgeProtocol.h. Commands: BridgeCommands.cpp. The MCP side: tools/bridge/BridgeMain.cpp.
//
// Off until the user turns on "Allow Claude to connect" (the flag file every instance checks once a second). While on,
// a listener thread takes connections on 127.0.0.1 only, on a free port, and the registration file in the folder tells
// the helper the port and this instance's random token. A connection that does not start with the token is closed.
//
// Threading: the listener and the connections have threads of their own, which only read, parse and write. Every command
// runs on the message thread (queued, AsyncUpdater), like a click in the editor. Nothing here touches the audio thread.
// A connection gives up on a command the message thread has not reached in kCommandTimeoutMs (a busy host). The command
// is then dropped if it has not started, and the reply says which of the two happened. A command holds the processor's
// edit lock, so a host restoring a state from another thread cannot change things under it.
//
// The host deletes the processor on the message thread, the same assumption the processor's own timer rests on.
namespace psplug
{
class Bridge final : private juce::Timer, private juce::AsyncUpdater
{
public:
    static constexpr int kMaxClients = 4;
    static constexpr int kCommandTimeoutMs = 15000;
    static constexpr int kHelloTimeoutMs = 3000;

    explicit Bridge (PureSheenProcessor&);
    ~Bridge() override;

    // The user's switch, shared by every instance. An instance follows within a second.
    static bool isAllowed();
    static void setAllowed (bool);

    // The helper program, where the installer puts it (or next to the settings, for a copy from the manual zip). A file
    // that does not exist when it was not found.
    static juce::File findHelper();

    bool isListening() const noexcept { return listening.load(); }
    int getNumClients() const noexcept { return numClients.load(); }

    // Dev tool: this folder instead of the user's, and follow its flag from now on. Outside the dev tool the bridge
    // follows the user's folder from the start.
    void useFolder (const juce::File&);
    void pollNow() { timerCallback(); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::atomic<bool> listening { false };
    std::atomic<int> numClients { 0 };

    void timerCallback() override;
    void handleAsyncUpdate() override;
};

// BridgeCommands.cpp. Runs one command on the message thread; throws juce::String for an error in plain words.
juce::var runBridgeCommand (PureSheenProcessor&, const juce::String& command, const juce::var& args);
bool isBridgeCommand (const juce::String& command);
bool bridgeCommandChangesState (const juce::String& command);
juce::StringArray bridgeCommandNames();
} // namespace psplug
