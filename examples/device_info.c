#include "rockblock960x.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <limits.h>
#include <errno.h>
#include "crossplatform.h"

static char _serialDevice[PATH_MAX];
// Default baud rate. The 9604 uses 115200; the 9602 and 9603 need 19200 (-b 19200).
static uint32_t _baudRate = 115200;
static bool _debug = false;

typedef enum
{
    SUCCESS = 0,
    INVALID_ARGUMENTS,
    FAILED_TO_INITIALISE_SERIAL_PORT,
    FAILED_TO_INITIALISE_RB_INSTANCE,
} returnCode_t;

static struct option _longOptions[] =
{
    {"device", required_argument, 0, 'd'},
    {"baud", required_argument, 0, 'b'},
    {"debug", no_argument, 0, 'D'},
    {"help", no_argument, 0, 'h'},
    {0, 0, 0, 0}
};

static void printHelp(const char * progName)
{
    printf("Usage: %s -d <device> [-h]\n", progName);
    printf("  -d, --device   Serial device to use (mandatory)\n");
    printf("  -b, --baud     Serial Baud Rate default is 115200 bps\n");
    printf("  -D, --debug    Enable debug on the standard output\n");
    printf("  -h, --help     Display this help message\n");
}

static const char * modelStr(rb960xModel_t model)
{
    const char * str = "Unknown";

    switch (model)
    {
        case RB960X_MODEL_9602:
            str = "RockBLOCK 9602";
        break;

        case RB960X_MODEL_9603:
            str = "RockBLOCK 9603";
        break;

        case RB960X_MODEL_9604:
            str = "RockBLOCK 9604";
        break;

        default:
        break;
    }

    return str;
}

static const char * ratStr(rb960xRatMode_t rat)
{
    const char * str = "Unknown";

    switch (rat)
    {
        case RB960X_RAT_LTE_CAT_M1:
            str = "LTE Cat M1";
        break;

        case RB960X_RAT_SBD:
            str = "Satellite (SBD)";
        break;

        default:
        break;
    }

    return str;
}

static void debugHandler(atDebugDir_t dir, const char * data, size_t len, void * userData)
{
    (void)userData;
    printf("%s %.*s\n", (dir == AT_DEBUG_TX) ? "TX >" : "RX <", (int)len, data);
}

static void progressUpdate (void)
{
    static size_t progressCount = 0;
    if (_debug != true)
    {
        fprintf(stdout, "Reading device information");
        for (size_t i = 0; i < progressCount; i++)
        {
            fprintf(stdout, ".");
        }
        fprintf(stdout, "\r");
        fflush(stdout);
    }
    progressCount++;
}

int main(int argc, char * argv[])
{
    int rVal = SUCCESS;
    bool gotArgs = true;
    int opt = 0;
    rb960x_t rb;
    rb960xStatus_t status = RB960X_OK;
    char iccid[RB960X_ICCID_LEN] = "";
    char cellularVersion[RB960X_MODEM_VERSION_LEN] = "";
    char satelliteVersion[RB960X_MODEM_VERSION_LEN] = "";

    _serialDevice[0] = '\0';

    while ((opt = getopt_long(argc, argv, "d:b:Dh", _longOptions, NULL)) != -1)
    {
        switch (opt)
        {
            case 'd':
                strncpy(_serialDevice, optarg, PATH_MAX -1);
            break;

            case 'b':
                _baudRate = (uint32_t)strtol(optarg, NULL, 10);
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
        progressUpdate();

        // Open the serial port and initialise the RockBLOCK instance
        status = rb960xBegin(&rb, _serialDevice, _baudRate, _debug ? debugHandler : NULL, NULL);

        if (status == RB960X_OK)
        {
            progressUpdate();

            if (rb.model == RB960X_MODEL_9604)
            {
                rb960xRatMode_t lastMode = rb.ratMode;

                /* Only a failed RAT switch stops the reads below. A failed
                 * read (e.g. no SIM for the ICCID) is reported and skipped,
                 * so the remaining fields are still read. */
                if (rb.ratMode != RB960X_RAT_LTE_CAT_M1)
                {
                    status = rb960xSwitchRat(&rb, RB960X_RAT_LTE_CAT_M1);
                }

                if (status == RB960X_OK)
                {
                    progressUpdate();
                    if (rb960xGetCellularVersion(&rb, cellularVersion, sizeof(cellularVersion)) != RB960X_OK)
                    {
                        fprintf(stderr, "Failed to read Cellular firmware version\n");
                    }

                    progressUpdate();
                    if (rb960xGetCellularIccid(&rb, iccid, sizeof(iccid)) != RB960X_OK)
                    {
                        fprintf(stderr, "Failed to read Cellular ICCID\n");
                    }

                    progressUpdate();
                    status = rb960xSwitchRat(&rb, RB960X_RAT_SBD);
                }
                else
                {
                    fprintf(stderr, "Failed to switch to LTE Cat M1\n");
                }

                if (status == RB960X_OK)
                {
                    progressUpdate();
                    if (rb960xGetSatelliteVersion(&rb, satelliteVersion, sizeof(satelliteVersion)) != RB960X_OK)
                    {
                        fprintf(stderr, "Failed to read Satellite firmware version\n");
                    }
                }
                else if (rb.ratMode != RB960X_RAT_SBD)
                {
                    fprintf(stderr, "Failed to switch to Satellite (SBD)\n");
                }

                if (lastMode != rb.ratMode)
                {
                    progressUpdate();
                    status = rb960xSwitchRat(&rb, lastMode);
                }
            }
            else if ((rb.model == RB960X_MODEL_9602) || (rb.model == RB960X_MODEL_9603))
            {
                progressUpdate();
                if ((status = rb960xGetSatelliteVersion(&rb, satelliteVersion, sizeof(satelliteVersion))) != RB960X_OK)
                {
                    fprintf(stderr, "Failed to read Satellite firmware version\n");
                }
            }

            printf("Modem:                       %s\n", modelStr(rb.model));
            if (rb.model == RB960X_MODEL_9604)
            {
                printf("Serial:                      %s\n", rb.serial); // RB9604 has serial, RB9603 and RB9602 just gave IMEI
                printf("Current RAT:                 %s\n", ratStr(rb.ratMode));
                printf("Cellular IMEI:               %s\n", rb.cellularImei);
                printf("Cellular ICCID:              %s\n", iccid);
                printf("Cellular Firmware Version:   %s\n", cellularVersion);
                printf("Satellite IMEI:              %s\n", rb.imei);
                printf("Satellite Firmware Version:  %s\n", satelliteVersion);
            }
            else if ((rb.model == RB960X_MODEL_9602) || (rb.model == RB960X_MODEL_9603))
            {
                printf("Satellite IMEI:              %s\n", rb.imei);
                printf("Satellite Firmware Version:  %s\n", satelliteVersion);
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