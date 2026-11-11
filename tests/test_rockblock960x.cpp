/**
 * @file test_rockblock960x.cpp
 * @brief GoogleTest unit tests for the RockBLOCK-960x driver.
 *
 * Uses the same scripted mock port (mock_port.h) as the AT client tests.
 * The fixture adds a simple command -> reply table on top of it: every
 * command line the driver sends is recorded, and if the table has an entry
 * for it the scripted reply is injected. Commands without an entry get no
 * reply at all, so they time out (instantly, in test time).
 */

#include "rockblock960x.h"
#include "mock_port.h"

#include <gtest/gtest.h>
#include <functional>

#include <algorithm>
#include <deque>
#include <map>
#include <string>
#include <vector>

#ifdef RB960X_TEST_HAVE_PORT
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/select.h>
#include <thread>
#include <unistd.h>
#endif

namespace
{
const char *const REPLY_OK = "\r\nOK\r\n";
const char *const REPLY_ERROR = "\r\nERROR\r\n";

class Rb960xInitTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        mock.onCommand = [this](const std::string & cmd)
        {
            reply(cmd);
        };
    }

    /* Record the command and inject its scripted reply, if any. */
    void reply(const std::string &cmd)
    {
        commands.push_back(cmd);

        auto it = replies.find(cmd);
        if (it != replies.end())
        {
            mock.inject(it->second);
        }
    }

    /* Commands rb960xInit() sends before the model-specific ones. */
    void scriptCommon(const std::string &ati7)
    {
        replies["AT"] = REPLY_OK;
        replies["ATE0"] = REPLY_OK;
        replies["ATI7"] = "\r\n" + ati7 + "\r\n\r\nOK\r\n";
        replies["AT&D0"] = REPLY_OK;
        replies["AT&K0"] = REPLY_OK;
    }

    void scriptModem9603()
    {
        scriptCommon("BOOT07d4/9603NGrevA/04/RAW0f");
        replies["AT+CGSN"] = "\r\n300234010753370\r\n\r\nOK\r\n";
        replies["AT+GSN"] = "\r\n300234010753370\r\n\r\nOK\r\n";
        replies["AT+SBDMTA=1"] = REPLY_OK;
    }

    /* TODO: replace these replies with ones captured from a real 9604. */
    void scriptModem9604()
    {
        scriptCommon("9604");
        replies["AT+CGSN"] = "\r\n351234567890123\r\n\r\nOK\r\n";
        replies["AT+USATSN"] = "\r\n+USATSN: SN0001,300434060000000\r\n\r\nOK\r\n";
        replies["AT+URAT?"] = "\r\n+URAT: 10\r\n\r\nOK\r\n";
        replies["AT+URING=1"] = REPLY_OK;
    }

    rb960xStatus_t init()
    {
        return rb960xInit(&rb, &mock.port, nullptr, nullptr);
    }

    MockPort mock;
    rb960x_t rb;
    std::map<std::string, std::string> replies;
    std::vector<std::string> commands;
};

/* ------------------------------------------------------------------ */
/* Argument checks                                                     */
/* ------------------------------------------------------------------ */

TEST_F(Rb960xInitTest, RejectsNullArguments)
{
    EXPECT_EQ(RB960X_ERR_ARG, rb960xInit(nullptr, &mock.port, nullptr, nullptr));
    EXPECT_EQ(RB960X_ERR_ARG, rb960xInit(&rb, nullptr, nullptr, nullptr));
    EXPECT_TRUE(commands.empty());
}

/* ------------------------------------------------------------------ */
/* Happy paths                                                         */
/* ------------------------------------------------------------------ */

TEST_F(Rb960xInitTest, Identifies9603)
{
    scriptModem9603();

    ASSERT_EQ(RB960X_OK, init());

    const std::vector<std::string> expected =
    {
        "AT", "ATE0", "ATI7", "AT+CGSN", "AT+GSN", "AT&D0", "AT&K0", "AT+SBDMTA=1"
    };
    EXPECT_EQ(expected, commands);
    EXPECT_TRUE(rb.initialised);
    EXPECT_EQ(RB960X_MODEL_9603, rb.model);
    EXPECT_STREQ("300234010753370", rb.imei);
    EXPECT_STREQ("300234010753370", rb.serial);
}

TEST_F(Rb960xInitTest, Identifies9604)
{
    scriptModem9604();

    ASSERT_EQ(RB960X_OK, init());

    /* No AT+GSN (uses AT+USATSN), and ring alerts come from AT+URING
     * rather than AT+SBDMTA on the 9604. */
    const std::vector<std::string> expected =
    {
        "AT", "ATE0", "ATI7", "AT+CGSN", "AT+USATSN", "AT+URAT?", "AT&D0", "AT&K0", "AT+URING=1"
    };
    EXPECT_EQ(expected, commands);
    EXPECT_TRUE(rb.initialised);
    EXPECT_EQ(RB960X_MODEL_9604, rb.model);
    EXPECT_STREQ("351234567890123", rb.cellularImei);
    EXPECT_STREQ("300434060000000", rb.imei);
    EXPECT_STREQ("SN0001", rb.serial);
    EXPECT_EQ(RB960X_RAT_SBD, rb.ratMode);
}

/* ------------------------------------------------------------------ */
/* Probe retries                                                       */
/* ------------------------------------------------------------------ */

TEST_F(Rb960xInitTest, ProbeRetriedUntilModemAnswers)
{
    scriptModem9603();

    /* Modem still booting: ignores the first "AT". */
    int probes = 0;
    mock.onCommand = [this, &probes](const std::string & cmd)
    {
        if ((cmd == "AT") && (++probes == 1))
        {
            commands.push_back(cmd);
            return;
        }
        reply(cmd);
    };

    EXPECT_EQ(RB960X_OK, init());
    EXPECT_EQ(2, probes);
}

TEST_F(Rb960xInitTest, TimesOutWhenModemNeverAnswers)
{
    /* Nothing scripted: every probe goes unanswered. */
    EXPECT_EQ(RB960X_ERR_TIMEOUT, init());
    EXPECT_EQ(std::vector<std::string>(RB960X_INIT_ATTEMPTS, "AT"), commands);
    EXPECT_FALSE(rb.initialised);
}

/* ------------------------------------------------------------------ */
/* Failures part-way through                                           */
/* ------------------------------------------------------------------ */

TEST_F(Rb960xInitTest, ErrorReplyStopsInit)
{
    scriptModem9603();
    replies["ATI7"] = REPLY_ERROR;

    EXPECT_EQ(RB960X_ERR_PROTOCOL, init());
    EXPECT_EQ("ATI7", commands.back());
    EXPECT_FALSE(rb.initialised);
}


/* ------------------------------------------------------------------ */
/* Switch RAT                                                         */
/* ------------------------------------------------------------------ */

/* Same scripted modem as the init tests; init() brings rb up first. */
class Rb960xSwitchRatTest : public Rb960xInitTest
{
};

TEST_F(Rb960xSwitchRatTest, ErrorInvalidModel)
{
    scriptModem9603();
    ASSERT_EQ(RB960X_OK, init());
    commands.clear();

    EXPECT_EQ(RB960X_ERR_INVALID_MODEL, rb960xSwitchRat(&rb, RB960X_RAT_SBD));
    EXPECT_TRUE(commands.empty());
}


TEST_F(Rb960xSwitchRatTest, ErrorInvalidArg)
{
    scriptModem9604();
    ASSERT_EQ(RB960X_OK, init());
    commands.clear();

    EXPECT_EQ(RB960X_ERR_ARG, rb960xSwitchRat(NULL, RB960X_RAT_SBD));
    EXPECT_EQ(RB960X_ERR_ARG, rb960xSwitchRat(&rb, static_cast<rb960xRatMode_t>(100)));
    EXPECT_TRUE(commands.empty());
}

TEST_F(Rb960xSwitchRatTest, SuccessfulSwitchSbdToLte)
{
    scriptModem9604();
    ASSERT_EQ(RB960X_OK, init());
    EXPECT_EQ(RB960X_RAT_SBD, rb.ratMode);
    commands.clear();

    /* Starts in SBD (from init); the modem reports LTE once switched. */
    replies["AT+CFUN=0"] = REPLY_OK;
    replies["AT+URAT=7"] = REPLY_OK;
    replies["AT+CFUN=16"] = REPLY_OK;
    replies["AT+URAT?"] = "\r\n+URAT: 7\r\n\r\nOK\r\n";

    EXPECT_EQ(RB960X_OK, rb960xSwitchRat(&rb, RB960X_RAT_LTE_CAT_M1));

    const std::vector<std::string> expected =
    {
        "AT+CFUN=0", "AT+URAT=7", "AT+CFUN=16", "AT", "ATE0", "AT+URAT?",
        "AT&D0", "AT&K0", "AT+URING=1"
    };
    EXPECT_EQ(expected, commands);
    EXPECT_EQ(RB960X_RAT_LTE_CAT_M1, rb.ratMode);
}

TEST_F(Rb960xSwitchRatTest, SuccessfulSwitchLteToSbd)
{
    scriptModem9604();
    replies["AT+URAT?"] = "\r\n+URAT: 7\r\n\r\nOK\r\n";
    ASSERT_EQ(RB960X_OK, init());
    EXPECT_EQ(RB960X_RAT_LTE_CAT_M1, rb.ratMode);
    commands.clear();

    /* Starts in LTE (from init); the modem reports SBD once switched. */
    replies["AT+CFUN=0"] = REPLY_OK;
    replies["AT+URAT=10"] = REPLY_OK;
    replies["AT+CFUN=16"] = REPLY_OK;
    replies["AT+URAT?"] = "\r\n+URAT: 10\r\n\r\nOK\r\n";

    EXPECT_EQ(RB960X_OK, rb960xSwitchRat(&rb, RB960X_RAT_SBD));

    const std::vector<std::string> expected =
    {
        "AT+CFUN=0", "AT+URAT=10", "AT+CFUN=16", "AT", "ATE0", "AT+URAT?",
        "AT&D0", "AT&K0", "AT+URING=1"
    };
    EXPECT_EQ(expected, commands);
    EXPECT_EQ(RB960X_RAT_SBD, rb.ratMode);
}

TEST_F(Rb960xSwitchRatTest, ErrorModeUnchangedAfterReset)
{
    scriptModem9604();
    ASSERT_EQ(RB960X_OK, init());
    commands.clear();

    /* Every command works, but the module still reports SBD. */
    replies["AT+CFUN=0"] = REPLY_OK;
    replies["AT+URAT=7"] = REPLY_OK;
    replies["AT+CFUN=16"] = REPLY_OK;

    EXPECT_EQ(RB960X_ERR_PROTOCOL, rb960xSwitchRat(&rb, RB960X_RAT_LTE_CAT_M1));
    EXPECT_EQ(RB960X_RAT_SBD, rb.ratMode);
    EXPECT_EQ(commands.end(), std::find(commands.begin(), commands.end(), "AT&D0"));
}

TEST_F(Rb960xSwitchRatTest, ErrorUratTurnsRadioBackOn)
{
    scriptModem9604();
    ASSERT_EQ(RB960X_OK, init());
    commands.clear();

    replies["AT+CFUN=0"] = REPLY_OK;
    replies["AT+URAT=7"] = REPLY_ERROR;
    replies["AT+CFUN=1"] = REPLY_OK;

    EXPECT_EQ(RB960X_ERR_PROTOCOL, rb960xSwitchRat(&rb, RB960X_RAT_LTE_CAT_M1));
    const std::vector<std::string> expected = { "AT+CFUN=0", "AT+URAT=7", "AT+CFUN=1" };
    EXPECT_EQ(expected, commands);
    EXPECT_EQ(RB960X_RAT_SBD, rb.ratMode);
}

