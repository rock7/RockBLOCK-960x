/**
 * @file test_at_client.cpp
 * @brief GoogleTest unit tests for the AT client core.
 *
 * A scripted mock port (mock_port.h) stands in for the modem: written bytes
 * are captured, complete command lines trigger an optional onCommand
 * callback that can inject the modem's reply, and time advances by 1 ms
 * whenever the client yields so timeouts elapse instantly in test time.
 */

#include "at_client.h"
#include "mock_port.h"

#include <gtest/gtest.h>

#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace
{
class AtClientTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_EQ(AT_OK, atClientInit(&client, &mock.port, lineBuf,
                                      sizeof(lineBuf)));
    }

    MockPort mock;
    atClient_t client;
    char lineBuf[256];
    char resp[512];
};

/* ------------------------------------------------------------------ */
/* Basic command / response                                            */
/* ------------------------------------------------------------------ */

TEST_F(AtClientTest, SimpleOk)
{
    mock.onCommand = [this](const std::string & cmd)
    {
        EXPECT_EQ("AT", cmd);
        mock.inject("\r\nOK\r\n");
    };

    EXPECT_EQ(AT_OK, atClientSendCmd(&client, "AT", nullptr, 0, 1000));
    EXPECT_EQ("AT\r", mock.tx);
}

TEST_F(AtClientTest, CapturesInformationLine)
{
    mock.onCommand = [this](const std::string &)
    {
        mock.inject("\r\n+CSQ: 17,99\r\n\r\nOK\r\n");
    };

    EXPECT_EQ(AT_OK, atClientSendCmd(&client, "AT+CSQ", resp, sizeof(resp), 1000));
    EXPECT_STREQ("+CSQ: 17,99", resp);
}

TEST_F(AtClientTest, CapturesMultipleLinesNewlineSeparated)
{
    mock.onCommand = [this](const std::string &)
    {
        mock.inject("\r\n+CGDCONT: 1,\"IP\",\"apn1\"\r\n"
                    "+CGDCONT: 2,\"IP\",\"apn2\"\r\nOK\r\n");
    };

    EXPECT_EQ(AT_OK, atClientSendCmd(&client, "AT+CGDCONT?", resp, sizeof(resp), 1000));
    EXPECT_STREQ("+CGDCONT: 1,\"IP\",\"apn1\"\n+CGDCONT: 2,\"IP\",\"apn2\"", resp);
}

TEST_F(AtClientTest, SuppressesCommandEcho)
{
    mock.onCommand = [this](const std::string &)
    {
        /* Echo on (ATE1 behaviour). */
        mock.inject("AT+CSQ\r\n+CSQ: 3,0\r\nOK\r\n");
    };

    EXPECT_EQ(AT_OK, atClientSendCmd(&client, "AT+CSQ", resp, sizeof(resp), 1000));
    EXPECT_STREQ("+CSQ: 3,0", resp);
}

TEST_F(AtClientTest, FormattedCommand)
{
    mock.onCommand = [this](const std::string & cmd)
    {
        EXPECT_EQ("AT+USOWR=0,4", cmd);
        mock.inject("\r\nOK\r\n");
    };

    EXPECT_EQ(AT_OK, atClientSendCmdFmt(&client, nullptr, 0, 1000,"AT+USOWR=%d,%d", 0, 4));
}

TEST_F(AtClientTest, ConnectIsSuccessfulFinal)
{
    mock.onCommand = [this](const std::string &)
    {
        mock.inject("\r\nCONNECT 115200\r\n");
    };

    EXPECT_EQ(AT_OK, atClientSendCmd(&client, "ATD*99#", resp, sizeof(resp), 1000));
    EXPECT_STREQ("CONNECT 115200", resp);
}

/* ------------------------------------------------------------------ */
/* Error finals                                                        */
/* ------------------------------------------------------------------ */

TEST_F(AtClientTest, PlainError)
{
    mock.onCommand = [this](const std::string &)
    {
        mock.inject("\r\nERROR\r\n");
    };

    EXPECT_EQ(AT_ERR_ERROR, atClientSendCmd(&client, "AT+BAD", nullptr, 0, 1000));
}

TEST_F(AtClientTest, CmeErrorWithCode)
{
    mock.onCommand = [this](const std::string &)
    {
        mock.inject("\r\n+CME ERROR: 100\r\n");
    };

    EXPECT_EQ(AT_ERR_CME, atClientSendCmd(&client, "AT+COPS=?", nullptr, 0, 1000));
    EXPECT_EQ(100, atClientLastErrorCode(&client));
}

