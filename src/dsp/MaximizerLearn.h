#pragma once

#include <atomic>
#include <cstdint>

namespace ps::maxi
{
// One publication contains the request generation, state and progress. In particular, an audio callback belonging
// to an old request cannot finish (or publish progress for) a request that the message thread has just restarted.
class LearnCaptureMailbox
{
public:
    using Token = std::uint64_t;
    enum State : unsigned { Idle = 0, Listening = 1, Captured = 2 };

    static_assert (std::atomic<Token>::is_always_lock_free, "Learn publication must be realtime safe");

    Token load() const noexcept { return value.load (std::memory_order_acquire); }
    static State state (Token v) noexcept { return (State) (v & 3u); }
    static unsigned generation (Token v) noexcept { return (unsigned) (v >> 32); }
    static unsigned count (Token v) noexcept { return (unsigned) v >> 2; }

    // Message thread only. Audio publication never changes the generation.
    void start() noexcept { request (Listening); }
    void cancel() noexcept { request (Idle); }

    bool publish (Token expected, unsigned samples, State next) noexcept
    {
        const Token desired = (expected & 0xffffffff00000000ull) | ((Token) samples << 2) | next;
        return value.compare_exchange_strong (expected, desired, std::memory_order_acq_rel);
    }

private:
    void request (State next) noexcept
    {
        const unsigned gen = generation (value.load (std::memory_order_relaxed)) + 1u;
        value.store (((Token) gen << 32) | next, std::memory_order_release);
    }

    std::atomic<Token> value { 0 };
};
} // namespace ps::maxi