TEST_F(Rb960xSwitchRatTest, SuccessfulAlreadyInMode)
{
    /* Modem already in SDB when init reads AT+URAT?. */
    scriptModem9604();
    ASSERT_EQ(RB960X_OK, init());
    ASSERT_EQ(RB960X_RAT_SBD, rb.ratMode);
    commands.clear();

    /* Nothing to do: succeeds without touching the modem. */
    EXPECT_EQ(RB960X_OK, rb960xSwitchRat(&rb, RB960X_RAT_SBD));
    EXPECT_TRUE(commands.empty());
    EXPECT_EQ(RB960X_RAT_SBD, rb.ratMode);
}

/* ------------------------------------------------------------------ */
/* Cellular ICCID                                                     */
/* ------------------------------------------------------------------ */

/* Same scripted modem as the init tests; init() brings rb up first. */
class Rb960xGetCellularIccidTest : public Rb960xInitTest
{
};

TEST_F(Rb960xGetCellularIccidTest, ErrorInvalidArg)
{
    char iccid[RB960X_ICCID_LEN];
    scriptModem9604();
    replies["AT+URAT?"] = "\r\n+URAT: 7\r\n\r\nOK\r\n"; // Ensure it is in cellular mode
    ASSERT_EQ(RB960X_OK, init());
    EXPECT_EQ(RB960X_ERR_ARG, rb960xGetCellularIccid(NULL, iccid, sizeof(iccid)));
    EXPECT_EQ(RB960X_ERR_ARG, rb960xGetCellularIccid(&rb, NULL, 0));
}

TEST_F(Rb960xGetCellularIccidTest, IncorrectMode)
{
    char iccid[RB960X_ICCID_LEN];
    scriptModem9604();
    ASSERT_EQ(RB960X_OK, init());
    EXPECT_EQ(RB960X_ERR_WRONG_RAT, rb960xGetCellularIccid(&rb, iccid, sizeof(iccid)));
}

TEST_F(Rb960xGetCellularIccidTest, Success)
{
    char iccid[RB960X_ICCID_LEN];
    scriptModem9604();
    replies["AT+URAT?"] = "\r\n+URAT: 7\r\n\r\nOK\r\n"; // Ensure it is in cellular mode
    ASSERT_EQ(RB960X_OK, init());
    replies["AT+CCID"] = "\r\n+CCID: 8944107253918064278\r\n\r\nOK\r\n";
    EXPECT_EQ(RB960X_OK, rb960xGetCellularIccid(&rb, iccid, sizeof(iccid)));
    EXPECT_STREQ("8944107253918064278", iccid);
}


/* ------------------------------------------------------------------ */
/* Celluar FW Version                                                 */
/* ------------------------------------------------------------------ */

/* Same scripted modem as the init tests; init() brings rb up first. */
class Rb960xGetCellularVersionTest : public Rb960xInitTest
{
};

TEST_F(Rb960xGetCellularVersionTest, ErrorInvalidArg)
{
    char version[RB960X_MODEM_VERSION_LEN];
    scriptModem9604();
    ASSERT_EQ(RB960X_OK, init());
    EXPECT_EQ(RB960X_ERR_ARG, rb960xGetCellularVersion(NULL, version, sizeof(version)));
    EXPECT_EQ(RB960X_ERR_ARG, rb960xGetCellularVersion(&rb, NULL, 0));
}

TEST_F(Rb960xGetCellularVersionTest, ErrorInvalidModel)
{
    char version[RB960X_MODEM_VERSION_LEN];
    scriptModem9603();
    ASSERT_EQ(RB960X_OK, init());
    EXPECT_EQ(RB960X_ERR_INVALID_MODEL, rb960xGetCellularVersion(&rb, version, sizeof(version)));
}

TEST_F(Rb960xGetCellularVersionTest, Success)
{
    char version[RB960X_MODEM_VERSION_LEN];
    scriptModem9604();
    ASSERT_EQ(RB960X_OK, init());
    replies["ATI9"] = "\r\n07.01.A00.01\r\n\r\nOK\r\n";
    EXPECT_EQ(RB960X_OK, rb960xGetCellularVersion(&rb, version, sizeof(version)));
    EXPECT_STREQ("07.01.A00.01", version);
}

TEST_F(Rb960xGetCellularVersionTest, SuccessTruncation)
{
    char version[3];
    scriptModem9604();
    ASSERT_EQ(RB960X_OK, init());
    replies["ATI9"] = "\r\n07.01.A00.01\r\n\r\nOK\r\n";
    EXPECT_EQ(RB960X_OK, rb960xGetCellularVersion(&rb, version, sizeof(version)));
    EXPECT_STREQ("07", version);
}

/* ------------------------------------------------------------------ */
/* Satellite FW Version                                               */
/* ------------------------------------------------------------------ */

/* Same scripted modem as the init tests; init() brings rb up first. */
class Rb960xGetSatelliteVersionTest : public Rb960xInitTest
{
};

TEST_F(Rb960xGetSatelliteVersionTest, ErrorInvalidArg)
{
    char version[RB960X_MODEM_VERSION_LEN];
    scriptModem9604();
    ASSERT_EQ(RB960X_OK, init());
    EXPECT_EQ(RB960X_ERR_ARG, rb960xGetSatelliteVersion(NULL, version, sizeof(version)));
    EXPECT_EQ(RB960X_ERR_ARG, rb960xGetSatelliteVersion(&rb, NULL, 0));
}

TEST_F(Rb960xGetSatelliteVersionTest, ErrorInvalidModel)
{
    char version[RB960X_MODEM_VERSION_LEN];
    scriptModem9603();
    replies["ATI3"] = "\r\nTA21004\r\n\r\nOK\r\n";
    ASSERT_EQ(RB960X_OK, init());
    EXPECT_EQ(RB960X_OK, rb960xGetSatelliteVersion(&rb, version, sizeof(version)));
    EXPECT_STREQ("TA21004", version);
}

TEST_F(Rb960xGetSatelliteVersionTest, ErrorWrongRat)
{
    char version[RB960X_MODEM_VERSION_LEN];
    scriptModem9604();
    replies["AT+URAT?"] = "\r\n+URAT: 7\r\n\r\nOK\r\n"; // Ensure it is in cellular mode
    ASSERT_EQ(RB960X_OK, init());
    EXPECT_EQ(RB960X_ERR_WRONG_RAT, rb960xGetSatelliteVersion(&rb, version, sizeof(version)));
}

TEST_F(Rb960xGetSatelliteVersionTest, Success)
{
    char version[RB960X_MODEM_VERSION_LEN];
    scriptModem9604();
    ASSERT_EQ(RB960X_OK, init());
    replies["AT+USATINFO"] = "\r\n1,SBD,03.00.10\r\n\r\nOK\r\n";
    EXPECT_EQ(RB960X_OK, rb960xGetSatelliteVersion(&rb, version, sizeof(version)));
    EXPECT_STREQ("03.00.10", version);
}

TEST_F(Rb960xGetSatelliteVersionTest, SuccessTruncation)
{
    char version[3];
    scriptModem9604();
    ASSERT_EQ(RB960X_OK, init());
    replies["AT+USATINFO"] = "\r\n1,SBD,03.00.10\r\n\r\nOK\r\n";
    EXPECT_EQ(RB960X_OK, rb960xGetSatelliteVersion(&rb, version, sizeof(version)));
    EXPECT_STREQ("03", version);
}

/* ------------------------------------------------------------------ */
/* SBD session API: shared fixture                                     */
/* ------------------------------------------------------------------ */

const char *const SBDWB_READY_OK = "\r\nREADY\r\n\r\n0\r\n\r\nOK\r\n";
const char *const SBDIX_SENT = "\r\n+SBDIX: 0, 12, 0, 0, 0, 0\r\n\r\nOK\r\n";

class Rb960xSbdTest : public Rb960xInitTest
{
protected:
    void SetUp() override
    {
        /* The payload and checksum written after "READY" have no '\r', so
         * the mock joins them onto the start of the next command line.
         * Keep only the part from the first "AT" onwards (the test payload
         * contains no "AT"). */
        mock.onCommand = [this](const std::string & line)
        {
            const size_t at = line.find("AT");
            reply((at == std::string::npos) ? line : line.substr(at));
        };

        /* Sent before every session. */
        replies[SBDST_CMD] = REPLY_OK;
    }

    /* Index of the first occurrence of cmd in commands, or -1. */
    long indexOf(const std::string &cmd) const
    {
        for (size_t i = 0; i < commands.size(); i++)
        {
            if (commands[i] == cmd)
            {
                return (long)i;
            }
        }
        return -1;
    }

    long count(const std::string &cmd) const
    {
        long n = 0;
        for (const std::string &c : commands)
        {
            n += (c == cmd) ? 1 : 0;
        }
        return n;
    }

    bool sent(const std::string &cmd) const
    {
        return indexOf(cmd) != -1;
    }

    void init9603()
    {
        scriptModem9603();
        ASSERT_EQ(RB960X_OK, init());
    }

    void init9604()
    {
        scriptModem9604();
        ASSERT_EQ(RB960X_OK, init());
    }

    static std::string csq(const std::string &value)
    {
        return "\r\n+CSQ:" + value + "\r\n\r\nOK\r\n";
    }

    static std::string csq(int bars)
    {
        return csq(std::to_string(bars));
    }

    static std::string sbdix(int mo, int mt = 0, int mtLength = 0, int queued = 0)
    {
        return "\r\n+SBDIX: " + std::to_string(mo) + ", 12, " + std::to_string(mt) + ", 3, " +
               std::to_string(mtLength) + ", " + std::to_string(queued) + "\r\n\r\nOK\r\n";
    }

    /* AT+SBDRB reply: big-endian length, message, big-endian checksum, OK. */
    static std::string sbdrb(const std::string &msg, int checksumAdjust = 0)
    {
        unsigned int sum = 0;
        for (unsigned char c : msg)
        {
            sum += c;
        }
        sum = (sum + (unsigned int)checksumAdjust) & 0xFFFFu;

        std::string out;
        out += (char)((msg.size() >> 8) & 0xFF);
        out += (char)(msg.size() & 0xFF);
        out += msg;
        out += (char)((sum >> 8) & 0xFF);
        out += (char)(sum & 0xFF);
        return out + "\r\nOK\r\n";
    }

    /* AT+SBDR reply (9604): +SBDR:<length>,"<hex>", OK. */
    static std::string sbdr(const std::string &msg, bool lower = false)
    {
        static const char upperDigits[] = "0123456789ABCDEF";
        static const char lowerDigits[] = "0123456789abcdef";
        const char *digits = lower ? lowerDigits : upperDigits;
        std::string hex;
        for (unsigned char c : msg)
        {
            hex += digits[c >> 4];
            hex += digits[c & 0x0F];
        }
        return "\r\n+SBDR:" + std::to_string(msg.size()) + ",\"" + hex + "\"\r\n\r\nOK\r\n";
    }

    rb960xStatus_t readMt()
    {
        return rb960xReadMtBuffer(&rb, buf, sizeof(buf), &length);
    }

    const std::string SBDST_CMD = "AT+SBDST=" + std::to_string(RB960X_SBD_SESSION_TIMEOUT_S);

    /* "Hello": byte sum 500 = 0x01F4. */
    const char *payload = "Hello";
    const size_t payloadLen = 5;

    rb960xSessionResult_t result;
    char buf[RB960X_MT_MAX];
    size_t length = 99;
};

/* Records every call to a session callback. */
struct SessionCapture
{
    std::vector<rb960xStatus_t> results;
    rb960x_t *rb = nullptr;
    void *userData = nullptr;
    bool hadSession = false;
    rb960xSessionResult_t session = {};

    static void callback(rb960x_t *rb, rb960xStatus_t status, const rb960xSessionResult_t *result, void *userData)
    {
        SessionCapture *self = static_cast<SessionCapture *>(userData);
        self->results.push_back(status);
        self->rb = rb;
        self->userData = userData;
        self->hadSession = (result != nullptr);
        if (result != nullptr)
        {
            self->session = *result;
        }
    }
};

/* Records every call to the signal callback. */
struct SignalCapture
{
    std::vector<rb960xStatus_t> statuses;
    std::vector<uint8_t> bars;
    void *userData = nullptr;

