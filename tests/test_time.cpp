/**
 * @file test_time.cpp
 * @brief The clock behind test_time.h: the active MockPort's @c now.
 *
 * With no MockPort (tests that talk to a real pseudo-terminal), real time
 * is used, so timeouts there still expire.
 */

#include "test_time.h"
#include "mock_port.h"

#include <chrono>
#include <thread>

extern "C" void rb960xTestDelayMs(unsigned long ms)
{
    MockPort *mock = MockPort::active();

    if (mock != nullptr)
    {
        mock->now += static_cast<uint32_t>(ms);
    }
    else
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
}

extern "C" uint32_t rb960xTestMillis(void)
{
    MockPort *mock = MockPort::active();

    if (mock != nullptr)
    {
        return mock->now;
    }

    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
