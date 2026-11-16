#include "rockblock960x.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include "crossplatform.h"

static char _serialDevice[PATH_MAX];
// Default baud rate. The 9604 uses 115200; the 9602 and 9603 need 19200 (-b 19200).
static uint32_t _baudRate = 115200;
// How long to listen for, in seconds. 0 listens until Ctrl+C.
static uint32_t _timeoutS = 0;
static bool _debug = false;
static volatile sig_atomic_t _run = 1;

// Pause between passes of the listen loop.
static const uint32_t _pollDelayMs = 1000;
// Shortest gap between mailbox checks. Every check is an SBD session, which
// can cost credits, so don't run them back to back.
static const uint32_t _checkGapMs = 30000;
// Last signal reported by the signal callback (rb960xProcess() polls it).
static uint8_t _lastBars = 0;

typedef enum
{
    SUCCESS = 0,
    INVALID_ARGUMENTS,
    FAILED_TO_INITIALISE_SERIAL_PORT,
    FAILED_TO_INITIALISE_RB_INSTANCE,
    FAILED_TO_GET_SIGNAL,
    FAILED_TO_RECEIVE_SBD
} returnCode_t;

static struct option _longOptions[] =
{
    {"device", required_argument, 0, 'd'},
    {"baud", required_argument, 0, 'b'},
    {"timeout", required_argument, 0, 't'},
    {"debug", no_argument, 0, 'D'},
    {"help", no_argument, 0, 'h'},
    {0, 0, 0, 0}
};

static void printHelp(const char * progName)
{
    printf("Usage: %s -d <device> [-b <baud>] [-t <seconds>] [-D] [-h]\n", progName);
    printf("  -d, --device   Serial device to use (mandatory)\n");
    printf("  -b, --baud     Serial Baud Rate default is 115200 bps\n");
    printf("  -t, --timeout  Seconds to listen for, default is 0 (until Ctrl+C)\n");
    printf("  -D, --debug    Enable debug on the standard output\n");
    printf("  -h, --help     Display this help message\n");
}

static void debugHandler(atDebugDir_t dir, const char * data, size_t len, void * userData)
{
    (void)userData;
    printf("%s %.*s\n", (dir == AT_DEBUG_TX) ? "TX >" : "RX <", (int)len, data);
}

static void stopHandler(int sig)
{
    (void)sig;
    _run = 0;
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
    } while ((_run != 0) && (waitCount < waitCountMax));

    printf("Signal: %u/5\n", (unsigned int)bars);

    return ((status == RB960X_OK) && (bars >= RB960X_MIN_SIGNAL_BARS));
}

// Called from rb960xProcess() every RB960X_SIGNAL_POLL_MS with the last
// known signal (AT+CSQF).
static void signalCallback(rb960x_t * rb, rb960xStatus_t status, uint8_t bars, void * userData)
{
    (void)rb;
    (void)userData;

    if ((status == RB960X_OK) && (bars != _lastBars))
    {
        printf("Signal: %u/5\n", (unsigned int)bars);
        fflush(stdout);
    }
    _lastBars = bars;
}

// Called from rb960xProcess() after an SBDRING ring alert: the gateway has
// an MT message for us. userData points at listenForMessages()'s checkDue. It could
// answer the ring right here with rb960xInitiateSession(rb, true, ...), but
// leaving it to the loop keeps the gap between sessions in one place.
static void ringCallback(rb960x_t * rb, void * userData)
{
    (void)rb;

    printf("Ring alert: an MT message is waiting at the gateway\n");
    fflush(stdout);
    *(bool *)userData = true;
}

// MT messages can be binary: print printable bytes as text and the rest as '.'.
static void printMessage(const char * data, size_t length)
{
    size_t i = 0;

    printf("Received MT SBD, %zu byte(s): ", length);

    for (i = 0; i < length; i++)
    {
        const unsigned char c = (unsigned char)data[i];
        putchar(((c >= 0x20U) && (c < 0x7FU)) ? (int)c : '.');
    }

    putchar('\n');
    fflush(stdout);
}

// One mailbox check: clear the MO buffer so nothing is sent, run a session
// (answering the ring alert if there is one), and read any MT message it
// brought down. Returns the session status.
static rb960xStatus_t checkMailbox(rb960x_t * rb, char * mtBuffer, size_t mtBufferSize, size_t * mtLength, bool * gotMessage)
{
    rb960xSessionResult_t session = { 0 };
    rb960xStatus_t status = rb960xClearBuffers(rb, RB960X_BUFFER_MO);

    *gotMessage = false;

    if (status == RB960X_OK)
    {
        status = rb960xInitiateSession(rb, rb->ringAlert, RB960X_SBDIX_TIMEOUT_MS, &session);
    }

    // A session can bring a message down even if the MO part failed.
    if (((status == RB960X_OK) || (status == RB960X_ERR_SESSION)) && (session.mtStatus == 1U))
    {
        rb960xStatus_t readStatus = rb960xReadMtBuffer(rb, mtBuffer, mtBufferSize, mtLength);

        if (readStatus == RB960X_OK)
        {
            *gotMessage = true;
        }
        else
        {
            status = readStatus;
        }
    }

    return status;
}