    static void callback(rb960x_t *rb, rb960xStatus_t status, uint8_t bars, void *userData)
    {
        SignalCapture *self = static_cast<SignalCapture *>(userData);
        (void)rb;
        self->statuses.push_back(status);
        self->bars.push_back(bars);
        self->userData = userData;
    }
};

/* ------------------------------------------------------------------ */
/* rb960xWriteMoBuffer()                                               */
/* ------------------------------------------------------------------ */

using Rb960xWriteMoBufferTest = Rb960xSbdTest;

TEST_F(Rb960xWriteMoBufferTest, ErrorInvalidArg)
{
    init9603();
    EXPECT_EQ(RB960X_ERR_ARG, rb960xWriteMoBuffer(nullptr, payload, payloadLen));
    EXPECT_EQ(RB960X_ERR_ARG, rb960xWriteMoBuffer(&rb, nullptr, payloadLen));
    EXPECT_EQ(RB960X_ERR_ARG, rb960xWriteMoBuffer(&rb, payload, 0));
    EXPECT_FALSE(sent("AT+SBDWB=5"));
}

TEST_F(Rb960xWriteMoBufferTest, ErrorTooLarge)
{
    static char big[RB960X_MO_MAX + 1];
    init9603();
    EXPECT_EQ(RB960X_ERR_TOO_LARGE, rb960xWriteMoBuffer(&rb, big, sizeof(big)));
    EXPECT_EQ(std::string::npos, mock.tx.find("AT+SBDWB"));
}

TEST_F(Rb960xWriteMoBufferTest, ErrorInvalidModel)
{
    scriptModem9603();
    replies["ATI7"] = "\r\nUNKNOWN\r\n\r\nOK\r\n";
    ASSERT_EQ(RB960X_OK, init());
    EXPECT_EQ(RB960X_ERR_INVALID_MODEL, rb960xWriteMoBuffer(&rb, payload, payloadLen));
}

TEST_F(Rb960xWriteMoBufferTest, ErrorWrongRat)
{
    scriptModem9604();
    replies["AT+URAT?"] = "\r\n+URAT: 7\r\n\r\nOK\r\n"; // LTE Cat M1 mode
    ASSERT_EQ(RB960X_OK, init());
    EXPECT_EQ(RB960X_ERR_WRONG_RAT, rb960xWriteMoBuffer(&rb, payload, payloadLen));
    EXPECT_EQ(std::string::npos, mock.tx.find("AT+SBDW"));
}

TEST_F(Rb960xWriteMoBufferTest, Success9603WritesBinaryAndStartsNoSession)
{
    init9603();
    replies["AT+SBDWB=5"] = SBDWB_READY_OK;

    EXPECT_EQ(RB960X_OK, rb960xWriteMoBuffer(&rb, payload, payloadLen));

    /* Command, payload, then the big-endian checksum. */
    EXPECT_NE(std::string::npos, mock.tx.find(std::string("AT+SBDWB=5\rHello\x01\xF4")));
    EXPECT_FALSE(sent("AT+SBDIX"));
    EXPECT_FALSE(sent(SBDST_CMD));
    EXPECT_EQ(0u, rb.sessionCount);
}

TEST_F(Rb960xWriteMoBufferTest, Success9604WritesHex)
{
    init9604();
    /* The 9604 takes hex-encoded data and replies "+SBDW:". */
    replies["AT+SBDW=5,\"48656C6C6F\""] = "\r\n+SBDW:0,5\r\n\r\nOK\r\n";

    EXPECT_EQ(RB960X_OK, rb960xWriteMoBuffer(&rb, payload, payloadLen));
    EXPECT_FALSE(sent("AT+SBDWB=5"));
    EXPECT_FALSE(sent("AT+SBDIX"));
}

TEST_F(Rb960xWriteMoBufferTest, Success9604MaxLength)
{
    /* A full-size message is streamed out in chunks; check the modem gets
     * one complete, correctly encoded command line. */
    std::string data(RB960X_MO_MAX, '\0');
    std::string hex;
    for (size_t i = 0; i < data.size(); i++)
    {
        data[i] = static_cast<char>(i & 0xFF);
        const char *digits = "0123456789ABCDEF";
        hex += digits[(i >> 4) & 0x0F];
        hex += digits[i & 0x0F];
    }
    const std::string cmd = "AT+SBDW=" + std::to_string(RB960X_MO_MAX) + ",\"" + hex + "\"";

    init9604();
    replies[cmd] = "\r\n+SBDW:0," + std::to_string(RB960X_MO_MAX) + "\r\n\r\nOK\r\n";

    EXPECT_EQ(RB960X_OK, rb960xWriteMoBuffer(&rb, data.data(), data.size()));
    EXPECT_TRUE(sent(cmd));
}

TEST_F(Rb960xWriteMoBufferTest, Error9604LengthOutOfRange)
{
    init9604();
    replies["AT+SBDW=5,\"48656C6C6F\""] = "\r\n+SBDW:3\r\n\r\nOK\r\n";
    EXPECT_EQ(RB960X_ERR_TOO_LARGE, rb960xWriteMoBuffer(&rb, payload, payloadLen));
}

TEST_F(Rb960xWriteMoBufferTest, Error9604InvalidData)
{
    init9604();
    replies["AT+SBDW=5,\"48656C6C6F\""] = "\r\n+SBDW:2\r\n\r\nOK\r\n";
    EXPECT_EQ(RB960X_ERR_PROTOCOL, rb960xWriteMoBuffer(&rb, payload, payloadLen));
}

TEST_F(Rb960xWriteMoBufferTest, ErrorNoReady)
{
    init9603();
    EXPECT_EQ(RB960X_ERR_TIMEOUT, rb960xWriteMoBuffer(&rb, payload, payloadLen));
}

TEST_F(Rb960xWriteMoBufferTest, ErrorChecksumMismatch)
{
    init9603();
    replies["AT+SBDWB=5"] = "\r\nREADY\r\n\r\n2\r\n\r\nOK\r\n";
    EXPECT_EQ(RB960X_ERR_PROTOCOL, rb960xWriteMoBuffer(&rb, payload, payloadLen));
}

TEST_F(Rb960xWriteMoBufferTest, ErrorSizeRejectedByModem)
{
    init9603();
    replies["AT+SBDWB=5"] = "\r\nREADY\r\n\r\n3\r\n\r\nOK\r\n";
    EXPECT_EQ(RB960X_ERR_TOO_LARGE, rb960xWriteMoBuffer(&rb, payload, payloadLen));
}

TEST_F(Rb960xWriteMoBufferTest, AllowedWhileMtMessageUnread)
{
    /* Writing the MO buffer doesn't touch the MT buffer. */
    init9603();
    rb.mtWaiting = true;
    replies["AT+SBDWB=5"] = SBDWB_READY_OK;
    EXPECT_EQ(RB960X_OK, rb960xWriteMoBuffer(&rb, payload, payloadLen));
}

/* ------------------------------------------------------------------ */
/* rb960xClearBuffers()                                                */
/* ------------------------------------------------------------------ */

using Rb960xClearBuffersTest = Rb960xSbdTest;

TEST_F(Rb960xClearBuffersTest, Commands9603)
{
    init9603();
    replies["AT+SBDD0"] = "\r\n0\r\n\r\nOK\r\n";
    replies["AT+SBDD1"] = "\r\n0\r\n\r\nOK\r\n";
    replies["AT+SBDD2"] = "\r\n0\r\n\r\nOK\r\n";

    EXPECT_EQ(RB960X_OK, rb960xClearBuffers(&rb, RB960X_BUFFER_MO));
    EXPECT_EQ(RB960X_OK, rb960xClearBuffers(&rb, RB960X_BUFFER_MT));
    EXPECT_EQ(RB960X_OK, rb960xClearBuffers(&rb, RB960X_BUFFER_BOTH));
    EXPECT_TRUE(sent("AT+SBDD0"));
    EXPECT_TRUE(sent("AT+SBDD1"));
    EXPECT_TRUE(sent("AT+SBDD2"));
}

TEST_F(Rb960xClearBuffersTest, Commands9604)
{
    init9604();
    replies["AT+SBDD=0"] = "\r\n+SBDD:0\r\n\r\nOK\r\n";
    replies["AT+SBDD=1"] = "\r\n+SBDD: 0\r\n\r\nOK\r\n";
    replies["AT+SBDD=2"] = "\r\n+SBDD:0\r\n\r\nOK\r\n";

    EXPECT_EQ(RB960X_OK, rb960xClearBuffers(&rb, RB960X_BUFFER_MO));
    EXPECT_EQ(RB960X_OK, rb960xClearBuffers(&rb, RB960X_BUFFER_MT));
    EXPECT_EQ(RB960X_OK, rb960xClearBuffers(&rb, RB960X_BUFFER_BOTH));
    EXPECT_FALSE(sent("AT+SBDD0"));
}

TEST_F(Rb960xClearBuffersTest, ErrorResultIsProtocolError)
{
    init9603();
    replies["AT+SBDD0"] = "\r\n1\r\n\r\nOK\r\n";
    EXPECT_EQ(RB960X_ERR_PROTOCOL, rb960xClearBuffers(&rb, RB960X_BUFFER_MO));

    replies["AT+SBDD0"] = REPLY_ERROR;
    EXPECT_EQ(RB960X_ERR_PROTOCOL, rb960xClearBuffers(&rb, RB960X_BUFFER_MO));
}

TEST_F(Rb960xClearBuffersTest, ErrorInvalidArg)
{
    init9603();
    EXPECT_EQ(RB960X_ERR_ARG, rb960xClearBuffers(nullptr, RB960X_BUFFER_MO));
    EXPECT_EQ(RB960X_ERR_ARG, rb960xClearBuffers(&rb, (rb960xBuffer_t)3));
    EXPECT_EQ(std::string::npos, mock.tx.find("AT+SBDD"));
}

TEST_F(Rb960xClearBuffersTest, ErrorWrongRat)
{
    scriptModem9604();
    replies["AT+URAT?"] = "\r\n+URAT: 7\r\n\r\nOK\r\n";
    ASSERT_EQ(RB960X_OK, init());
    EXPECT_EQ(RB960X_ERR_WRONG_RAT, rb960xClearBuffers(&rb, RB960X_BUFFER_MO));
}

TEST_F(Rb960xClearBuffersTest, ClearingMtDiscardsUnreadMessage)
{
    init9603();
    replies["AT+SBDD0"] = "\r\n0\r\n\r\nOK\r\n";
    replies["AT+SBDD1"] = "\r\n0\r\n\r\nOK\r\n";
    rb.mtWaiting = true;

    /* Clearing only the MO buffer leaves the MT message alone. */
    ASSERT_EQ(RB960X_OK, rb960xClearBuffers(&rb, RB960X_BUFFER_MO));
    EXPECT_TRUE(rb.mtWaiting);

    ASSERT_EQ(RB960X_OK, rb960xClearBuffers(&rb, RB960X_BUFFER_MT));
    EXPECT_FALSE(rb.mtWaiting);
}

/* ------------------------------------------------------------------ */
/* rb960xInitiateSession()                                             */
/* ------------------------------------------------------------------ */

using Rb960xInitiateSessionTest = Rb960xSbdTest;

TEST_F(Rb960xInitiateSessionTest, ErrorInvalidArg)
{
    memset(&result, 0xA5, sizeof(result));
    EXPECT_EQ(RB960X_ERR_ARG, rb960xInitiateSession(nullptr, false, 1000, &result));
    EXPECT_EQ(0u, result.moStatus);
    EXPECT_EQ(0u, result.mtQueued);
}

TEST_F(Rb960xInitiateSessionTest, ErrorInvalidModel)
{
    scriptModem9603();
    replies["ATI7"] = "\r\nUNKNOWN\r\n\r\nOK\r\n";
    ASSERT_EQ(RB960X_OK, init());
    EXPECT_EQ(RB960X_ERR_INVALID_MODEL, rb960xInitiateSession(&rb, false, 1000, nullptr));
    EXPECT_FALSE(sent("AT+SBDIX"));
}