TEST_F(AtClientTest, CmsErrorWithCode)
{
    mock.onCommand = [this](const std::string &)
    {
        mock.inject("\r\n+CMS ERROR: 305\r\n");
    };

    EXPECT_EQ(AT_ERR_CMS,atClientSendCmd(&client, "AT+CMGS=1", nullptr, 0, 1000));
    EXPECT_EQ(305, atClientLastErrorCode(&client));
}

TEST_F(AtClientTest, Timeout)
{
    /* Modem never answers. */
    EXPECT_EQ(AT_ERR_TIMEOUT,atClientSendCmd(&client, "AT", nullptr, 0, 500));
}

TEST_F(AtClientTest, TimeoutAcrossMillisWraparound)
{
    mock.now = UINT32_MAX - 100; /* expires ~100 ms after the tick wraps */
    EXPECT_EQ(AT_ERR_TIMEOUT,atClientSendCmd(&client, "AT", nullptr, 0, 500));
}

TEST_F(AtClientTest, WriteFailureReportsIoError)
{
    mock.failWrite = true;
    EXPECT_EQ(AT_ERR_IO, atClientSendCmd(&client, "AT", nullptr, 0, 1000));
}

TEST_F(AtClientTest, ReadFailureReportsIoError)
{
    mock.failRead = true;
    EXPECT_EQ(AT_ERR_IO, atClientSendCmd(&client, "AT", nullptr, 0, 1000));
}

TEST_F(AtClientTest, ResponseBufferOverflowIsReported)
{
    char small[8];

    mock.onCommand = [this](const std::string &)
    {
        mock.inject("\r\n+CGMR: 12.34.56\r\nOK\r\n");
    };

    EXPECT_EQ(AT_ERR_OVERFLOW, atClientSendCmd(&client, "AT+CGMR", small, sizeof(small), 1000));
    EXPECT_STREQ("+CGMR: ", small); /* truncated but NUL-terminated */
}

/* ------------------------------------------------------------------ */
/* URC handling                                                        */
/* ------------------------------------------------------------------ */

struct UrcCapture
{
    std::vector<std::string> lines;

    static void handler(const char *line, void *ud)
    {
        static_cast<UrcCapture *>(ud)->lines.push_back(line);
    }
};

TEST_F(AtClientTest, UrcDispatchedWhenIdle)
{
    UrcCapture urc;

    ASSERT_EQ(AT_OK, atClientRegisterUrc(&client, "+UUSORD:", UrcCapture::handler, &urc));
    mock.inject("\r\n+UUSORD: 0,42\r\n");
    atClientProcess(&client);

    ASSERT_EQ(1u, urc.lines.size());
    EXPECT_EQ("+UUSORD: 0,42", urc.lines[0]);
}

TEST_F(AtClientTest, UrcDuringCommandBypassesResponse)
{
    UrcCapture urc;

    ASSERT_EQ(AT_OK, atClientRegisterUrc(&client, "+UUSORD:", UrcCapture::handler, &urc));
    mock.onCommand = [this](const std::string &)
    {
        mock.inject("\r\n+UUSORD: 2,10\r\n+CSQ: 20,99\r\nOK\r\n");
    };

    EXPECT_EQ(AT_OK, atClientSendCmd(&client, "AT+CSQ", resp, sizeof(resp), 1000));
    EXPECT_STREQ("+CSQ: 20,99", resp);
    ASSERT_EQ(1u, urc.lines.size());
    EXPECT_EQ("+UUSORD: 2,10", urc.lines[0]);
}

TEST_F(AtClientTest, PendingUrcDispatchedBeforeNextCommand)
{
    UrcCapture urc;

    ASSERT_EQ(AT_OK, atClientRegisterUrc(&client, "+CREG:", UrcCapture::handler, &urc));
    /* URC arrives while idle; nobody calls process() before the command. */
    mock.inject("\r\n+CREG: 1\r\n");
    mock.onCommand = [this](const std::string &)
    {
        mock.inject("\r\nOK\r\n");
    };

    EXPECT_EQ(AT_OK, atClientSendCmd(&client, "AT", resp, sizeof(resp), 1000));
    EXPECT_STREQ("", resp);
    ASSERT_EQ(1u, urc.lines.size());
    EXPECT_EQ("+CREG: 1", urc.lines[0]);
}

