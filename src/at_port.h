/**
 * @file at_port.h
 * @brief Portable serial port API for the AT client.
 *
 * Application code uses only these names, so it builds unchanged on every
 * target. This header includes no platform headers, which is what lets
 * rockblock960x.h embed an atPortHandle_t in rb960x_t.
 *
 *   - atPortHandle_t: the port context. Allocate one (statically is fine)
 *     and treat its fields as private. The same type on every platform;
 *     each port keeps its own state in the @c sys union.
 *   - atPortDevice_t: what to open. Arduino: a Stream pointer such as
 *     &Serial1. Everywhere else: a device path or name, such as
 *     "/dev/ttyUSB0" on Linux or "/dev/cu.usbserial-XXXX" on macOS.
 *
 * Most applications don't use this directly: rb960xBegin() opens the port
 * inside the rb960x_t. To drive the AT client on its own:
 *
 *   atPortHandle_t serial;
 *   atClient_t client;
 *   char lineBuf[256];
 *
 *   if (atPortOpen(&serial, "/dev/ttyUSB0", 115200) != 0) { ... }
 *   atClientInit(&client, atPortGet(&serial), lineBuf, sizeof(lineBuf));
 *   ...
 *   atPortClose(&serial);
 *
 * Porting to a new target: implement the three functions below in a source
 * file, keeping the port's state in the @c sys union, and build only that
 * port into the program. Only one port is built into any one program, so
 * the function names never clash. If the target opens something other than
 * a path or a Stream, write a header that defines atPortDevice_t and build
 * with -DAT_PORT_HEADER="\"my_port.h\"". It must not include platform
 * headers either.
 */

#ifndef AT_PORT_H
#define AT_PORT_H

#include "at_client.h"

#include <stdint.h>

#if defined(AT_PORT_HEADER)
    #include AT_PORT_HEADER
#elif defined(ARDUINO)
    /* Forward declaration only: Arduino.h stays out of this header. */
    #ifdef __cplusplus
        class Stream;
        typedef Stream *atPortDevice_t;
    #else
        typedef struct Stream *atPortDevice_t;
    #endif
#else
    /** Device path or name to open, e.g. "/dev/ttyUSB0". */
    typedef const char *atPortDevice_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Serial port context. Treat the fields as private. */
typedef struct atPortHandle
{
    atPort_t port;      /**< Callback table; atPortGet() returns a pointer to it. */
    union
    {
        int fd;         /**< File descriptor (the POSIX port: Linux and macOS). */
        void *ptr;      /**< The platform's port object (Arduino: the Stream). */
    } sys;              /**< Platform state, used only by the port's own source file. */
} atPortHandle_t;

/**
 * @brief Open the serial port and fill in the callback table.
 *
 * @param[out] p       Port context to initialise.
 * @param[in]  device  What to open (see atPortDevice_t).
 * @param[in]  baud    Baud rate. Linux and macOS: 9600, 19200, 38400,
 *                     57600, 115200 or 230400; Linux also 460800 and
 *                     921600. Arduino: ignored; call begin() on the stream
 *                     before opening.
 *
 * @return 0 on success, -1 on failure (on Linux and macOS, errno says why).
 */
int atPortOpen(atPortHandle_t *p, atPortDevice_t device, uint32_t baud);

/** @brief Close the port. Safe to call more than once, or on a port that failed to open. */
void atPortClose(atPortHandle_t *p);

/** @brief Callback table to pass to atClientInit() or rb960xInit(). NULL if @p p is NULL. */
const atPort_t *atPortGet(const atPortHandle_t *p);

#ifdef __cplusplus
}
#endif

#endif /* AT_PORT_H */