static returnCode_t listenForMessages(rb960x_t * rb, const char * progName)
{
    returnCode_t rVal = SUCCESS;
    char mtBuffer[RB960X_MT_MAX];
    size_t mtLength = 0;
    size_t received = 0;
    rb960xStatus_t status = RB960X_OK;
    bool gotMessage = false;
    // Check once at the start: that picks up anything sent while we were
    // away, and registers the modem with the gateway for ring alerts.
    bool checkDue = true;
    const unsigned long startMs = millis();
    unsigned long lastCheckMs = startMs;
    bool checked = false;

    if (_timeoutS == 0)
    {
        printf("Listening for MT messages (Ctrl+C to stop)...\n");
    }
    else
    {
        printf("Listening for MT messages for %u seconds (Ctrl+C to stop)...\n", (unsigned int)_timeoutS);
    }
    fflush(stdout);

    rb960xSetSignalCallback(rb, signalCallback, NULL);
    rb960xSetRingCallback(rb, ringCallback, &checkDue);

    while ((_run != 0) && ((_timeoutS == 0) || ((millis() - startMs) < ((unsigned long)_timeoutS * 1000UL))))
    {
        // Dispatches SBDRING (calls ringCallback) and polls the signal.
        rb960xProcess(rb);

        if (rb->mtQueued > 0U)
        {
            checkDue = true;
        }

        if ((checkDue == true) && ((checked != true) || ((millis() - lastCheckMs) >= _checkGapMs)) &&
            (rb960xSessionReady(rb) == RB960X_OK))
        {
            lastCheckMs = millis();
            checked = true;
            printf("Checking the mailbox%s...\n", (rb->ringAlert == true) ? " (ring alert)" : "");
            fflush(stdout);

            status = checkMailbox(rb, mtBuffer, sizeof(mtBuffer), &mtLength, &gotMessage);

            if (gotMessage == true)
            {
                printMessage(mtBuffer, mtLength);
                received++;
            }

            if ((status == RB960X_OK) || (status == RB960X_ERR_SESSION))
            {
                // Only done once the gateway has answered; otherwise try again
                // after the gap.
                checkDue = (status != RB960X_OK);

                if (rb->mtQueued > 0U)
                {
                    printf("%u more MT message(s) queued at the gateway\n", (unsigned int)rb->mtQueued);
                }
            }
            else if (status == RB960X_ERR_TOO_LARGE)
            {
                // Can't happen with an RB960X_MT_MAX buffer, but the message is lost if it does.
                fprintf(stderr, "%s: MT message too large for the buffer, discarded\n", progName);
            }
            else
            {
                fprintf(stderr, "%s: mailbox check failed: (status %d)\n", progName, (int)status);
                rVal = FAILED_TO_RECEIVE_SBD;
                break;
            }
        }

        delay(_pollDelayMs);
    }

    rb960xSetSignalCallback(rb, NULL, NULL);
    rb960xSetRingCallback(rb, NULL, NULL);
    printf("Received %zu message(s), %lu session(s) used\n", received, (unsigned long)rb->sessionCount);

    return rVal;
}

int main(int argc, char * argv[])
{
    int rVal = SUCCESS;
    bool gotArgs = true;
    int opt = 0;
    rb960x_t rb;
    rb960xStatus_t status = RB960X_OK;

    _serialDevice[0] = '\0';

    while ((opt = getopt_long(argc, argv, "d:b:t:Dh", _longOptions, NULL)) != -1)
    {
        switch (opt)
        {
            case 'd':
                strncpy(_serialDevice, optarg, PATH_MAX -1);
            break;

            case 'b':
                _baudRate = (uint32_t)strtol(optarg, NULL, 10);
            break;

            case 't':
                _timeoutS = (uint32_t)strtoul(optarg, NULL, 10);
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

    if ((rVal == SUCCESS) && (gotArgs == true) && (_serialDevice[0] == '\0'))
    {
        fprintf(stderr, "%s: a serial device is required (-d)\n", argv[0]);
        printHelp(argv[0]);
        rVal = -INVALID_ARGUMENTS;
    }

    if ((rVal == SUCCESS) && (gotArgs == true))
    {
        signal(SIGINT, stopHandler);

        // Open the serial port and initialise the RockBLOCK instance
        status = rb960xBegin(&rb, _serialDevice, _baudRate, (_debug == true) ? debugHandler : NULL, NULL);

        if (status == RB960X_OK)
        {
            // The 9604 can only receive in Satellite (SBD) mode. It's left in that mode afterwards.
            if ((rb.model == RB960X_MODEL_9604) && (rb.ratMode != RB960X_RAT_SBD))
            {
                printf("Switching the 9604 to Satellite mode (the module reboots)...\n");
                status = rb960xSwitchRat(&rb, RB960X_RAT_SBD);
                if (status != RB960X_OK)
                {
                    fprintf(stderr, "%s: failed to switch to Satellite mode: (status %d)\n", argv[0], (int)status);
                    rVal = -FAILED_TO_INITIALISE_RB_INSTANCE;
                }
            }

            if (status == RB960X_OK)
            {
                // Mailbox checks only start with a usable signal, so take a fresh reading first.
                printf("Waiting for valid signal..\n");
                if (waitForSignal(&rb) == true)
                {
                    rVal = -(int)listenForMessages(&rb, argv[0]);
                }
                else if (_run != 0)
                {
                    fprintf(stderr, "%s: timedout waiting for a valid SBD signal\n", argv[0]);
                    rVal = -FAILED_TO_GET_SIGNAL;
                }
            }

            rb960xEnd(&rb);
        }
        else if (status == RB960X_ERR_PORT)
        {
            fprintf(stderr, "%s: failed to open %s at %u baud: %s\n", argv[0], _serialDevice, (unsigned int)_baudRate, strerror(errno));
            rVal = -FAILED_TO_INITIALISE_SERIAL_PORT;
        }
        else
        {
            fprintf(stderr, "%s: failed to initialise rb960x instance\n", argv[0]);
            rVal = -FAILED_TO_INITIALISE_RB_INSTANCE;
        }
    }

    return rVal;
}