TEST_F(AtClientTest, UnregisteredUrcGoesToLineHandlerWhenIdle)
{
    UrcCapture catchAll;

    atClientSetLineHandler(&client, UrcCapture::handler, &catchAll);
    mock.inject("\r\nRING\r\n");
    atClientProcess(&client);

    ASSERT_EQ(1u, catchAll.lines.size());
    EXPECT_EQ("RING", catchAll.lines[0]);
}

TEST_F(AtClientTest, UnregisterUrcStopsDispatch)
{
    UrcCapture urc;

    ASSERT_EQ(AT_OK, atClientRegisterUrc(&client, "+CREG:", UrcCapture::handler, &urc));
    atClientUnregisterUrc(&client, "+CREG:");
    mock.inject("\r\n+CREG: 1\r\n");
    atClientProcess(&client);

    EXPECT_TRUE(urc.lines.empty());
}

TEST_F(AtClientTest, UrcTableFullReturnsOverflow)
{
    static const char *prefixes[] = {"+A:", "+B:", "+C:", "+D:",
                                     "+E:", "+F:", "+G:", "+H:"};
    UrcCapture urc;

    for (const char *p : prefixes)
    {
        ASSERT_EQ(AT_OK, atClientRegisterUrc(&client, p, UrcCapture::handler, &urc));
    }
    EXPECT_EQ(AT_ERR_OVERFLOW, atClientRegisterUrc(&client, "+I:", UrcCapture::handler, &urc));
}

/* ------------------------------------------------------------------ */
/* Debug trace handler                                                 */
/* ------------------------------------------------------------------ */

struct DebugCapture
{
    std::vector<std::pair<atDebugDir_t, std::string>> events;

    static void handler(atDebugDir_t dir, const char *data, size_t len, void *ud)
    {
        static_cast<DebugCapture *>(ud)->events.emplace_back(dir, std::string(data, len));
    }
};

TEST_F(AtClientTest, DebugHandlerSeesCommandAndResponse)
{
    DebugCapture dbg;

    atClientSetDebugHandler(&client, DebugCapture::handler, &dbg);
    mock.onCommand = [this](const std::string &)
    {
        mock.inject("\r\n+CSQ: 17,99\r\nOK\r\n");
    };

    EXPECT_EQ(AT_OK, atClientSendCmd(&client, "AT+CSQ", resp, sizeof(resp), 1000));

    ASSERT_EQ(3u, dbg.events.size());
    EXPECT_EQ(AT_DEBUG_TX, dbg.events[0].first);
    EXPECT_EQ("AT+CSQ", dbg.events[0].second);
    EXPECT_EQ(AT_DEBUG_RX, dbg.events[1].first);
    EXPECT_EQ("+CSQ: 17,99", dbg.events[1].second);
    EXPECT_EQ(AT_DEBUG_RX, dbg.events[2].first);
    EXPECT_EQ("OK", dbg.events[2].second);
}

TEST_F(AtClientTest, DebugHandlerSeesUrcAndRawData)
{
    DebugCapture dbg;
    char buf[16];

    atClientSetDebugHandler(&client, DebugCapture::handler, &dbg);

    mock.inject("\r\n+UUSORD: 0,3\r\n");
    atClientProcess(&client);

    EXPECT_EQ(3, atClientWriteRaw(&client, "abc", 3));
    mock.inject("xyz");
    EXPECT_EQ(3, atClientReadRaw(&client, buf, sizeof(buf), 100));

    ASSERT_EQ(3u, dbg.events.size());
    EXPECT_EQ(AT_DEBUG_RX, dbg.events[0].first);
    EXPECT_EQ("+UUSORD: 0,3", dbg.events[0].second);
    EXPECT_EQ(AT_DEBUG_TX, dbg.events[1].first);
    EXPECT_EQ("abc", dbg.events[1].second);
    EXPECT_EQ(AT_DEBUG_RX, dbg.events[2].first);
    EXPECT_EQ("xyz", dbg.events[2].second);
}

TEST_F(AtClientTest, DebugHandlerCanBeDisabled)
{
    DebugCapture dbg;

    atClientSetDebugHandler(&client, DebugCapture::handler, &dbg);
    atClientSetDebugHandler(&client, nullptr, nullptr);
    mock.onCommand = [this](const std::string &)
    {
        mock.inject("\r\nOK\r\n");
    };

    EXPECT_EQ(AT_OK, atClientSendCmd(&client, "AT", nullptr, 0, 1000));
    EXPECT_TRUE(dbg.events.empty());
}

/* ------------------------------------------------------------------ */
/* Prompt / raw data flow (SMS, socket writes)                         */
/* ------------------------------------------------------------------ */

