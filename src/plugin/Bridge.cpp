#include "plugin/Bridge.h"

#include "plugin/PluginProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <deque>
#include <mutex>
#include <random>

#if JUCE_WINDOWS
 #include <process.h>
#endif

namespace psplug
{
namespace
{
juce::String randomHex (int bytes)
{
    std::random_device device;
    juce::String out;
    for (int i = 0; i < bytes; ++i)
        out << juce::String::toHexString ((int) (device() & 0xff)).paddedLeft ('0', 2);
    return out;
}

// Takes as long for a wrong token as for a right one.
bool sameToken (const juce::String& a, const juce::String& b)
{
    const auto x = a.toStdString(), y = b.toStdString();
    unsigned difference = (unsigned) (x.size() ^ y.size());
    for (size_t i = 0; i < x.size() && i < y.size(); ++i)
        difference |= (unsigned) (x[i] ^ y[i]);
    return difference == 0;
}

int currentProcessId()
{
   #if JUCE_WINDOWS
    return (int) _getpid();
   #else
    return 0;
   #endif
}

struct Request
{
    enum State { queued, running, cancelled };

    juce::String command;
    juce::var args;
    juce::var result;
    juce::String error;
    bool failed = false;
    std::atomic<int> state { queued };   // queued -> running (message thread) or cancelled (the connection gave up)
    juce::WaitableEvent done;
};
} // namespace

//==============================================================================
struct Bridge::Impl
{
    struct Client;
    struct Listener;

    Impl (Bridge& b, PureSheenProcessor& p) : bridge (b), processor (p) {}

    Bridge& bridge;
    PureSheenProcessor& processor;
    juce::File root = bridge::folder();
    juce::File registration;
    juce::String instanceId, token;

    juce::StreamingSocket listenSocket;
    std::unique_ptr<Listener> listener;

    std::mutex queueMutex;
    std::deque<std::shared_ptr<Request>> queue;
    int lastReportedClients = 0;

    bool start();
    void stop();
    void enqueue (const std::shared_ptr<Request>&);
};

//==============================================================================
struct Bridge::Impl::Client final : juce::Thread
{
    Client (Impl& o, std::unique_ptr<juce::StreamingSocket> s)
        : juce::Thread ("PureSheen bridge client"), owner (o), socket (std::move (s)) {}

    ~Client() override
    {
        stopThread (2000);   // every wait in run() is short and checks for this
        socket->close();
    }

    bool writeLine (const juce::var& message)
    {
        const auto text = juce::JSON::toString (message, true) + "\n";
        const char* data = text.toRawUTF8();
        int left = (int) text.getNumBytesAsUTF8();
        while (left > 0 && ! threadShouldExit())
        {
            const int ready = socket->waitUntilReady (false, 100);
            if (ready < 0)
                return false;
            if (ready == 0)
                continue;
            const int written = socket->write (data, left);
            if (written <= 0)
                return false;
            data += written;
            left -= written;
        }
        return left == 0;
    }

    bool hello (const juce::String& line)
    {
        const auto message = juce::JSON::parse (line);
        if (! message.isObject() || (int) message["protocol"] != bridge::kProtocolVersion
            || ! sameToken (message["token"].toString(), owner.token))
            return false;

        auto reply = std::make_unique<juce::DynamicObject>();
        reply->setProperty ("ok", true);
        reply->setProperty ("id", owner.instanceId);
        return writeLine (juce::var (reply.release()));
    }

    bool command (const juce::String& line)
    {
        const auto message = juce::JSON::parse (line);
        if (! message.isObject())
            return false;

        auto request = std::make_shared<Request>();
        request->command = message["cmd"].toString();
        request->args = message["args"];
        owner.enqueue (request);

        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) kCommandTimeoutMs;
        bool answered = false;
        while (! threadShouldExit() && ! (answered = request->done.wait (100)))
            if (juce::Time::getMillisecondCounter() >= deadline)
                break;

        auto reply = std::make_unique<juce::DynamicObject>();
        reply->setProperty ("id", message["id"]);
        if (! answered)
        {
            int expected = Request::queued;
            const bool neverStarted = request->state.compare_exchange_strong (expected, Request::cancelled);
            reply->setProperty ("ok", false);
            reply->setProperty ("error", neverStarted
                ? "The host is busy (a dialog, a render), so nothing was done. It is safe to try again."
                : "The host is slow to finish this, and it may still take effect. Check get_history before repeating it.");
        }
        else if (request->failed)
        {
            reply->setProperty ("ok", false);
            reply->setProperty ("error", request->error);
        }
        else
        {
            reply->setProperty ("ok", true);
            reply->setProperty ("result", request->result);
        }
        return writeLine (juce::var (reply.release()));
    }

