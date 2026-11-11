/**
 * @file mock_port.h
 * @brief Scripted atPort_t mock shared by the GoogleTest suites.
 *
 * Stands in for the modem: written bytes are captured, complete command
 * lines trigger an optional onCommand callback that can inject the modem's
 * reply, and time advances by 1 ms whenever the client yields so timeouts
 * elapse instantly in test time.
 *
 * The newest MockPort is also the clock: test_time.cpp points the code
 * under test's MILLIS() and DELAY_MS at its @c now.
 */
#ifndef MOCK_PORT_H
#define MOCK_PORT_H

#include "at_client.h"

#include <deque>
#include <functional>
#include <string>

class MockPort
{
public:
    MockPort()
    {
        port.userData = this;
        port.write = &MockPort::sWrite;
        port.read = &MockPort::sRead;
        port.yield = &MockPort::sYield;
        port.lock = nullptr;
        port.unlock = nullptr;
        active() = this;
    }

    ~MockPort()
    {
        if (active() == this)
        {
            active() = nullptr;
        }
    }

    MockPort(const MockPort &) = delete;
    MockPort &operator=(const MockPort &) = delete;

    /* The MockPort whose clock MILLIS() reads; NULL if there isn't one. */
    static MockPort *&active()
    {
        static MockPort *current = nullptr;
        return current;
    }

    /* Queue bytes the "modem" will send to the client. */
    void inject(const std::string &data)
    {
        rx.insert(rx.end(), data.begin(), data.end());
    }

    atPort_t port;
    std::deque<char> rx;
    std::string tx;      /* everything the client wrote */
    std::string txLine; /* current command line being assembled */
    uint32_t now = 0;
    bool failWrite = false;
    bool failRead = false;

    /* Invoked with each complete command line (terminator stripped). */
    std::function<void(const std::string &)> onCommand;

private:
    static int sWrite(void *ud, const void *data, size_t len)
    {
        MockPort *self = static_cast<MockPort *>(ud);
        const char *bytes = static_cast<const char *>(data);

        if (self->failWrite)
        {
            return -1;
        }
        self->tx.append(bytes, len);
        for (size_t i = 0; i < len; i++)
        {
            if (bytes[i] == '\r')
            {
                std::string line = self->txLine;
                self->txLine.clear();
                if (self->onCommand)
                {
                    self->onCommand(line);
                }
            }
            else
            {
                self->txLine.push_back(bytes[i]);
            }
        }
        return static_cast<int>(len);
    }

    static int sRead(void *ud, void *buf, size_t len)
    {
        MockPort *self = static_cast<MockPort *>(ud);
        char *out = static_cast<char *>(buf);
        size_t n = 0;

        if (self->failRead)
        {
            return -1;
        }
        while (n < len && !self->rx.empty())
        {
            out[n++] = self->rx.front();
            self->rx.pop_front();
        }
        return static_cast<int>(n);
    }

    static void sYield(void *ud)
    {
        /* Idle wait: advance fake time so timeouts elapse immediately. */
        static_cast<MockPort *>(ud)->now += 1;
    }
};

#endif /* MOCK_PORT_H */
