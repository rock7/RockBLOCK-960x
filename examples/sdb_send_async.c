#include "rockblock960x.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <errno.h>
#include <limits.h>
#include "crossplatform.h"

static char _serialDevice[PATH_MAX];
static char _payload[RB960X_MO_MAX];
// Default baud rate. The 9604 uses 115200; the 9602 and 9603 need 19200 (-b 19200).
static uint32_t _baudRate = 115200;
static bool _debug = false;

/* Filled in by sessionCallback(). */
typedef struct
{
    bool done;
    rb960xStatus_t status;
    rb960xSessionResult_t session;
} userDataResult_t;

typedef enum
{
    SUCCESS = 0,
    INVALID_ARGUMENTS,
    FAILED_TO_INITIALISE_SERIAL_PORT,
    FAILED_TO_INITIALISE_RB_INSTANCE,
    FAILED_TO_GET_SIGNAL,
    FAILED_TO_SEND_SBD,
    FAILED_TO_SBD_SESSION_FAILED
} returnCode_t;

static struct option _longOptions[] =
{
    {"device", required_argument, 0, 'd'},
    {"baud", required_argument, 0, 'b'},
    {"message", required_argument, 0, 'm'},
    {"debug", no_argument, 0, 'D'},
    {"help", no_argument, 0, 'h'},
    {0, 0, 0, 0}
};

static void printHelp(const char * progName)
{
    printf("Usage: %s -d <device> [-h]\n", progName);
    printf("  -d, --device   Serial device to use (mandatory)\n");
    printf("  -b, --baud     Serial Baud Rate default is 115200 bps\n");
    printf("  -m, --message  Payload to send over SDB\n");
    printf("  -D, --debug    Enable debug on the standard output\n");
    printf("  -h, --help     Display this help message\n");
}

static void debugHandler(atDebugDir_t dir, const char * data, size_t len, void * userData)
{
    (void)userData;
    printf("%s %.*s\n", (dir == AT_DEBUG_TX) ? "TX >" : "RX <", (int)len, data);
}


static bool waitForSignal (rb960x_t * rb)
{
    const uint32_t delayMs = 500;
    const size_t waitCountMax = (60000 / delayMs); // Should be about 60 seconds
    size_t waitCount = 0;
    uint8_t bars = 0;
    rb960xStatus_t status = RB960X_OK;

    do
    {
        // A fresh reading (AT+CSQ): free, but takes a few seconds.
        status = rb960xGetSignal(rb, true, &bars);

        if ((status != RB960X_OK) || (bars >= RB960X_MIN_SIGNAL_BARS))
        {
            break;
        }

        delay(delayMs);
        waitCount++;
    } while (waitCount < waitCountMax);

    printf("Signal: %u/5\n", (unsigned int)bars);

    return ((status == RB960X_OK) && (bars >= RB960X_MIN_SIGNAL_BARS));
}

static void sessionCallback(rb960x_t *rb, rb960xStatus_t status, const rb960xSessionResult_t *session, void *userData)
{
    userDataResult_t *result = (userDataResult_t *)userData;

    (void)rb;
    result->status = status;
    if (session != NULL)
    {
        result->session = *session;
    }
    result->done = true;
}

// One attempt: load the MO buffer, start the session, then keep calling
// rb960xProcess() until the callback reports the result. No retry.
static rb960xStatus_t sendAsync(rb960x_t *rb, const char *message, size_t length, uint32_t timeoutMs, rb960xSessionResult_t *session)
{
    userDataResult_t result = { false, RB960X_OK, { 0 } };
    rb960xStatus_t status;
    unsigned int polls = 0U;
    const uint32_t pollDelayMs = 100;

    status = rb960xWriteMoBuffer(rb, message, length);

    if (status == RB960X_OK)
    {
        printf("Sending %zu byte(s) asynchronously, waiting up to %u seconds", length, (unsigned int)(timeoutMs / 1000U));
        fflush(stdout);

        status = rb960xInitiateSessionAsync(rb, false, timeoutMs, sessionCallback, &result);
    }

    if (status == RB960X_OK)
    {
        /* The session has started; the callback will report how it went.
         * The driver applies timeoutMs, so this loop ends. */
        while (result.done == false)
        {
            rb960xProcess(rb);
            delay(pollDelayMs);

            polls++;
            if (((polls % 10) == 0U) && (_debug == false))
            {
                printf(".");
                fflush(stdout);
            }
        }

        printf("\n");
        status = result.status;
        *session = result.session;
    }

    fflush(stdout); /* finish the progress line before any error on stderr */

    return status;
}

