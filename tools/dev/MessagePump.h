#pragma once

#include <juce_events/juce_events.h>

#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#else
namespace juce::detail
{
bool dispatchNextMessageOnSystemQueue (bool returnIfNoPendingMessages);
}
#endif

// The dev tools run the message thread by hand: they call this between steps so async callbacks, timers and modal
// callbacks run. JUCE's modal loops are off (JUCE_MODAL_LOOPS_PERMITTED 0), so runDispatchLoopUntil is not built.
// On Windows and Linux one message is taken from the system queue at a time; on macOS JUCE posts its messages to the
// main run loop, so the run loop is run in short slices.
namespace ps::dev
{
inline void pumpMessages (int milliseconds, int idleSleepMs = 1)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) milliseconds;

    while (juce::Time::getMillisecondCounter() < end)
    {
       #if JUCE_MAC
        juce::ignoreUnused (idleSleepMs);
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.001, true);
       #else
        if (! juce::detail::dispatchNextMessageOnSystemQueue (true))
            juce::Thread::sleep (idleSleepMs);
       #endif
    }
}
} // namespace ps::dev