    void run() override
    {
        juce::MemoryBlock pending;
        char chunk[4096];
        bool authenticated = false;
        const auto helloDeadline = juce::Time::getMillisecondCounter() + (juce::uint32) kHelloTimeoutMs;

        while (! threadShouldExit())
        {
            if (! authenticated && juce::Time::getMillisecondCounter() >= helloDeadline)
                break;

            const int ready = socket->waitUntilReady (true, 100);
            if (ready < 0)
                break;
            if (ready == 0)
                continue;

            const int got = socket->read (chunk, (int) sizeof (chunk), false);
            if (got <= 0)
                break;
            pending.append (chunk, (size_t) got);

            bool keepGoing = true;
            while (keepGoing)
            {
                const auto* bytes = static_cast<const char*> (pending.getData());
                const int size = (int) pending.getSize();
                int end = -1;
                for (int i = 0; i < size; ++i)
                    if (bytes[i] == '\n')
                    {
                        end = i;
                        break;
                    }
                if (end < 0)
                    break;

                // Limits first: nothing is parsed that could hurt the host, and before the token nothing long is read.
                if (end > (authenticated ? bridge::kMaxLineBytes : bridge::kMaxHelloBytes)
                    || ! bridge::jsonNestingIsSafe (bytes, (size_t) end))
                {
                    keepGoing = false;
                    break;
                }

                const auto line = juce::String::fromUTF8 (bytes, end).trim();
                pending.removeSection (0, (size_t) end + 1);
                if (line.isEmpty())
                    continue;

                keepGoing = authenticated ? command (line) : (authenticated = hello (line));
            }
            if (! keepGoing || (int) pending.getSize() > (authenticated ? bridge::kMaxLineBytes : bridge::kMaxHelloBytes))
                break;
        }

        finished.store (true);   // the listener collects it
    }

    Impl& owner;
    std::unique_ptr<juce::StreamingSocket> socket;
    std::atomic<bool> finished { false };
};

//==============================================================================
struct Bridge::Impl::Listener final : juce::Thread
{
    explicit Listener (Impl& o) : juce::Thread ("PureSheen bridge"), owner (o) {}

    ~Listener() override
    {
        stopThread (2000);   // the wait in run() is short and checks for this
        owner.listenSocket.close();
        clients.clear();
    }

    void reap()
    {
        for (auto it = clients.begin(); it != clients.end();)
            it = (*it)->finished.load() ? clients.erase (it) : std::next (it);
        if (owner.bridge.numClients.exchange ((int) clients.size()) != (int) clients.size())
            owner.bridge.triggerAsyncUpdate();   // the editor says so
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            const int ready = owner.listenSocket.waitUntilReady (true, 200);
            reap();
            if (ready < 0)
                break;
            if (ready == 0)
                continue;

            std::unique_ptr<juce::StreamingSocket> socket (owner.listenSocket.waitForNextConnection());
            if (socket == nullptr || threadShouldExit())
                continue;
            if ((int) clients.size() >= kMaxClients)
                continue;   // closes it

            auto client = std::make_unique<Client> (owner, std::move (socket));
            client->startThread();
            clients.push_back (std::move (client));
            reap();
        }
    }

    Impl& owner;
    std::vector<std::unique_ptr<Client>> clients;
};

//==============================================================================
bool Bridge::Impl::start()
{
    if (! root.createDirectory())
        return false;
    if (! listenSocket.createListener (0, "127.0.0.1"))
        return false;

    instanceId = randomHex (4);
    token = randomHex (24);

    auto info = std::make_unique<juce::DynamicObject>();
    info->setProperty ("id", instanceId);
    info->setProperty ("port", listenSocket.getBoundPort());
    info->setProperty ("token", token);
    info->setProperty ("pid", currentProcessId());
    info->setProperty ("product", processor.getName());
   #ifdef PURESHEEN_VERSION
    info->setProperty ("version", PURESHEEN_VERSION);
   #endif

    registration = root.getChildFile (instanceId + ".json");
    if (! registration.replaceWithText (juce::JSON::toString (juce::var (info.release()), true)))
    {
        listenSocket.close();
        return false;
    }

    listener = std::make_unique<Listener> (*this);
    listener->startThread();
    return true;
}