int main(int argc, char * argv[])
{
    int rVal = SUCCESS;
    bool gotArgs = true;
    int opt = 0;
    rb960x_t rb;
    rb960xStatus_t status = RB960X_OK;
    rb960xSessionResult_t session = { 0 };

    _serialDevice[0] = '\0';
    _payload[0] = '\0';

    while ((opt = getopt_long(argc, argv, "d:b:m:Dh", _longOptions, NULL)) != -1)
    {
        switch (opt)
        {
            case 'd':
                strncpy(_serialDevice, optarg, PATH_MAX -1);
            break;

            case 'b':
                _baudRate = (uint32_t)strtol(optarg, NULL, 10);
            break;

            case 'm':
                strncpy(_payload, optarg, RB960X_MO_MAX -1);
            break;

            case 'D':
                /* A flag (no_argument), so optarg is NULL here. */
                _debug = true;
            break;

            case 'h':
                printHelp(argv[0]);
                gotArgs = false;
            break;

            case '?':
            // fall through
            default:
                printHelp(argv[0]);
                rVal = -INVALID_ARGUMENTS;
            break;
        }
    }

    if ((rVal == SUCCESS) && (gotArgs == true))
    {
        // Open the serial port and initialise the RockBLOCK instance
        status = rb960xBegin(&rb, _serialDevice, _baudRate, _debug ? debugHandler : NULL, NULL);

        if (status == RB960X_OK)
        {
            // The 9604 can only send in Satellite (SBD) mode. It's left in that mode afterwards.
            if ((rb.model == RB960X_MODEL_9604) && (rb.ratMode != RB960X_RAT_SBD))
            {
                printf("Switching the 9604 to Satellite mode (the module reboots)...\n");
                status = rb960xSwitchRat(&rb, RB960X_RAT_SBD);
                if (status != RB960X_OK)
                {
                    fprintf(stderr, "%s: failed to switch to Satellite mode: (status %d)\n", argv[0], (int)status);
                }
            }

            if (status == RB960X_OK)
            {
                printf("Waiting for valid signal..\n");
                if (waitForSignal(&rb) == true)
                {
                    status = sendAsync(&rb, _payload, strlen(_payload), RB960X_SBDIX_TIMEOUT_MS, &session);

                    if((status == RB960X_OK) || (status == RB960X_ERR_SESSION))
                    {
                        printf("Session: MO status %u, MOMSN %u\n", (unsigned int)session.moStatus, (unsigned int)session.momsn);

                        if (status != RB960X_OK)
                        {
                            printf("Session failed: %s\n", rb960xMoStatusText(session.moResult));
                            rVal = -FAILED_TO_SBD_SESSION_FAILED;
                        }
                    }
                    else
                    {
                        fprintf(stderr, "%s: failed to send SBD message\n", argv[0]);
                        rVal = -FAILED_TO_SEND_SBD;
                    }
                }
                else
                {
                    fprintf(stderr, "%s: timedout waiting for a valid SBD signal\n", argv[0]);
                    rVal = -FAILED_TO_GET_SIGNAL;
                }
            }
        }
        else if (status == RB960X_ERR_PORT)
        {
            fprintf(stderr, "%s: failed to open %s at %d baud: %s\n", argv[0], _serialDevice, _baudRate, strerror(errno));
            rVal = -FAILED_TO_INITIALISE_SERIAL_PORT;
        }
        else
        {
            fprintf(stderr, "%s: failed to initialise rb960x instance\n", argv[0]);
            rVal = -FAILED_TO_INITIALISE_RB_INSTANCE;
        }

        rb960xEnd(&rb);
    }

    return rVal;
}