TEST_F(Rb960xInitiateSessionTest, ErrorWrongRat)
{
    scriptModem9604();
    replies["AT+URAT?"] = "\r\n+URAT: 7\r\n\r\nOK\r\n";
    ASSERT_EQ(RB960X_OK, init());
    EXPECT_EQ(RB960X_ERR_WRONG_RAT, rb960xInitiateSession(&rb, false, 1000, nullptr));
    EXPECT_FALSE(sent("AT+SBDIX"));
}

TEST_F(Rb960xInitiateSessionTest, Success9603)
{
    init9603();
    replies["AT+SBDIX"] = "\r\n+SBDIX: 1, 65535, 1, 7, 270, 3\r\n\r\nOK\r\n";

    ASSERT_EQ(RB960X_OK, rb960xInitiateSession(&rb, false, 1000, &result));
    EXPECT_EQ(1u, result.moStatus);
    EXPECT_EQ(65535u, result.momsn);
    EXPECT_EQ(1u, result.mtStatus);
    EXPECT_EQ(7u, result.mtmsn);
    EXPECT_EQ(270u, result.mtLength);
    EXPECT_EQ(3u, result.mtQueued);
    EXPECT_EQ(1u, rb.sessionCount);

    /* Only the session timeout and the session: no write, no clear. */
    EXPECT_LT(indexOf(SBDST_CMD), indexOf("AT+SBDIX"));
    EXPECT_EQ(std::string::npos, mock.tx.find("AT+SBDWB"));
    EXPECT_EQ(std::string::npos, mock.tx.find("AT+SBDD"));
}

TEST_F(Rb960xInitiateSessionTest, Success9604)
{
    init9604();
    /* The 9604 guide shows "+SBDI:" for the session reply. */
    replies["AT+SBDIX"] = "\r\n+SBDI:0,42,0,0,0,0\r\n\r\nOK\r\n";

    ASSERT_EQ(RB960X_OK, rb960xInitiateSession(&rb, false, 1000, &result));
    EXPECT_EQ(42u, result.momsn);
}

TEST_F(Rb960xInitiateSessionTest, MoStatus4IsSuccess)
{
    init9603();
    replies["AT+SBDIX"] = "\r\n+SBDIX: 4, 12, 0, 0, 0, 0\r\n\r\nOK\r\n";
    EXPECT_EQ(RB960X_OK, rb960xInitiateSession(&rb, false, 1000, nullptr));
}

TEST_F(Rb960xInitiateSessionTest, SessionFailureFillsResult)
{
    init9603();
    replies["AT+SBDIX"] = "\r\n+SBDIX: 36, 12, 2, 0, 0, 0\r\n\r\nOK\r\n";

    ASSERT_EQ(RB960X_ERR_SESSION, rb960xInitiateSession(&rb, false, 1000, &result));
    EXPECT_EQ(36u, result.moStatus);
    EXPECT_EQ(12u, result.momsn);
    EXPECT_EQ(2u, result.mtStatus);
    EXPECT_EQ(RB960X_MO_WAIT_REGISTRATION, result.moResult);
}

TEST_F(Rb960xInitiateSessionTest, TimeoutZeroesResult)
{
    memset(&result, 0xA5, sizeof(result));
    init9603();
    /* No AT+SBDIX reply: the session times out. */

    ASSERT_EQ(RB960X_ERR_TIMEOUT, rb960xInitiateSession(&rb, false, 1000, &result));
    EXPECT_EQ(0u, result.moStatus);
    EXPECT_EQ(0u, result.momsn);
    EXPECT_EQ(0u, result.mtStatus);
    EXPECT_EQ(0u, result.mtmsn);
    EXPECT_EQ(0u, result.mtLength);
    EXPECT_EQ(0u, result.mtQueued);
}

TEST_F(Rb960xInitiateSessionTest, NeverRetriesByItself)
{
    init9603();
    replies["AT+SBDIX"] = sbdix(32); /* no network service */

    EXPECT_EQ(RB960X_ERR_SESSION, rb960xInitiateSession(&rb, false, 1000, nullptr));
    EXPECT_EQ(1, count("AT+SBDIX"));
    EXPECT_EQ(1u, rb.sessionCount);
}

TEST_F(Rb960xInitiateSessionTest, SbdstSentBeforeEverySession)
{
    init9603();
    replies["AT+SBDIX"] = SBDIX_SENT;

    ASSERT_EQ(RB960X_OK, rb960xInitiateSession(&rb, false, 1000, nullptr));
    ASSERT_NE(-1, indexOf(SBDST_CMD));
    EXPECT_LT(indexOf(SBDST_CMD), indexOf("AT+SBDIX"));

    commands.clear();
    ASSERT_EQ(RB960X_OK, rb960xInitiateSession(&rb, false, 1000, nullptr));
    ASSERT_NE(-1, indexOf(SBDST_CMD));
    EXPECT_LT(indexOf(SBDST_CMD), indexOf("AT+SBDIX"));
}

TEST_F(Rb960xInitiateSessionTest, SbdstErrorStopsSession)
{
    init9603();
    replies[SBDST_CMD] = REPLY_ERROR;
    replies["AT+SBDIX"] = SBDIX_SENT;

    EXPECT_EQ(RB960X_ERR_PROTOCOL, rb960xInitiateSession(&rb, false, 1000, nullptr));
    /* No session without a session timeout. */
    EXPECT_FALSE(sent("AT+SBDIX"));
    EXPECT_EQ(0u, rb.sessionCount);
}

TEST_F(Rb960xInitiateSessionTest, AnswerRingUsesSbdixa9603)
{
    init9603();
    replies["AT+SBDIXA"] = sbdix(0);

    mock.inject("\r\nSBDRING\r\n");
    rb960xProcess(&rb);
    ASSERT_TRUE(rb.ringAlert);

    EXPECT_EQ(RB960X_OK, rb960xInitiateSession(&rb, true, 1000, nullptr));
    EXPECT_TRUE(sent("AT+SBDIXA"));
    EXPECT_FALSE(sent("AT+SBDIX"));
    EXPECT_FALSE(rb.ringAlert);
}

TEST_F(Rb960xInitiateSessionTest, AnswerRingUsesSbdix9604)
{
    /* The 9604 has no AT+SBDIXA. */
    init9604();
    replies["AT+SBDIX"] = sbdix(0);

    EXPECT_EQ(RB960X_OK, rb960xInitiateSession(&rb, true, 1000, nullptr));
    EXPECT_TRUE(sent("AT+SBDIX"));
    EXPECT_FALSE(sent("AT+SBDIXA"));
}

TEST_F(Rb960xInitiateSessionTest, UnansweredGatewayKeepsRingAndQueue)
{
    init9603();
    replies["AT+SBDIX"] = sbdix(0, 0, 0, 2); /* 2 MT queued */
    ASSERT_EQ(RB960X_OK, rb960xInitiateSession(&rb, false, 1000, nullptr));
    EXPECT_EQ(2u, rb.mtQueued);

    mock.inject("\r\nSBDRING\r\n");
    rb960xProcess(&rb);

    /* MT status 2: the gateway wasn't checked, so nothing changes. */
    replies["AT+SBDIX"] = sbdix(32, 2);
    EXPECT_EQ(RB960X_ERR_SESSION, rb960xInitiateSession(&rb, false, 1000, nullptr));
    EXPECT_TRUE(rb.ringAlert);
    EXPECT_EQ(2u, rb.mtQueued);
}

TEST_F(Rb960xInitiateSessionTest, RefusedWhileMtMessageUnread)
{
    init9603();
    replies["AT+SBDIX"] = sbdix(18, 1, 5); /* MO failed, but an MT came down */
    replies["AT+SBDRB"] = sbdrb("World");

    ASSERT_EQ(RB960X_ERR_SESSION, rb960xInitiateSession(&rb, false, 1000, nullptr));
    ASSERT_TRUE(rb.mtWaiting);

    /* Another session would erase the message: refused before AT+SBDST. */
    commands.clear();
    EXPECT_EQ(RB960X_ERR_MT_UNREAD, rb960xInitiateSession(&rb, false, 1000, nullptr));
    EXPECT_TRUE(commands.empty());
    EXPECT_EQ(1u, rb.sessionCount);

    /* Read it, and sessions are allowed again. */
    ASSERT_EQ(RB960X_OK, readMt());
    replies["AT+SBDIX"] = sbdix(0);
    EXPECT_EQ(RB960X_OK, rb960xInitiateSession(&rb, false, 1000, nullptr));
    EXPECT_EQ(2u, rb.sessionCount);
}

/* Malformed or out-of-range replies are protocol errors. */
class Rb960xSbdixReplyTest : public Rb960xSbdTest,
                             public ::testing::WithParamInterface<const char *>
{
};

TEST_P(Rb960xSbdixReplyTest, MalformedReplyIsProtocolError)
{
    init9603();
    replies["AT+SBDIX"] = std::string("\r\n") + GetParam() + "\r\n\r\nOK\r\n";

    EXPECT_EQ(RB960X_ERR_PROTOCOL, rb960xInitiateSession(&rb, false, 1000, nullptr));
}

INSTANTIATE_TEST_SUITE_P(Replies, Rb960xSbdixReplyTest,
                         ::testing::Values("+SBDIX: 0, 12, 0",            /* too few fields */
                                           "+SBDIX: 0, 12, 0, 0, 0,",     /* empty last field */
                                           "+SBDIX: 256, 12, 0, 0, 0, 0", /* MO status > 255 */
                                           "+SBDIX: 0, 65536, 0, 0, 0, 0",/* MOMSN > 65535 */
                                           "+SBDIX: 0, x, 0, 0, 0, 0",    /* not a number */
                                           "SBDIX 0, 12, 0, 0, 0, 0"));   /* no colon */

/* ------------------------------------------------------------------ */
/* rb960xInitiateSessionAsync()                                        */
/* ------------------------------------------------------------------ */

class Rb960xInitiateSessionAsyncTest : public Rb960xSbdTest
{
protected:
    rb960xStatus_t start(bool answerRing = false)
    {
        return rb960xInitiateSessionAsync(&rb, answerRing, 1000, SessionCapture::callback, &capture);
    }

    SessionCapture capture;
};

TEST_F(Rb960xInitiateSessionAsyncTest, ErrorInvalidArg)
{
    init9603();
    EXPECT_EQ(RB960X_ERR_ARG, rb960xInitiateSessionAsync(nullptr, false, 1000, SessionCapture::callback, &capture));
    EXPECT_EQ(RB960X_ERR_ARG, rb960xInitiateSessionAsync(&rb, false, 1000, nullptr, nullptr));
    EXPECT_FALSE(sent("AT+SBDIX"));
}

TEST_F(Rb960xInitiateSessionAsyncTest, Success9603)
{
    init9603();

    /* Returns as soon as AT+SBDIX has gone out. */
    EXPECT_EQ(RB960X_OK, start());
    EXPECT_TRUE(sent("AT+SBDIX"));
    EXPECT_TRUE(rb.sessionPending);
    EXPECT_EQ(1u, rb.sessionCount);

    rb960xProcess(&rb);
    EXPECT_TRUE(capture.results.empty());

    mock.inject(SBDIX_SENT);
    rb960xProcess(&rb);
    ASSERT_EQ(1u, capture.results.size());
    EXPECT_EQ(RB960X_OK, capture.results[0]);
    EXPECT_EQ(&rb, capture.rb);
    EXPECT_EQ(&capture, capture.userData);
    EXPECT_FALSE(rb.sessionPending);

    /* Nothing more to report. */
    rb960xProcess(&rb);
    EXPECT_EQ(1u, capture.results.size());
}

TEST_F(Rb960xInitiateSessionAsyncTest, AnswerRingUsesSbdixa)
{
    init9603();
    replies["AT+SBDIXA"] = sbdix(0);

    ASSERT_EQ(RB960X_OK, start(true));
    rb960xProcess(&rb);
    ASSERT_EQ(1u, capture.results.size());
    EXPECT_TRUE(sent("AT+SBDIXA"));
}