TEST_F(AtClientTest, PromptPayloadFinalFlow)
{
    /* AT+CMGS style: command, "> " prompt, payload + Ctrl-Z, final. */
    mock.inject("\r\n> ");

    EXPECT_EQ(AT_OK, atClientWaitFor(&client, "> ", 1000));

    const char payload[] = "hello world\x1a";
    EXPECT_EQ(static_cast<int>(sizeof(payload) - 1), atClientWriteRaw(&client, payload, sizeof(payload) - 1));

    mock.inject("\r\n+CMGS: 5\r\n\r\nOK\r\n");
    EXPECT_EQ(AT_OK, atClientWaitFinal(&client, resp, sizeof(resp), 1000));
    EXPECT_STREQ("+CMGS: 5", resp);
    EXPECT_NE(std::string::npos, mock.tx.find("hello world\x1a"));
}

TEST_F(AtClientTest, WaitForTimesOutWithoutPattern)
{
    mock.inject("nothing to see");
    EXPECT_EQ(AT_ERR_TIMEOUT, atClientWaitFor(&client, "> ", 200));
}

TEST_F(AtClientTest, ReadRawReturnsAvailableBytes)
{
    char buf[16];

    mock.inject("abcdef");
    int n = atClientReadRaw(&client, buf, sizeof(buf), 100);
    ASSERT_EQ(6, n);
    EXPECT_EQ(0, memcmp(buf, "abcdef", 6));
}

TEST_F(AtClientTest, ReadRawTimesOutWhenSilent)
{
    char buf[16];

    EXPECT_EQ(0, atClientReadRaw(&client, buf, sizeof(buf), 100));
}

/* ------------------------------------------------------------------ */
/* Guards and parameter checks                                         */
/* ------------------------------------------------------------------ */

static void reentrantHandler(const char *line, void *ud)
{
    (void)line;
    atClient_t *c = static_cast<atClient_t *>(ud);
    /* Sending from a URC handler while a command is in flight must fail
     * cleanly rather than corrupt the client state. */
    EXPECT_EQ(AT_ERR_BUSY, atClientSendCmd(c, "AT", nullptr, 0, 100));
}

TEST_F(AtClientTest, NestedSendRejectedWithBusy)
{
    ASSERT_EQ(AT_OK, atClientRegisterUrc(&client, "+EVT:",reentrantHandler, &client));
    mock.onCommand = [this](const std::string &)
    {
        mock.inject("\r\n+EVT: 1\r\nOK\r\n");
    };

    EXPECT_EQ(AT_OK, atClientSendCmd(&client, "AT", nullptr, 0, 1000));
}

TEST_F(AtClientTest, InitRejectsBadArguments)
{
    atClient_t c;
    char buf[64];
    atPort_t bad = mock.port;

    EXPECT_EQ(AT_ERR_PARAM, atClientInit(nullptr, &mock.port, buf, sizeof(buf)));
    EXPECT_EQ(AT_ERR_PARAM, atClientInit(&c, nullptr, buf, sizeof(buf)));
    EXPECT_EQ(AT_ERR_PARAM, atClientInit(&c, &mock.port, nullptr, sizeof(buf)));
    EXPECT_EQ(AT_ERR_PARAM, atClientInit(&c, &mock.port, buf, 4));

    bad.read = nullptr;
    EXPECT_EQ(AT_ERR_PARAM, atClientInit(&c, &bad, buf, sizeof(buf)));
}

TEST_F(AtClientTest, OversizedReceivedLineIsTruncatedSafely)
{
    /* Line longer than the 256-byte line buffer must not crash and the
     * command must still complete. */
    std::string longline(1000, 'x');

    mock.onCommand = [ &, this](const std::string &)
    {
        mock.inject("\r\n" + longline + "\r\nOK\r\n");
    };

    EXPECT_EQ(AT_OK, atClientSendCmd(&client, "AT+CGMR", resp, sizeof(resp), 1000));
    EXPECT_EQ(sizeof(lineBuf) - 1, strlen(resp));
}


/* ------------------------------------------------------------------ */
/* Asynchronous commands                                               */
/* ------------------------------------------------------------------ */

