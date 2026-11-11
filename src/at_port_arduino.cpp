/**
 * @file at_port_arduino.cpp
 * @brief Arduino / PlatformIO port for the AT client: implements the
 *        portable API in at_port.h.
 *
 * Works with anything derived from Stream: HardwareSerial, SoftwareSerial,
 * USB CDC serial, etc. A Stream can't be opened at a given baud rate, so
 * bring the port up yourself (Serial1.begin(...)) before atPortOpen() or
 * rb960xBegin(); the baud argument is ignored here. The Stream is kept in
 * the handle's sys.ptr.
 */

#ifdef ARDUINO

#include "at_port.h"

#include <Arduino.h>

/* The Stream the port was opened on, or NULL once closed. */
static Stream *streamOf(const atPortHandle_t *p)
{
    return static_cast<Stream *>(p->sys.ptr);
}

static int arduinoWrite(void *userData, const void *data, size_t len)
{
    Stream *stream = streamOf(static_cast<atPortHandle_t *>(userData));
    if (stream == NULL)
    {
        return -1; /* closed */
    }
    size_t sent = stream->write(static_cast<const uint8_t *>(data), len);
    return (sent == len) ? static_cast<int>(sent) : -1;
}

static int arduinoRead(void *userData, void *buf, size_t len)
{
    Stream *stream = streamOf(static_cast<atPortHandle_t *>(userData));
    uint8_t *out = static_cast<uint8_t *>(buf);
    size_t n = 0;

    if (stream == NULL)
    {
        return -1; /* closed */
    }

    while (n < len && stream->available() > 0)
    {
        int c = stream->read();
        if (c < 0)
        {
            break;
        }
        out[n++] = static_cast<uint8_t>(c);
    }
    return static_cast<int>(n);
}

static void arduinoYield(void *userData)
{
    /* delay() also feeds the watchdog / runs background tasks on ESP cores. */
    (void)userData;
    delay(1);
}

int atPortOpen(atPortHandle_t *p, atPortDevice_t device, uint32_t baud)
{
    /* A Stream has no begin(); the caller has already started it. */
    (void)baud;

    if (p == NULL)
    {
        return -1;
    }

    p->sys.ptr = NULL;
    p->port.userData = p;
    p->port.write = arduinoWrite;
    p->port.read = arduinoRead;
    p->port.yield = arduinoYield;
    p->port.lock = NULL;
    p->port.unlock = NULL;

    if (device == NULL)
    {
        return -1;
    }

    p->sys.ptr = device;
    return 0;
}

void atPortClose(atPortHandle_t *p)
{
    /* The Stream belongs to the sketch, so it's left running. */
    if (p != NULL)
    {
        p->sys.ptr = NULL;
    }
}

const atPort_t *atPortGet(const atPortHandle_t *p)
{
    return (p != NULL) ? &p->port : NULL;
}

#endif /* ARDUINO */