TEST_F(Rb960xInitiateSessionAsyncTest, CallbackGetsSessionResult)
{
    init9604();
    replies["AT+SBDIX"] = "\r\n+SBDI:0,42,1,9,25,2\r\n\r\nOK\r\n";

    ASSERT_EQ(RB960X_OK, start());
    rb960xProcess(&rb);
    ASSERT_EQ(1u, capture.results.size());
    EXPECT_EQ(RB960X_OK, capture.results[0]);
    ASSERT_TRUE(capture.hadSession);
    EXPECT_EQ(0u, capture.session.moStatus);
    EXPECT_EQ(42u, capture.session.momsn);
    EXPECT_EQ(1u, capture.session.mtStatus);
    EXPECT_EQ(9u, capture.session.mtmsn);
    EXPECT_EQ(25u, capture.session.mtLength);
    EXPECT_EQ(2u, capture.session.mtQueued);
    EXPECT_TRUE(rb.mtWaiting);
}

TEST_F(Rb960xInitiateSessionAsyncTest, CallbackGetsResultOnSessionFailure)
{
    init9603();
    replies["AT+SBDIX"] = sbdix(32);

    ASSERT_EQ(RB960X_OK, start());
    rb960xProcess(&rb);
    ASSERT_EQ(1u, capture.results.size());
    EXPECT_EQ(RB960X_ERR_SESSION, capture.results[0]);
    ASSERT_TRUE(capture.hadSession);
    EXPECT_EQ(32u, capture.session.moStatus);
}

TEST_F(Rb960xInitiateSessionAsyncTest, CallbackGetsNoResultOnError)
{
    init9603();
    replies["AT+SBDIX"] = REPLY_ERROR;

    ASSERT_EQ(RB960X_OK, start());
    rb960xProcess(&rb);
    ASSERT_EQ(1u, capture.results.size());
    EXPECT_EQ(RB960X_ERR_PROTOCOL, capture.results[0]);
    EXPECT_FALSE(capture.hadSession);
}

TEST_F(Rb960xInitiateSessionAsyncTest, TimeoutGoesToCallback)
{
    init9603();

    EXPECT_EQ(RB960X_OK, start());

    mock.now += 999;
    rb960xProcess(&rb);
    EXPECT_TRUE(capture.results.empty());

    mock.now += 2;
    rb960xProcess(&rb);
    ASSERT_EQ(1u, capture.results.size());
    EXPECT_EQ(RB960X_ERR_TIMEOUT, capture.results[0]);
    EXPECT_FALSE(rb.sessionPending);
}

TEST_F(Rb960xInitiateSessionAsyncTest, BusyWhileSessionRunning)
{
    char version[32];
    uint8_t bars = 0;
    rb960xSbdStatus_t sbdStatus;

    init9603();
    replies["ATI3"] = "\r\nTA21004\r\n\r\nOK\r\n";
    replies["AT+CSQF"] = csq(5);

    ASSERT_EQ(RB960X_OK, start());
    commands.clear();

    /* Every SBD call is refused before it sends anything. */
    EXPECT_EQ(RB960X_ERR_BUSY, start());
    EXPECT_EQ(RB960X_ERR_BUSY, rb960xInitiateSession(&rb, false, 1000, nullptr));
    EXPECT_EQ(RB960X_ERR_BUSY, rb960xWriteMoBuffer(&rb, payload, payloadLen));
    EXPECT_EQ(RB960X_ERR_BUSY, rb960xClearBuffers(&rb, RB960X_BUFFER_MO));
    EXPECT_EQ(RB960X_ERR_BUSY, readMt());
    EXPECT_EQ(RB960X_ERR_BUSY, rb960xGetSignal(&rb, false, &bars));
    EXPECT_EQ(RB960X_ERR_BUSY, rb960xSessionReady(&rb));
    EXPECT_EQ(RB960X_ERR_BUSY, rb960xGetSbdStatus(&rb, &sbdStatus));
    EXPECT_TRUE(commands.empty());

    /* Other commands fail in the AT client. */
    EXPECT_EQ(RB960X_ERR_PROTOCOL, rb960xGetSatelliteVersion(&rb, version, sizeof(version)));
    EXPECT_FALSE(sent("ATI3"));

    /* Once the session finishes the modem is usable again. */
    mock.inject(SBDIX_SENT);
    rb960xProcess(&rb);
    EXPECT_EQ(1u, capture.results.size());
    EXPECT_FALSE(rb.sessionPending);
    EXPECT_EQ(RB960X_OK, rb960xGetSatelliteVersion(&rb, version, sizeof(version)));
    EXPECT_STREQ("TA21004", version);
}

TEST_F(Rb960xInitiateSessionAsyncTest, NotStartedWithoutCallback)
{
    init9603();
    rb.mtWaiting = true;

    EXPECT_EQ(RB960X_ERR_MT_UNREAD, start());
    EXPECT_FALSE(sent("AT+SBDIX"));
    EXPECT_FALSE(rb.sessionPending);

    rb960xProcess(&rb);
    EXPECT_TRUE(capture.results.empty());
}

/* The callback can start the next session straight away. */
struct RestartCapture
{
    rb960xStatus_t restartStatus = RB960X_ERR_ARG;
    int calls = 0;

    static void callback(rb960x_t *rb, rb960xStatus_t status, const rb960xSessionResult_t *result, void *userData)
    {
        RestartCapture *self = static_cast<RestartCapture *>(userData);
        (void)status;
        (void)result;
        self->calls++;
        if (self->calls == 1)
        {
            self->restartStatus = rb960xInitiateSessionAsync(rb, false, 1000, RestartCapture::callback, self);
        }
    }
};

TEST_F(Rb960xInitiateSessionAsyncTest, CallbackCanStartAnotherSession)
{
    RestartCapture restart;

    init9603();
    replies["AT+SBDIX"] = SBDIX_SENT;

    ASSERT_EQ(RB960X_OK, rb960xInitiateSessionAsync(&rb, false, 1000, RestartCapture::callback, &restart));
    rb960xProcess(&rb);
    EXPECT_EQ(1, restart.calls);
    EXPECT_EQ(RB960X_OK, restart.restartStatus);
    EXPECT_TRUE(rb.sessionPending);

    rb960xProcess(&rb);
    EXPECT_EQ(2, restart.calls);
    EXPECT_FALSE(rb.sessionPending);
    EXPECT_EQ(2, count("AT+SBDIX"));
}

/* ------------------------------------------------------------------ */
/* rb960xAtDn() / rb960xAtKn()                                         */
/* ------------------------------------------------------------------ */

using Rb960xModemSettingsTest = Rb960xSbdTest;

TEST_F(Rb960xModemSettingsTest, DtrOption3Only9603)
{
    init9603();
    replies["AT&D3"] = REPLY_OK;
    EXPECT_EQ(RB960X_OK, rb960xAtDn(&rb, 3));
    EXPECT_TRUE(sent("AT&D3"));
    EXPECT_EQ(RB960X_ERR_ARG, rb960xAtDn(&rb, 4));
    EXPECT_FALSE(sent("AT&D4"));
}

TEST_F(Rb960xModemSettingsTest, DtrOption3Rejected9604)
{
    init9604();
    replies["AT&D2"] = REPLY_OK;
    EXPECT_EQ(RB960X_OK, rb960xAtDn(&rb, 2));
    EXPECT_TRUE(sent("AT&D2"));
    EXPECT_EQ(RB960X_ERR_ARG, rb960xAtDn(&rb, 3));
    EXPECT_FALSE(sent("AT&D3"));
}

TEST_F(Rb960xModemSettingsTest, FlowControlNoneOrRtsCtsOnly)
{
    init9604();
    replies["AT&K3"] = REPLY_OK;
    EXPECT_EQ(RB960X_OK, rb960xAtKn(&rb, RB960X_FLOW_RTS_CTS));
    EXPECT_TRUE(sent("AT&K3"));
    for (uint32_t option : { 1u, 2u, 4u, 5u, 6u, 10u })
    {
        EXPECT_EQ(RB960X_ERR_ARG, rb960xAtKn(&rb, option)) << "AT&K" << option;
    }
    EXPECT_FALSE(sent("AT&K4"));
}

/* ------------------------------------------------------------------ */
/* MO status decoding (rb960xSessionResult_t::moResult)                */
/* ------------------------------------------------------------------ */

using Rb960xMoStatusTest = Rb960xSbdTest;

/* Runs one session that reports @p mo as the MO status and returns the
 * decoded result. */
static rb960xMoStatus_t sessionMoResult(rb960x_t *rb, std::map<std::string, std::string> &replies, unsigned int mo)
{
    rb960xSessionResult_t result;
    replies["AT+SBDIX"] = "\r\n+SBDIX: " + std::to_string(mo) + ", 12, 0, 0, 0, 0\r\n\r\nOK\r\n";
    (void)rb960xInitiateSession(rb, false, 1000, &result);
    EXPECT_EQ(mo, (unsigned int)result.moStatus);
    return result.moResult;
}

/* Codes both guides define decode the same on every model. */
static const std::pair<unsigned int, rb960xMoStatus_t> SHARED_MO_CODES[] = {
    { 0, RB960X_MO_OK },
    { 2, RB960X_MO_OK_LOCATION_UPDATE_REJECTED },
    { 15, RB960X_MO_ACCESS_DENIED },
    { 16, RB960X_MO_LOCKED },
    { 17, RB960X_MO_GATEWAY_NOT_RESPONDING },
    { 18, RB960X_MO_RF_DROP },
    { 32, RB960X_MO_NO_SERVICE },
    { 33, RB960X_MO_ANTENNA_FAULT },
    { 34, RB960X_MO_RADIO_DISABLED },
    { 35, RB960X_MO_BUSY },
    { 36, RB960X_MO_WAIT_REGISTRATION },
    { 37, RB960X_MO_SERVICE_DISABLED },
    { 38, RB960X_MO_TRAFFIC_MANAGEMENT },
    { 65, RB960X_MO_HARDWARE_ERROR },
};

TEST_F(Rb960xMoStatusTest, SharedCodes9603)
{
    init9603();
    for (const auto &c : SHARED_MO_CODES)
    {
        EXPECT_EQ(c.second, sessionMoResult(&rb, replies, c.first)) << "MO status " << c.first;
    }
}

TEST_F(Rb960xMoStatusTest, SharedCodes9604)
{
    init9604();
    for (const auto &c : SHARED_MO_CODES)
    {
        EXPECT_EQ(c.second, sessionMoResult(&rb, replies, c.first)) << "MO status " << c.first;
    }
}

TEST_F(Rb960xMoStatusTest, Codes9603)
{
    const std::pair<unsigned int, rb960xMoStatus_t> codes[] = {
        { 1, RB960X_MO_OK_MT_TOO_BIG },
        { 3, RB960X_MO_RESERVED_SUCCESS },
        { 4, RB960X_MO_RESERVED_SUCCESS },
        { 5, RB960X_MO_RESERVED_FAILURE },
        { 9, RB960X_MO_RESERVED_FAILURE },
        { 10, RB960X_MO_GSS_CALL_TIMEOUT },
        { 11, RB960X_MO_GSS_QUEUE_FULL },
        { 12, RB960X_MO_TOO_MANY_SEGMENTS },
        { 13, RB960X_MO_GSS_SESSION_INCOMPLETE },
        { 14, RB960X_MO_INVALID_SEGMENT_SIZE },
        { 19, RB960X_MO_LINK_FAILURE },
        { 20, RB960X_MO_RESERVED_FAILURE },
        { 39, RB960X_MO_RESERVED_FAILURE },
        { 64, RB960X_MO_BAND_VIOLATION },
        { 66, RB960X_MO_RESERVED_FAILURE },
        { 255, RB960X_MO_RESERVED_FAILURE },
    };

    init9603();
    for (const auto &c : codes)
    {
        EXPECT_EQ(c.second, sessionMoResult(&rb, replies, c.first)) << "MO status " << c.first;
    }
}