TEST_F(AtClientTest, AsyncCmdPendingUntilFinalArrives)
{
    ASSERT_EQ(AT_OK, atClientSendCmdAsync(&client, "AT+SBDIX", resp, sizeof(resp), 1000));
    EXPECT_EQ("AT+SBDIX\r", mock.tx);

    EXPECT_EQ(AT_PENDING, atClientPollCmd(&client));

    mock.inject("\r\n+SBDIX: 0, 12, 0, 0, 0, 0\r\n");
    EXPECT_EQ(AT_PENDING, atClientPollCmd(&client));

    mock.inject("\r\nOK\r\n");
    EXPECT_EQ(AT_OK, atClientPollCmd(&client));
    EXPECT_STREQ("+SBDIX: 0, 12, 0, 0, 0, 0", resp);

    /* The client is idle again. */
    EXPECT_EQ(AT_ERR_PARAM, atClientPollCmd(&client));
    mock.onCommand = [this](const std::string &)
    {
        mock.inject("\r\nOK\r\n");
    };
    EXPECT_EQ(AT_OK, atClientSendCmd(&client, "AT", nullptr, 0, 1000));
}

TEST_F(AtClientTest, AsyncCmdReportsErrorFinal)
{
    ASSERT_EQ(AT_OK, atClientSendCmdAsync(&client, "AT+SBDIX", resp, sizeof(resp), 1000));
    mock.inject("\r\nERROR\r\n");
    EXPECT_EQ(AT_ERR_ERROR, atClientPollCmd(&client));
}

TEST_F(AtClientTest, AsyncCmdTimesOut)
{
    ASSERT_EQ(AT_OK, atClientSendCmdAsync(&client, "AT+SBDIX", resp, sizeof(resp), 1000));

    mock.now += 999;
    EXPECT_EQ(AT_PENDING, atClientPollCmd(&client));

    mock.now += 2;
    EXPECT_EQ(AT_ERR_TIMEOUT, atClientPollCmd(&client));
    EXPECT_EQ(AT_ERR_PARAM, atClientPollCmd(&client));
}

TEST_F(AtClientTest, AsyncCmdMakesClientBusy)
{
    ASSERT_EQ(AT_OK, atClientSendCmdAsync(&client, "AT+SBDIX", resp, sizeof(resp), 1000));

    EXPECT_EQ(AT_ERR_BUSY, atClientSendCmd(&client, "AT", nullptr, 0, 100));
    EXPECT_EQ(AT_ERR_BUSY, atClientSendCmdAsync(&client, "AT", nullptr, 0, 100));
    EXPECT_EQ(AT_ERR_BUSY, atClientWaitFinal(&client, nullptr, 0, 100));
    EXPECT_EQ("AT+SBDIX\r", mock.tx);
}

TEST_F(AtClientTest, AsyncCmdWriteFailureLeavesClientIdle)
{
    mock.failWrite = true;
    EXPECT_EQ(AT_ERR_IO, atClientSendCmdAsync(&client, "AT+SBDIX", resp, sizeof(resp), 1000));
    EXPECT_EQ(AT_ERR_PARAM, atClientPollCmd(&client));

    mock.failWrite = false;
    mock.onCommand = [this](const std::string &)
    {
        mock.inject("\r\nOK\r\n");
    };
    EXPECT_EQ(AT_OK, atClientSendCmd(&client, "AT", nullptr, 0, 1000));
}

TEST_F(AtClientTest, AsyncCmdRejectsBadArguments)
{
    EXPECT_EQ(AT_ERR_PARAM, atClientSendCmdAsync(nullptr, "AT", nullptr, 0, 1000));
    EXPECT_EQ(AT_ERR_PARAM, atClientSendCmdAsync(&client, nullptr, nullptr, 0, 1000));
    EXPECT_EQ(AT_ERR_PARAM, atClientPollCmd(nullptr));
    EXPECT_EQ(AT_ERR_PARAM, atClientPollCmd(&client));
}

TEST_F(AtClientTest, UrcDispatchedWhilePollingAsyncCmd)
{
    UrcCapture urc;

    ASSERT_EQ(AT_OK, atClientRegisterUrc(&client, "+CIEV:", UrcCapture::handler, &urc));
    ASSERT_EQ(AT_OK, atClientSendCmdAsync(&client, "AT+SBDIX", resp, sizeof(resp), 1000));

    mock.inject("\r\n+CIEV:1,1\r\n");
    EXPECT_EQ(AT_PENDING, atClientPollCmd(&client));
    ASSERT_EQ(1u, urc.lines.size());
    EXPECT_EQ("+CIEV:1,1", urc.lines[0]);

    mock.inject("\r\n+SBDIX: 0, 12, 0, 0, 0, 0\r\nOK\r\n");
    EXPECT_EQ(AT_OK, atClientPollCmd(&client));
    EXPECT_STREQ("+SBDIX: 0, 12, 0, 0, 0, 0", resp);
}

} /* namespace */
