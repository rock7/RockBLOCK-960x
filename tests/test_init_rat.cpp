/**
 * @file test_init_rat.cpp
 * @brief Tests for RB960X_INIT_RAT: the RAT rb960xInit() puts the 9604 in.
 *
 * Built once per setting (see CMakeLists.txt), each against a copy of the
 * driver built with that setting. The scripted 9604 here remembers its RAT:
 * AT+URAT=<n> sets it and AT+URAT? reports it, so a switch can be followed
 * through the reset.
 */

#include "rockblock960x.h"
#include "mock_port.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace
{
const char *const REPLY_OK = "\r\nOK\r\n";
const char *const REPLY_ERROR = "\r\nERROR\r\n";

const char *const URAT_CELLULAR = "7";
const char *const URAT_SBD = "10";

#if RB960X_INIT_RAT == RB960X_INIT_RAT_SBD
const rb960xRatMode_t INIT_MODE = RB960X_RAT_SBD;
const char *const INIT_URAT = URAT_SBD;
const char *const OTHER_URAT = URAT_CELLULAR;
#elif RB960X_INIT_RAT == RB960X_INIT_RAT_CELLULAR
const rb960xRatMode_t INIT_MODE = RB960X_RAT_LTE_CAT_M1;
const char *const INIT_URAT = URAT_CELLULAR;
const char *const OTHER_URAT = URAT_SBD;
#else
#error "test_init_rat.cpp is only built with RB960X_INIT_RAT set to SBD or CELLULAR"
#endif

class Rb960xInitRatTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        mock.onCommand = [this](const std::string &cmd)
        {
            reply(cmd);
        };

        replies["AT"] = REPLY_OK;
        replies["ATE0"] = REPLY_OK;
        replies["AT&D0"] = REPLY_OK;
        replies["AT&K0"] = REPLY_OK;
        replies["AT+CFUN=0"] = REPLY_OK;
        replies["AT+CFUN=1"] = REPLY_OK;
        replies["AT+CFUN=16"] = REPLY_OK;
    }

    void script9603()
    {
        replies["ATI7"] = "\r\nBOOT07d4/9603NGrevA/04/RAW0f\r\n\r\nOK\r\n";
        replies["AT+CGSN"] = "\r\n300234010753370\r\n\r\nOK\r\n";
        replies["AT+GSN"] = "\r\n300234010753370\r\n\r\nOK\r\n";
        replies["AT+SBDMTA=1"] = REPLY_OK;
    }

    void script9604(const std::string &urat)
    {
        modemUrat = urat;
        replies["ATI7"] = "\r\n9604\r\n\r\nOK\r\n";
        replies["AT+CGSN"] = "\r\n351234567890123\r\n\r\nOK\r\n";
        replies["AT+USATSN"] = "\r\n+USATSN: SN0001,300434060000000\r\n\r\nOK\r\n";
        replies["AT+URING=1"] = REPLY_OK;
    }

    /* Record the command and inject the reply: AT+URAT from the modem's
     * state (unless uratWorks is false), everything else from replies. */
    void reply(const std::string &cmd)
    {
        commands.push_back(cmd);

        if ((modemUrat.empty() != true) && (cmd == "AT+URAT?"))
        {
            mock.inject("\r\n+URAT: " + modemUrat + "\r\n\r\nOK\r\n");
        }
        else if ((modemUrat.empty() != true) && (cmd.rfind("AT+URAT=", 0) == 0))
        {
            if (uratWorks == true)
            {
                modemUrat = cmd.substr(8);
            }
            mock.inject((uratWorks == true) ? REPLY_OK : REPLY_ERROR);
        }
        else
        {
            auto it = replies.find(cmd);
            if (it != replies.end())
            {
                mock.inject(it->second);
            }
        }
    }

    bool sent(const std::string &cmd) const
    {
        return std::find(commands.begin(), commands.end(), cmd) != commands.end();
    }

    rb960xStatus_t init()
    {
        return rb960xInit(&rb, &mock.port, nullptr, nullptr);
    }

    MockPort mock;
    rb960x_t rb;
    std::map<std::string, std::string> replies;
    std::vector<std::string> commands;
    std::string modemUrat;
    bool uratWorks = true;
};

TEST_F(Rb960xInitRatTest, SwitchesWhenInTheOtherMode)
{
    script9604(OTHER_URAT);

    ASSERT_EQ(RB960X_OK, init());
    EXPECT_EQ(INIT_MODE, rb.ratMode);
    EXPECT_EQ(INIT_URAT, modemUrat);

    /* The switch comes before the settings, which are made once. */
    const std::vector<std::string> expected =
    {
        "AT", "ATE0", "ATI7", "AT+CGSN", "AT+USATSN", "AT+URAT?",
        "AT+CFUN=0", std::string("AT+URAT=") + INIT_URAT, "AT+CFUN=16", "AT", "ATE0", "AT+URAT?",
        "AT&D0", "AT&K0", "AT+URING=1"
    };
    EXPECT_EQ(expected, commands);
}

TEST_F(Rb960xInitRatTest, NoResetWhenAlreadyInTheMode)
{
    script9604(INIT_URAT);

    ASSERT_EQ(RB960X_OK, init());
    EXPECT_EQ(INIT_MODE, rb.ratMode);
    EXPECT_FALSE(sent("AT+CFUN=0"));
    EXPECT_FALSE(sent("AT+CFUN=16"));
}

TEST_F(Rb960xInitRatTest, FailedSwitchFailsInit)
{
    script9604(OTHER_URAT);
    uratWorks = false;

    EXPECT_EQ(RB960X_ERR_PROTOCOL, init());
    EXPECT_FALSE(rb.initialised);
    /* The radio is turned back on and the module isn't reset. */
    EXPECT_TRUE(sent("AT+CFUN=1"));
    EXPECT_FALSE(sent("AT+CFUN=16"));
}

TEST_F(Rb960xInitRatTest, IgnoredOn9603)
{
    script9603();

    ASSERT_EQ(RB960X_OK, init());
    EXPECT_EQ(RB960X_RAT_UNKNOWN, rb.ratMode);
    EXPECT_FALSE(sent("AT+CFUN=0"));
    EXPECT_FALSE(sent("AT+URAT?"));
}

} // namespace
