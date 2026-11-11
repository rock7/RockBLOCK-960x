#include <stdio.h>
#include <string.h>
#include "rockblock960x.h"

#define RB960X_SERIAL_PORT "/dev/ttyUSB0"
// 115200 for the 9604. The 9602 and 9603 need 19200.
#define RB960X_BAUD_RATE 115200

// Simplest send: one attempt, no retry. The session also checks the
// gateway for MT messages, so read one if it brought one down.
int main(void)
{
    const char *message = "Hello World!";
    char mtBuffer[RB960X_MT_MAX];
    size_t mtLength = 0;
    rb960xSessionResult_t session = { 0 };
    rb960xStatus_t status;
    rb960x_t rb;

    status = rb960xBegin(&rb, RB960X_SERIAL_PORT, RB960X_BAUD_RATE, NULL, NULL);
    printf("Connected: %s\n", (status == RB960X_OK) ? "true" : "false");

    if (status == RB960X_OK)
    {
        // A 9604 must be in Satellite (SBD) mode.
        if ((rb.model == RB960X_MODEL_9604) && (rb.ratMode != RB960X_RAT_SBD))
        {
            status = rb960xSwitchRat(&rb, RB960X_RAT_SBD);
        }

        // Cheap checks and the last known signal (AT+CSQF).
        if (status == RB960X_OK)
        {
            status = rb960xSessionReady(&rb);
        }

        if (status == RB960X_OK)
        {
            status = rb960xWriteMoBuffer(&rb, message, strlen(message));
        }

        if (status == RB960X_OK)
        {
            printf("Sending MO SBD...\n");
            status = rb960xInitiateSession(&rb, false, RB960X_SBDIX_TIMEOUT_MS, &session);
        }

        if (status == RB960X_OK)
        {
            printf("Sent MO SBD (MOMSN %u)\n", (unsigned int)session.momsn);
        }
        else if (status == RB960X_ERR_NO_SIGNAL)
        {
            printf("Not enough signal to send\n");
        }
        else
        {
            printf("Send failed: status %d, MO status %u (%s)\n", (int)status, (unsigned int)session.moStatus,
                   rb960xMoStatusText(session.moResult));
        }

        if (((status == RB960X_OK) || (status == RB960X_ERR_SESSION)) && (session.mtStatus == 1U) &&
            (rb960xReadMtBuffer(&rb, mtBuffer, sizeof(mtBuffer), &mtLength) == RB960X_OK))
        {
            printf("Received MT SBD: %.*s\n", (int)mtLength, mtBuffer);
        }

        rb960xEnd(&rb);
    }

    return 0;
}
