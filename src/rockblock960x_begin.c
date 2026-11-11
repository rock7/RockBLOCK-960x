/**
 * @file rockblock960x_begin.c
 * @brief rb960xBegin() and rb960xEnd(): rb960xInit() on the platform's
 *        serial port.
 *
 * Kept out of rockblock960x.c because these call atPortOpen(), which only
 * exists where a port is built in (atclient_port in CMake, or the Arduino
 * port). Builds that supply their own callbacks, like the unit tests and
 * the Python bindings, leave this file out and use rb960xInit().
 */

#include "rockblock960x.h"
#include "at_port.h"

#include <string.h>

rb960xStatus_t rb960xBegin(rb960x_t *rb, atPortDevice_t device, uint32_t baud,
                             atDebugHandler_t debugHandler, void *debugUserData)
{
    rb960xStatus_t status = RB960X_ERR_ARG;

    if (rb != NULL)
    {
        memset(rb, 0, sizeof(*rb));

        if (atPortOpen(&rb->portHandle, device, baud) != 0)
        {
            status = RB960X_ERR_PORT;
        }
        else
        {
            rb->ownsPort = true;
            status = rb960xInit(rb, atPortGet(&rb->portHandle), debugHandler, debugUserData);

            if (status != RB960X_OK)
            {
                (void)rb960xEnd(rb);
            }
        }
    }

    return status;
}

rb960xStatus_t rb960xEnd(rb960x_t *rb)
{
    rb960xStatus_t status = RB960X_ERR_ARG;

    if (rb != NULL)
    {
        if (rb->ownsPort == true)
        {
            atPortClose(&rb->portHandle);
            rb->ownsPort = false;
        }

        rb->initialised = false;
        status = RB960X_OK;
    }

    return status;
}
