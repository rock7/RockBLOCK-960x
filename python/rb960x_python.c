/**
 * @file rb960x_python.c
 * @brief Small C helpers for the Python (ctypes) bindings.
 *
 * ctypes can call the driver's functions directly, but it can't see the C
 * headers. Mirroring rb960x_t in Python would break every time the struct
 * (or the atClient_t inside it) changed, so Python treats rb960x_t as an
 * opaque block of memory and uses these helpers to size it and read its
 * fields. The same goes for the compile-time limits in rockblock960x_config.h,
 * so Python always matches the build it loaded.
 *
 * Built into the rockblock960x_python shared library, along with the
 * driver itself; see CMakeLists.txt.
 */

#include "rockblock960x.h"

#include <stddef.h>

/* Windows exports everything via WINDOWS_EXPORT_ALL_SYMBOLS (CMake), and
 * GCC/Clang export everything by default, so nothing to mark here. */
#define RB960X_PY_API

/* ---- Sizes and build limits --------------------------------------------- */

RB960X_PY_API size_t rb960xPySizeof(void)
{
    return sizeof(rb960x_t);
}

RB960X_PY_API size_t rb960xPyMoMax(void)
{
    return (size_t)RB960X_MO_MAX;
}

RB960X_PY_API size_t rb960xPyMtMax(void)
{
    return (size_t)RB960X_MT_MAX;
}

RB960X_PY_API size_t rb960xPyIccidLen(void)
{
    return (size_t)RB960X_ICCID_LEN;
}

RB960X_PY_API size_t rb960xPyVersionLen(void)
{
    return (size_t)RB960X_MODEM_VERSION_LEN;
}

RB960X_PY_API uint32_t rb960xPySbdixTimeoutMs(void)
{
    return (uint32_t)RB960X_SBDIX_TIMEOUT_MS;
}

RB960X_PY_API uint32_t rb960xPySignalPollMs(void)
{
    return (uint32_t)RB960X_SIGNAL_POLL_MS;
}

RB960X_PY_API const char *rb960xPyVersion(void)
{
    return RB960X_VERSION_STRING;
}

/* ---- rb960x_t fields ---------------------------------------------------- */

RB960X_PY_API int rb960xPyModel(const rb960x_t *rb)
{
    return (rb != NULL) ? (int)rb->model : (int)RB960X_MODEL_UNKNOWN;
}

RB960X_PY_API int rb960xPyRatMode(const rb960x_t *rb)
{
    return (rb != NULL) ? (int)rb->ratMode : (int)RB960X_RAT_UNKNOWN;
}

RB960X_PY_API const char *rb960xPyImei(const rb960x_t *rb)
{
    return (rb != NULL) ? rb->imei : "";
}

RB960X_PY_API const char *rb960xPyCellularImei(const rb960x_t *rb)
{
    return (rb != NULL) ? rb->cellularImei : "";
}

RB960X_PY_API const char *rb960xPySerial(const rb960x_t *rb)
{
    return (rb != NULL) ? rb->serial : "";
}

RB960X_PY_API bool rb960xPyRingAlert(const rb960x_t *rb)
{
    return (rb != NULL) ? rb->ringAlert : false;
}

RB960X_PY_API bool rb960xPyMtWaiting(const rb960x_t *rb)
{
    return (rb != NULL) ? rb->mtWaiting : false;
}

RB960X_PY_API unsigned int rb960xPyMtQueued(const rb960x_t *rb)
{
    return (rb != NULL) ? (unsigned int)rb->mtQueued : 0U;
}

RB960X_PY_API bool rb960xPySessionPending(const rb960x_t *rb)
{
    return (rb != NULL) ? rb->sessionPending : false;
}

RB960X_PY_API uint32_t rb960xPySessionCount(const rb960x_t *rb)
{
    return (rb != NULL) ? rb->sessionCount : 0U;
}

/* ---- Raw AT commands ---------------------------------------------------- */

/* Send any AT command and return the at_client status (atStatus_t), for
 * poking at the modem from Python. Not part of the driver's API. */
RB960X_PY_API int rb960xPyCommand(rb960x_t *rb, const char *cmd, char *resp, size_t respSize, uint32_t timeoutMs)
{
    if (rb == NULL)
    {
        return (int)AT_ERR_PARAM;
    }
    return (int)atClientSendCmd(&rb->at, cmd, resp, respSize, timeoutMs);
}