void Bridge::Impl::stop()
{
    listener.reset();   // closes the socket and every client, joins the threads
    registration.deleteFile();
    registration = juce::File();

    std::deque<std::shared_ptr<Request>> left;
    {
        const std::lock_guard<std::mutex> lock (queueMutex);
        left.swap (queue);
    }
    for (auto& r : left)
    {
        r->failed = true;
        r->error = "PureSheen Dynamics closed the connection.";
        r->done.signal();
    }
    bridge.numClients.store (0);
}

void Bridge::Impl::enqueue (const std::shared_ptr<Request>& request)
{
    {
        const std::lock_guard<std::mutex> lock (queueMutex);
        queue.push_back (request);
    }
    bridge.triggerAsyncUpdate();
}

//==============================================================================
Bridge::Bridge (PureSheenProcessor& p) : impl (std::make_unique<Impl> (*this, p))
{
   #if ! PURESHEEN_DEV_TOOL
    startTimer (1000);   // not right away: a host's plugin scan creates and deletes instances
   #endif
}

Bridge::~Bridge()
{
    stopTimer();
    cancelPendingUpdate();
    impl->stop();
}

bool Bridge::isAllowed() { return bridge::allowedFlag (bridge::folder()).existsAsFile(); }

void Bridge::setAllowed (bool allowed)
{
    const auto flag = bridge::allowedFlag (bridge::folder());
    if (allowed)
    {
        flag.getParentDirectory().createDirectory();
        flag.create();
    }
    else
    {
        flag.deleteFile();
    }
}

juce::File Bridge::findHelper()
{
    // Windows: the installer's folder in Program Files. macOS: /Applications/PureSheen Dynamics, as the Mac download's
    // "How to install.txt" says. Either: next to the registration folder, for a helper copied by hand.
   #if JUCE_WINDOWS
    const juce::String name ("PureSheenBridge.exe");
   #else
    const juce::String name ("PureSheenBridge");
   #endif
    const auto installed = juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory)
                               .getChildFile ("PureSheen Dynamics")
                               .getChildFile (name);
    if (installed.existsAsFile())
        return installed;
    const auto copied = bridge::folder().getSiblingFile (name);
    return copied.existsAsFile() ? copied : installed;
}

void Bridge::useFolder (const juce::File& folderToUse)
{
    impl->stop();
    impl->root = folderToUse;
    listening.store (false);
    startTimer (1000);
    timerCallback();
}

void Bridge::timerCallback()
{
    const bool wanted = bridge::allowedFlag (impl->root).existsAsFile();
    if (wanted == listening.load())
        return;

    if (wanted)
    {
        listening.store (impl->start());
    }
    else
    {
        impl->stop();
        listening.store (false);
    }
    triggerAsyncUpdate();
}

void Bridge::handleAsyncUpdate()
{
    for (;;)
    {
        std::shared_ptr<Request> request;
        {
            const std::lock_guard<std::mutex> lock (impl->queueMutex);
            if (impl->queue.empty())
                break;
            request = impl->queue.front();
            impl->queue.pop_front();
        }

        int expected = Request::queued;
        if (! request->state.compare_exchange_strong (expected, Request::running))
            continue;   // the connection gave up on it, so it must not happen now

        auto& processor = impl->processor;
        const juce::ScopedLock editLock (processor.getEditLock());   // a host may restore a state from its own thread
        const bool changes = bridgeCommandChangesState (request->command);
        int number = 0;
        if (changes)
        {
            processor.finishUserEdits();   // the user's own edit in progress stays the user's undo step
            number = processor.beginClaudeRequest();
        }

        try
        {
            request->result = runBridgeCommand (processor, request->command, request->args);
            if (auto* reply = request->result.getDynamicObject(); reply != nullptr && changes)
                reply->setProperty ("request", number);   // get_history shows it on the step, even one that lands later
        }
        catch (const juce::String& message)
        {
            request->failed = true;
            request->error = message;
        }
        catch (const std::exception& e)
        {
            request->failed = true;
            request->error = juce::String ("Unexpected error: ") + e.what();
        }
        catch (...)
        {
            request->failed = true;
            request->error = "Unexpected error.";
        }

        if (changes)
            processor.endClaudeRequest();
        request->done.signal();
    }

    const int clients = numClients.load();
    if (clients != impl->lastReportedClients)
    {
        impl->lastReportedClients = clients;
        impl->processor.sendChangeMessage();   // the editor says so
    }
}
} // namespace psplug