TEST_F(Rb960xMoStatusTest, Codes9604)
{
    /* Codes the 9602/9603 guide names but the 9604 guide leaves reserved. */
    const std::pair<unsigned int, rb960xMoStatus_t> codes[] = {
        { 1, RB960X_MO_RESERVED_SUCCESS },
        { 3, RB960X_MO_RESERVED_LOCATION_UPDATE_FAILURE },
        { 4, RB960X_MO_RESERVED_LOCATION_UPDATE_FAILURE },
        { 10, RB960X_MO_RESERVED_LOCATION_UPDATE_FAILURE },
        { 14, RB960X_MO_RESERVED_LOCATION_UPDATE_FAILURE },
        { 19, RB960X_MO_RESERVED_FAILURE },
        { 64, RB960X_MO_RESERVED_FAILURE },
        { 66, RB960X_MO_RESERVED_FAILURE },
    };

    init9604();
    for (const auto &c : codes)
    {
        EXPECT_EQ(c.second, sessionMoResult(&rb, replies, c.first)) << "MO status " << c.first;
    }
}

TEST(Rb960xMoStatusText, EveryValueHasText)
{
    for (int v = 0; v <= 65; v++)
    {
        EXPECT_NE(nullptr, rb960xMoStatusText((rb960xMoStatus_t)v));
    }
    EXPECT_STREQ("Access is denied", rb960xMoStatusText(RB960X_MO_ACCESS_DENIED));
    EXPECT_STREQ("Reserved, but indicates failure", rb960xMoStatusText(RB960X_MO_RESERVED_FAILURE));
    EXPECT_STREQ("Reserved, but indicates location update failure",
                 rb960xMoStatusText(RB960X_MO_RESERVED_LOCATION_UPDATE_FAILURE));
    EXPECT_STREQ("Unknown MO status", rb960xMoStatusText((rb960xMoStatus_t)9));
}

/* ------------------------------------------------------------------ */
/* Session timeout (AT+SBDST)                                          */
/* ------------------------------------------------------------------ */

TEST(Rb960xConfig, SbdixTimeoutDefaultsToSessionTimeoutPlusMargin)
{
    EXPECT_EQ((RB960X_SBD_SESSION_TIMEOUT_S + 30UL) * 1000UL, (unsigned long)RB960X_SBDIX_TIMEOUT_MS);
}

TEST_F(Rb960xSbdTest, AtSbdstFormatsSeconds)
{
    init9603();
    replies["AT+SBDST=0"] = REPLY_OK;
    replies["AT+SBDST=65535"] = REPLY_OK;

    EXPECT_EQ(RB960X_OK, rb960xAtSbdst(&rb, 0));
    EXPECT_EQ(RB960X_OK, rb960xAtSbdst(&rb, 65535));
    EXPECT_TRUE(sent("AT+SBDST=0"));
    EXPECT_TRUE(sent("AT+SBDST=65535"));
}

/* ------------------------------------------------------------------ */
/* rb960xReadMtBuffer()                                                */
/* ------------------------------------------------------------------ */

using Rb960xReadMtBufferTest = Rb960xSbdTest;

TEST_F(Rb960xReadMtBufferTest, ErrorCases)
{
    scriptModem9603();
    replies["ATI7"] = "\r\nUNKNOWN\r\n\r\nOK\r\n";
    ASSERT_EQ(RB960X_OK, init());
    EXPECT_EQ(RB960X_ERR_INVALID_MODEL, readMt());
    EXPECT_EQ(0u, length);
    EXPECT_EQ(RB960X_ERR_ARG, rb960xReadMtBuffer(nullptr, buf, sizeof(buf), &length));
    EXPECT_EQ(RB960X_ERR_ARG, rb960xReadMtBuffer(&rb, nullptr, sizeof(buf), &length));
    EXPECT_EQ(RB960X_ERR_ARG, rb960xReadMtBuffer(&rb, buf, sizeof(buf), nullptr));
}

TEST_F(Rb960xReadMtBufferTest, ReadsMessageWithoutSession)
{
    init9603();
    replies["AT+SBDRB"] = sbdrb("World");

    /* Reads the buffer whether or not the driver saw a session put a
     * message there (it may predate rb960xInit()). */
    ASSERT_EQ(RB960X_OK, readMt());
    EXPECT_EQ(std::string("World"), std::string(buf, length));
    EXPECT_FALSE(sent("AT+SBDIX"));
    EXPECT_EQ(0u, rb.sessionCount);
}

TEST_F(Rb960xReadMtBufferTest, ReadsMessageFromSession)
{
    init9603();
    replies["AT+SBDIX"] = sbdix(0, 1, 5);
    replies["AT+SBDRB"] = sbdrb("World");

    ASSERT_EQ(RB960X_OK, rb960xInitiateSession(&rb, false, 1000, &result));
    ASSERT_EQ(1u, result.mtStatus);
    ASSERT_TRUE(rb.mtWaiting);

    ASSERT_EQ(RB960X_OK, readMt());
    EXPECT_EQ(std::string("World"), std::string(buf, length));
    EXPECT_FALSE(rb.mtWaiting);
    EXPECT_EQ(1, count("AT+SBDIX"));
}

TEST_F(Rb960xReadMtBufferTest, EmptyBuffer)
{
    init9603();
    replies["AT+SBDRB"] = sbdrb("");

    EXPECT_EQ(RB960X_ERR_NO_MESSAGE, readMt());
    EXPECT_EQ(0u, length);
}

TEST_F(Rb960xReadMtBufferTest, BinaryMessage)
{
    const std::string msg("\x00\x01\r\nOK\xFF", 7);

    init9603();
    replies["AT+SBDRB"] = sbdrb(msg);

    ASSERT_EQ(RB960X_OK, readMt());
    EXPECT_EQ(msg, std::string(buf, length));
}

TEST_F(Rb960xReadMtBufferTest, ChecksumMismatchKeepsMessage)
{
    init9603();
    rb.mtWaiting = true;
    replies["AT+SBDRB"] = sbdrb("World", 1);

    EXPECT_EQ(RB960X_ERR_PROTOCOL, readMt());
    EXPECT_TRUE(rb.mtWaiting);

    replies["AT+SBDRB"] = sbdrb("World");
    EXPECT_EQ(RB960X_OK, readMt()); /* read again */
    EXPECT_FALSE(rb.mtWaiting);
}

TEST_F(Rb960xReadMtBufferTest, MessageTooLargeIsDiscarded)
{
    char small[3];

    init9603();
    rb.mtWaiting = true;
    replies["AT+SBDRB"] = sbdrb("World");
    replies["AT"] = REPLY_OK;

    EXPECT_EQ(RB960X_ERR_TOO_LARGE, rb960xReadMtBuffer(&rb, small, sizeof(small), &length));
    EXPECT_EQ(0u, length);
    EXPECT_FALSE(rb.mtWaiting);
    /* The whole reply was consumed: the next command works. */
    EXPECT_EQ(RB960X_OK, rb960xAtProbe(&rb, 1000));
}

/* 9604: the MT buffer is read hex encoded with AT+SBDR. */
class Rb960xReadMtBuffer9604Test : public Rb960xSbdTest
{
protected:
    void SetUp() override
    {
        Rb960xSbdTest::SetUp();
        init9604();
        rb.mtWaiting = true;
    }
};

TEST_F(Rb960xReadMtBuffer9604Test, ReadsHex)
{
    replies["AT+SBDR"] = sbdr("World");

    ASSERT_EQ(RB960X_OK, readMt());
    EXPECT_EQ(std::string("World"), std::string(buf, length));
    EXPECT_FALSE(sent("AT+SBDRB"));
    EXPECT_FALSE(rb.mtWaiting);
}

TEST_F(Rb960xReadMtBuffer9604Test, BinaryAndLowerCaseHex)
{
    const std::string msg("\x00\x01\r\nOK\"\xFF", 8);

    replies["AT+SBDR"] = sbdr(msg, true);

    ASSERT_EQ(RB960X_OK, readMt());
    EXPECT_EQ(msg, std::string(buf, length));
}

TEST_F(Rb960xReadMtBuffer9604Test, FullSizeMessage)
{
    std::string msg;
    for (int i = 0; i < RB960X_MT_MAX; i++)
    {
        msg += (char)(i & 0xFF);
    }

    replies["AT+SBDR"] = sbdr(msg);
    replies["AT"] = REPLY_OK;

    ASSERT_EQ(RB960X_OK, readMt());
    EXPECT_EQ(msg, std::string(buf, length));
    /* The whole reply was consumed: the next command works. */
    EXPECT_EQ(RB960X_OK, rb960xAtProbe(&rb, 1000));
}

TEST_F(Rb960xReadMtBuffer9604Test, MessageTooLargeIsDiscarded)
{
    char small[3];

    replies["AT+SBDR"] = sbdr("World");
    replies["AT"] = REPLY_OK;

    EXPECT_EQ(RB960X_ERR_TOO_LARGE, rb960xReadMtBuffer(&rb, small, sizeof(small), &length));
    EXPECT_EQ(0u, length);
    EXPECT_FALSE(rb.mtWaiting);
    EXPECT_EQ(RB960X_OK, rb960xAtProbe(&rb, 1000));
}

TEST_F(Rb960xReadMtBuffer9604Test, MalformedReplyKeepsMessage)
{
    replies["AT"] = REPLY_OK;

    for (const char *bad : { "\r\n+SBDR:5,\"576F7G6C64\"\r\n\r\nOK\r\n",  /* not hex */
                             "\r\n+SBDR:x,\"576F726C64\"\r\n\r\nOK\r\n",   /* no length */
                             "\r\nERROR\r\n" })
    {
        replies["AT+SBDR"] = bad;
        EXPECT_EQ(RB960X_ERR_PROTOCOL, readMt()) << bad;
        EXPECT_TRUE(rb.mtWaiting) << bad;
        EXPECT_EQ(RB960X_OK, rb960xAtProbe(&rb, 1000)) << bad; /* still in step */
    }

    replies["AT+SBDR"] = sbdr("World");
    ASSERT_EQ(RB960X_OK, readMt()); /* read again */
    EXPECT_EQ(std::string("World"), std::string(buf, length));
}

TEST_F(Rb960xReadMtBuffer9604Test, EmptyBuffer)
{
    replies["AT+SBDR"] = "\r\n+SBDR:0,\"\"\r\n\r\nOK\r\n";

    EXPECT_EQ(RB960X_ERR_NO_MESSAGE, readMt());
    EXPECT_FALSE(rb.mtWaiting);
}

TEST_F(Rb960xReadMtBuffer9604Test, WrongRat)
{
    rb.ratMode = RB960X_RAT_LTE_CAT_M1;
    EXPECT_EQ(RB960X_ERR_WRONG_RAT, readMt());
    EXPECT_FALSE(sent("AT+SBDR"));
}

/* ------------------------------------------------------------------ */
/* Mailbox check: clear the MO buffer, then a session                  */
/* ------------------------------------------------------------------ */

TEST_F(Rb960xSbdTest, MailboxCheckFlow9603)
{
    init9603();
    replies["AT+SBDD0"] = "\r\n0\r\n\r\nOK\r\n";
    replies["AT+SBDIXA"] = sbdix(0, 1, 5, 1);
    replies["AT+SBDRB"] = sbdrb("World");

    mock.inject("\r\nSBDRING\r\n");
    rb960xProcess(&rb);
    ASSERT_TRUE(rb.ringAlert);

    ASSERT_EQ(RB960X_OK, rb960xClearBuffers(&rb, RB960X_BUFFER_MO));
    ASSERT_EQ(RB960X_OK, rb960xInitiateSession(&rb, rb.ringAlert, 1000, &result));
    ASSERT_EQ(1u, result.mtStatus);
    ASSERT_EQ(RB960X_OK, readMt());
    EXPECT_EQ(std::string("World"), std::string(buf, length));

    EXPECT_LT(indexOf("AT+SBDD0"), indexOf(SBDST_CMD));
    EXPECT_LT(indexOf(SBDST_CMD), indexOf("AT+SBDIXA"));
    EXPECT_LT(indexOf("AT+SBDIXA"), indexOf("AT+SBDRB"));
    EXPECT_FALSE(rb.ringAlert);
    EXPECT_EQ(1u, rb.mtQueued);
}

/* ------------------------------------------------------------------ */
/* rb960xGetSignal()                                                   */
/* ------------------------------------------------------------------ */

class Rb960xGetSignalTest : public Rb960xSbdTest
{
protected:
    uint8_t bars = 0xFF;
};

TEST_F(Rb960xGetSignalTest, ErrorInvalidArg)
{
    init9603();
    EXPECT_EQ(RB960X_ERR_ARG, rb960xGetSignal(nullptr, false, &bars));
    EXPECT_EQ(0u, bars);
    EXPECT_EQ(RB960X_ERR_ARG, rb960xGetSignal(&rb, false, nullptr));
}

TEST_F(Rb960xGetSignalTest, LastKnownSignalUsesCsqf)
{
    init9603();
    replies["AT+CSQF"] = csq(5);

    EXPECT_EQ(RB960X_OK, rb960xGetSignal(&rb, false, &bars));
    EXPECT_EQ(5u, bars);
    EXPECT_TRUE(sent("AT+CSQF"));
    EXPECT_FALSE(sent("AT+CSQ"));
}

TEST_F(Rb960xGetSignalTest, FreshSignalUsesCsq)
{
    init9604();
    replies["AT+CSQ"] = csq(" 4");

    EXPECT_EQ(RB960X_OK, rb960xGetSignal(&rb, true, &bars));
    EXPECT_EQ(4u, bars);
    EXPECT_TRUE(sent("AT+CSQ"));
    EXPECT_FALSE(sent("AT+CSQF"));
}

TEST_F(Rb960xGetSignalTest, TextAfterDigitIgnored)
{
    /* The 9604 guide's example reply is "+CSQ:5 F". */
    init9604();
    replies["AT+CSQF"] = csq("5 F");
    EXPECT_EQ(RB960X_OK, rb960xGetSignal(&rb, false, &bars));
    EXPECT_EQ(5u, bars);
}

TEST_F(Rb960xGetSignalTest, InvalidReadingIsProtocolError)
{
    init9603();

    for (const char *value : { "6", "", "x" })
    {
        replies["AT+CSQF"] = csq(std::string(value));
        EXPECT_EQ(RB960X_ERR_PROTOCOL, rb960xGetSignal(&rb, false, &bars)) << value;
        EXPECT_EQ(0u, bars) << value;
    }

    replies["AT+CSQF"] = "\r\nCSQ 3\r\n\r\nOK\r\n"; /* no colon */
    EXPECT_EQ(RB960X_ERR_PROTOCOL, rb960xGetSignal(&rb, false, &bars));
}

TEST_F(Rb960xGetSignalTest, ErrorReplyIsProtocolError)
{
    init9603();
    replies["AT+CSQF"] = REPLY_ERROR;
    EXPECT_EQ(RB960X_ERR_PROTOCOL, rb960xGetSignal(&rb, false, &bars));
}

TEST_F(Rb960xGetSignalTest, NoReplyTimesOut)
{
    init9603();
    EXPECT_EQ(RB960X_ERR_TIMEOUT, rb960xGetSignal(&rb, true, &bars));
    EXPECT_EQ(0u, bars);
}

TEST_F(Rb960xGetSignalTest, ErrorWrongRat)
{
    scriptModem9604();
    replies["AT+URAT?"] = "\r\n+URAT: 7\r\n\r\nOK\r\n"; // LTE Cat M1 mode
    ASSERT_EQ(RB960X_OK, init());
    replies["AT+CSQF"] = csq(5);

    EXPECT_EQ(RB960X_ERR_WRONG_RAT, rb960xGetSignal(&rb, false, &bars));
    EXPECT_FALSE(sent("AT+CSQF"));
}

TEST_F(Rb960xGetSignalTest, ErrorInvalidModel)
{
    scriptModem9603();
    replies["ATI7"] = "\r\nUNKNOWN\r\n\r\nOK\r\n";
    ASSERT_EQ(RB960X_OK, init());
    EXPECT_EQ(RB960X_ERR_INVALID_MODEL, rb960xGetSignal(&rb, false, &bars));
}

/* ------------------------------------------------------------------ */
/* rb960xSessionReady()                                                */
/* ------------------------------------------------------------------ */

using Rb960xSessionReadyTest = Rb960xSbdTest;

TEST_F(Rb960xSessionReadyTest, ErrorInvalidArg)
{
    EXPECT_EQ(RB960X_ERR_ARG, rb960xSessionReady(nullptr));
}

TEST_F(Rb960xSessionReadyTest, ReadyUsesLastKnownSignal)
{
    init9603();
    replies["AT+CSQF"] = csq(RB960X_MIN_SIGNAL_BARS);

    EXPECT_EQ(RB960X_OK, rb960xSessionReady(&rb));
    EXPECT_TRUE(sent("AT+CSQF"));
    EXPECT_FALSE(sent("AT+CSQ"));
    EXPECT_EQ(0u, rb.sessionCount);
}

TEST_F(Rb960xSessionReadyTest, NoSignalBelowMinimum)
{
    init9603();

    if (RB960X_MIN_SIGNAL_BARS > 0)
    {
        replies["AT+CSQF"] = csq(RB960X_MIN_SIGNAL_BARS - 1);
        EXPECT_EQ(RB960X_ERR_NO_SIGNAL, rb960xSessionReady(&rb));
    }
}

TEST_F(Rb960xSessionReadyTest, MtUnreadCheckedBeforeSignal)
{
    init9603();
    replies["AT+CSQF"] = csq(5);
    rb.mtWaiting = true;

    EXPECT_EQ(RB960X_ERR_MT_UNREAD, rb960xSessionReady(&rb));
    EXPECT_FALSE(sent("AT+CSQF"));
}

TEST_F(Rb960xSessionReadyTest, ErrorWrongRat)
{
    scriptModem9604();
    replies["AT+URAT?"] = "\r\n+URAT: 7\r\n\r\nOK\r\n";
    ASSERT_EQ(RB960X_OK, init());
    EXPECT_EQ(RB960X_ERR_WRONG_RAT, rb960xSessionReady(&rb));
}

TEST_F(Rb960xSessionReadyTest, SignalErrorPassedOn)
{
    init9603();
    replies["AT+CSQF"] = REPLY_ERROR;
    EXPECT_EQ(RB960X_ERR_PROTOCOL, rb960xSessionReady(&rb));
}

/* ------------------------------------------------------------------ */
/* rb960xSetSignalCallback() and signal polling in rb960xProcess()     */
/* ------------------------------------------------------------------ */

class Rb960xSignalPollTest : public Rb960xSbdTest
{
protected:
    SignalCapture capture;
};

TEST_F(Rb960xSignalPollTest, NoPollingWithoutCallback)
{
    init9603();
    replies["AT+CSQF"] = csq(5);

    rb960xProcess(&rb);
    mock.now += 10U * RB960X_SIGNAL_POLL_MS;
    rb960xProcess(&rb);
    EXPECT_FALSE(sent("AT+CSQF"));
}

TEST_F(Rb960xSignalPollTest, PollsEveryInterval)
{
    init9603();
    replies["AT+CSQF"] = csq(4);
    rb960xSetSignalCallback(&rb, SignalCapture::callback, &capture);

    /* First poll on the next rb960xProcess(). */
    rb960xProcess(&rb);
    ASSERT_EQ(1u, capture.statuses.size());
    EXPECT_EQ(RB960X_OK, capture.statuses[0]);
    EXPECT_EQ(4u, capture.bars[0]);
    EXPECT_EQ(&capture, capture.userData);

    /* Not again until the interval has passed. */
    mock.now += RB960X_SIGNAL_POLL_MS - 1U;
    rb960xProcess(&rb);
    EXPECT_EQ(1u, capture.statuses.size());

    replies["AT+CSQF"] = csq(2);
    mock.now += 1U;
    rb960xProcess(&rb);
    ASSERT_EQ(2u, capture.statuses.size());
    EXPECT_EQ(2u, capture.bars[1]);

    /* Only ever the last known signal, never a fresh reading. */
    EXPECT_EQ(2, count("AT+CSQF"));
    EXPECT_FALSE(sent("AT+CSQ"));
}

TEST_F(Rb960xSignalPollTest, ClearingCallbackStopsPolling)
{
    init9603();
    replies["AT+CSQF"] = csq(4);
    rb960xSetSignalCallback(&rb, SignalCapture::callback, &capture);
    rb960xProcess(&rb);
    rb960xSetSignalCallback(&rb, nullptr, nullptr);

    mock.now += 10U * RB960X_SIGNAL_POLL_MS;
    rb960xProcess(&rb);
    EXPECT_EQ(1u, capture.statuses.size());
    EXPECT_EQ(1, count("AT+CSQF"));
}

TEST_F(Rb960xSignalPollTest, ErrorsGoToCallback)
{
    scriptModem9604();
    replies["AT+URAT?"] = "\r\n+URAT: 7\r\n\r\nOK\r\n"; // LTE Cat M1 mode
    ASSERT_EQ(RB960X_OK, init());
    rb960xSetSignalCallback(&rb, SignalCapture::callback, &capture);

    rb960xProcess(&rb);
    ASSERT_EQ(1u, capture.statuses.size());
    EXPECT_EQ(RB960X_ERR_WRONG_RAT, capture.statuses[0]);
    EXPECT_EQ(0u, capture.bars[0]);
}

TEST_F(Rb960xSignalPollTest, SkippedWhileSessionRunning)
{
    SessionCapture session;

    init9603();
    replies["AT+CSQF"] = csq(4);
    rb960xSetSignalCallback(&rb, SignalCapture::callback, &capture);
    ASSERT_EQ(RB960X_OK, rb960xInitiateSessionAsync(&rb, false, 60000, SessionCapture::callback, &session));

    mock.now += 2U * RB960X_SIGNAL_POLL_MS;
    rb960xProcess(&rb);
    EXPECT_TRUE(capture.statuses.empty());
    EXPECT_FALSE(sent("AT+CSQF"));

    /* The session finishes, then the overdue poll runs. */
    mock.inject(SBDIX_SENT);
    rb960xProcess(&rb);
    EXPECT_EQ(1u, session.results.size());
    rb960xProcess(&rb);
    EXPECT_EQ(1u, capture.statuses.size());
}

/* ------------------------------------------------------------------ */
/* rb960xSetRingCallback()                                             */
/* ------------------------------------------------------------------ */

/* Records every call to the ring callback, optionally doing something. */
struct RingCapture
{
    int calls = 0;
    bool ringAlertSeen = false;
    void *userData = nullptr;
    std::function<void(rb960x_t *)> action;

    static void callback(rb960x_t *rb, void *userData)
    {
        RingCapture *self = static_cast<RingCapture *>(userData);
        self->calls++;
        self->ringAlertSeen = rb->ringAlert;
        self->userData = userData;
        if (self->action)
        {
            self->action(rb);
        }
    }
};

class Rb960xRingCallbackTest : public Rb960xSbdTest
{
protected:
    RingCapture ring;
};

TEST_F(Rb960xRingCallbackTest, CalledOncePerRing)
{
    init9603();
    rb960xSetRingCallback(&rb, RingCapture::callback, &ring);

    rb960xProcess(&rb);
    EXPECT_EQ(0, ring.calls);

    mock.inject("\r\nSBDRING\r\n");
    rb960xProcess(&rb);
    ASSERT_EQ(1, ring.calls);
    EXPECT_TRUE(ring.ringAlertSeen);
    EXPECT_EQ(&ring, ring.userData);

    /* Reported once; the flag stays set until a session answers. */
    rb960xProcess(&rb);
    EXPECT_EQ(1, ring.calls);
    EXPECT_TRUE(rb.ringAlert);

    /* A repeated ring alert is reported again. */
    mock.inject("\r\nSBDRING\r\n");
    rb960xProcess(&rb);
    EXPECT_EQ(2, ring.calls);

    /* The driver never answers a ring by itself. */
    EXPECT_FALSE(sent("AT+SBDIX"));
    EXPECT_FALSE(sent("AT+SBDIXA"));
}

TEST_F(Rb960xRingCallbackTest, RingDuringCommandWaitsForProcess)
{
    uint8_t bars = 0;

    init9603();
    rb960xSetRingCallback(&rb, RingCapture::callback, &ring);
    replies["AT+CSQF"] = "\r\nSBDRING\r\n" + csq(3);

    ASSERT_EQ(RB960X_OK, rb960xGetSignal(&rb, false, &bars));
    EXPECT_EQ(3u, bars);
    EXPECT_TRUE(rb.ringAlert);
    EXPECT_EQ(0, ring.calls);

    rb960xProcess(&rb);
    EXPECT_EQ(1, ring.calls);
}

TEST_F(Rb960xRingCallbackTest, SkippedWhenSessionAnsweredIt)
{
    init9603();
    rb960xSetRingCallback(&rb, RingCapture::callback, &ring);
    /* The ring alert arrives during a session that reaches the gateway. */
    replies["AT+SBDIX"] = "\r\nSBDRING\r\n" + sbdix(0);

    ASSERT_EQ(RB960X_OK, rb960xInitiateSession(&rb, false, 1000, nullptr));
    EXPECT_FALSE(rb.ringAlert);

    rb960xProcess(&rb);
    EXPECT_EQ(0, ring.calls);
}

TEST_F(Rb960xRingCallbackTest, RingBeforeCallbackSetIsReported)
{
    init9603();
    mock.inject("\r\nSBDRING\r\n");
    rb960xProcess(&rb);
    ASSERT_TRUE(rb.ringAlert);

    rb960xSetRingCallback(&rb, RingCapture::callback, &ring);
    rb960xProcess(&rb);
    EXPECT_EQ(1, ring.calls);
}

TEST_F(Rb960xRingCallbackTest, ClearingCallbackStopsReports)
{
    init9603();
    rb960xSetRingCallback(&rb, RingCapture::callback, &ring);
    rb960xSetRingCallback(&rb, nullptr, nullptr);

    mock.inject("\r\nSBDRING\r\n");
    rb960xProcess(&rb);
    EXPECT_EQ(0, ring.calls);
    EXPECT_TRUE(rb.ringAlert);
}

TEST_F(Rb960xRingCallbackTest, CallbackCanAnswerRing)
{
    rb960xStatus_t answered = RB960X_ERR_ARG;

    init9603();
    replies["AT+SBDIXA"] = sbdix(0, 1, 5);
    ring.action = [&answered](rb960x_t *r) { answered = rb960xInitiateSession(r, true, 1000, nullptr); };
    rb960xSetRingCallback(&rb, RingCapture::callback, &ring);

    mock.inject("\r\nSBDRING\r\n");
    rb960xProcess(&rb);
    ASSERT_EQ(1, ring.calls);
    EXPECT_EQ(RB960X_OK, answered);
    EXPECT_TRUE(sent("AT+SBDIXA"));
    EXPECT_FALSE(rb.ringAlert);
    EXPECT_TRUE(rb.mtWaiting);
}

TEST_F(Rb960xRingCallbackTest, AsyncSessionFromCallbackSkipsSignalPoll)
{
    SessionCapture session;
    SignalCapture signal;

    init9603();
    replies["AT+CSQF"] = csq(4);
    ring.action = [&session](rb960x_t *r)
    {
        EXPECT_EQ(RB960X_OK, rb960xInitiateSessionAsync(r, true, 60000, SessionCapture::callback, &session));
    };
    rb960xSetRingCallback(&rb, RingCapture::callback, &ring);
    rb960xSetSignalCallback(&rb, SignalCapture::callback, &signal);

    mock.inject("\r\nSBDRING\r\n");
    rb960xProcess(&rb);
    ASSERT_EQ(1, ring.calls);
    EXPECT_TRUE(rb.sessionPending);
    /* No poll (it would only have reported RB960X_ERR_BUSY). */
    EXPECT_TRUE(signal.statuses.empty());
    EXPECT_FALSE(sent("AT+CSQF"));

    mock.inject(sbdix(0));
    rb960xProcess(&rb);
    ASSERT_EQ(1u, session.results.size());
    EXPECT_EQ(RB960X_OK, session.results[0]);
    EXPECT_FALSE(rb.ringAlert);

    rb960xProcess(&rb);
    ASSERT_EQ(1u, signal.statuses.size());
    EXPECT_EQ(RB960X_OK, signal.statuses[0]);
}

/* ------------------------------------------------------------------ */
/* rb960xGetSbdStatus()                                                */
/* ------------------------------------------------------------------ */

TEST_F(Rb960xSbdTest, GetSbdStatusParsesReply)
{
    rb960xSbdStatus_t st;

    init9603();
    replies["AT+SBDSX"] = "\r\n+SBDSX: 1, 42, 1, 126, 1, 3\r\n\r\nOK\r\n";
    ASSERT_EQ(RB960X_OK, rb960xGetSbdStatus(&rb, &st));
    EXPECT_TRUE(st.moWaiting);
    EXPECT_EQ(42u, st.momsn);
    EXPECT_TRUE(st.mtWaiting);
    EXPECT_EQ(126, st.mtmsn);
    EXPECT_TRUE(st.ringAlert);
    EXPECT_EQ(3u, st.mtQueued);

    /* 9604 style, no spaces, empty MT buffer. */
    replies["AT+SBDSX"] = "\r\n+SBDSX:0,42,0,-1,0,0\r\n\r\nOK\r\n";
    ASSERT_EQ(RB960X_OK, rb960xGetSbdStatus(&rb, &st));
    EXPECT_FALSE(st.moWaiting);
    EXPECT_FALSE(st.mtWaiting);
    EXPECT_EQ(-1, st.mtmsn);
    EXPECT_FALSE(st.ringAlert);
}

TEST_F(Rb960xSbdTest, GetSbdStatusErrors)
{
    rb960xSbdStatus_t st;

    init9603();
    EXPECT_EQ(RB960X_ERR_ARG, rb960xGetSbdStatus(nullptr, &st));
    EXPECT_EQ(RB960X_ERR_ARG, rb960xGetSbdStatus(&rb, nullptr));

    for (const char *bad : { "\r\n+SBDSX: 0, 42, 0, -1, 0\r\n\r\nOK\r\n",    /* field missing */
                             "\r\n+SBDSX: 0, 42, 0, -1, 2, 0\r\n\r\nOK\r\n",  /* flag out of range */
                             "\r\n+SBDSX: 0, x, 0, -1, 0, 0\r\n\r\nOK\r\n",
                             "\r\nSBDSX 0, 42, 0, -1, 0, 0\r\n\r\nOK\r\n",    /* no colon */
                             REPLY_ERROR })
    {
        replies["AT+SBDSX"] = bad;
        EXPECT_EQ(RB960X_ERR_PROTOCOL, rb960xGetSbdStatus(&rb, &st)) << bad;
        EXPECT_FALSE(st.ringAlert) << bad;
    }
}

/* ------------------------------------------------------------------ */
/* rb960xEnd() after rb960xInit()                                      */
/* ------------------------------------------------------------------ */

#ifdef RB960X_TEST_HAVE_PORT

TEST_F(Rb960xInitTest, InitOwnsNoPortEvenOnDirtyContext)
{
    /* rb960xEnd() must never close a port rb960xInit() didn't open, even
     * if the context held garbage beforehand. */
    memset(&rb, 0xA5, sizeof(rb));
    scriptModem9603();

    ASSERT_EQ(RB960X_OK, init());
    EXPECT_FALSE(rb.ownsPort);
    EXPECT_EQ(RB960X_OK, rb960xEnd(&rb));
    EXPECT_FALSE(rb.initialised);
}

/* ------------------------------------------------------------------ */
/* rb960xBegin() / rb960xEnd()                                         */
/* ------------------------------------------------------------------ */

TEST(Rb960xBeginTest, RejectsNullContext)
{
    EXPECT_EQ(RB960X_ERR_ARG, rb960xBegin(nullptr, "/dev/null", 19200, nullptr, nullptr));
    EXPECT_EQ(RB960X_ERR_ARG, rb960xEnd(nullptr));
}

TEST(Rb960xBeginTest, ReportsPortErrorAndOwnsNothing)
{
    rb960x_t rb;

    EXPECT_EQ(RB960X_ERR_PORT, rb960xBegin(&rb, "/dev/rb960x-test-does-not-exist", 19200, nullptr, nullptr));
    EXPECT_FALSE(rb.ownsPort);
    EXPECT_EQ(RB960X_ERR_PORT, rb960xBegin(&rb, nullptr, 19200, nullptr, nullptr));
    EXPECT_EQ(RB960X_ERR_PORT, rb960xBegin(&rb, "/dev/null", 12345, nullptr, nullptr)); /* unsupported baud */
    EXPECT_EQ(RB960X_OK, rb960xEnd(&rb));
}

/* Answers a 9603's init commands on the master side of a pseudo-terminal,
 * so rb960xBegin() runs on the real POSIX port (Linux or macOS). */
class FakeModemPty
{
public:
    FakeModemPty()
    {
        master = posix_openpt(O_RDWR | O_NOCTTY);
        if ((master >= 0) && (grantpt(master) == 0) && (unlockpt(master) == 0))
        {
            const char *name = ptsname(master);
            if (name != nullptr)
            {
                slave = name;
            }
        }
        replies = {
            {"AT", "OK"}, {"ATE0", "OK"}, {"ATI7", "BOOT07d4/9603NGrevA/04/RAW0f\r\n\r\nOK"},
            {"AT+CGSN", "300234010753370\r\n\r\nOK"}, {"AT+GSN", "300234010753370\r\n\r\nOK"},
            {"AT&D0", "OK"}, {"AT&K0", "OK"}, {"AT+SBDMTA=1", "OK"},
        };
        worker = std::thread([this] { run(); });
    }

    ~FakeModemPty()
    {
        stop = true;
        worker.join();
        if (master >= 0)
        {
            close(master);
        }
    }

    int master = -1;
    std::string slave;

private:
    void run()
    {
        std::string line;
        while (!stop && (master >= 0))
        {
            /* select(), not poll(): macOS's poll() doesn't support
             * devices such as a pty. */
            fd_set readable;
            struct timeval wait = {0, 20000};
            char c;

            FD_ZERO(&readable);
            FD_SET(master, &readable);
            if ((select(master + 1, &readable, nullptr, nullptr, &wait) <= 0) || (read(master, &c, 1) != 1))
            {
                continue;
            }
            if (c != '\r')
            {
                line += c;
                continue;
            }
            auto it = replies.find(line);
            std::string reply = "\r\n" + ((it != replies.end()) ? it->second : std::string("ERROR")) + "\r\n";
            ssize_t written = write(master, reply.data(), reply.size());
            (void)written; /* nothing to do if the driver side has gone */
            line.clear();
        }
    }

    std::map<std::string, std::string> replies;
    std::atomic<bool> stop{false};
    std::thread worker;
};

TEST(Rb960xBeginTest, OpensPortInitialisesAndEndCloses)
{
    FakeModemPty modem;
    rb960x_t rb;

    ASSERT_FALSE(modem.slave.empty());
    ASSERT_EQ(RB960X_OK, rb960xBegin(&rb, modem.slave.c_str(), 19200, nullptr, nullptr));

    /* The port rb960xBegin() opened survives rb960xInit()'s clear. */
    EXPECT_TRUE(rb.ownsPort);
    EXPECT_GE(rb.portHandle.sys.fd, 0);
    EXPECT_TRUE(rb.initialised);
    EXPECT_EQ(RB960X_MODEL_9603, rb.model);
    EXPECT_STREQ("300234010753370", rb.imei);

    EXPECT_EQ(RB960X_OK, rb960xEnd(&rb));
    EXPECT_FALSE(rb.ownsPort);
    EXPECT_FALSE(rb.initialised);
    EXPECT_EQ(-1, rb.portHandle.sys.fd);

    EXPECT_EQ(RB960X_OK, rb960xEnd(&rb)); /* twice is fine */
}

#endif /* RB960X_TEST_HAVE_PORT */

} /* namespace */
