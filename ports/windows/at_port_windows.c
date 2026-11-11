/**
 * @file at_port_windows.c
 * @brief Windows serial (Win32 COM port) port for the AT client: implements
 *        the portable API in at_port.h.
 *
 * The device is a COM port name such as "COM3". Names from COM10 up only
 * open with the "\\.\" prefix, so it's added when missing; "\\.\COM3" is
 * also accepted as-is.
 *
 * Reads return straight away with whatever has arrived, like the POSIX
 * port's O_NONBLOCK: the AT client does its own timing.
 */

#include "at_port.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

/* Longest device name accepted, including the "\\.\" prefix and '\0'. */
#define WIN_PORT_NAME_MAX 64U

static HANDLE winHandle(const atPortHandle_t *p)
{
    return (HANDLE)p->sys.ptr;
}

static int winWrite(void *userData, const void *data, size_t len)
{
    atPortHandle_t *p = (atPortHandle_t *)userData;
    size_t sent = 0U;

    while (sent < len)
    {
        DWORD chunk = (len - sent > (size_t)MAXDWORD) ? MAXDWORD : (DWORD)(len - sent);
        DWORD n = 0;

        if (WriteFile(winHandle(p), (const char *)data + sent, chunk, &n, NULL) == FALSE)
        {
            return -1;
        }
        sent += (size_t)n;
    }
    return (int)sent;
}

static int winRead(void *userData, void *buf, size_t len)
{
    atPortHandle_t *p = (atPortHandle_t *)userData;
    DWORD want = (len > (size_t)MAXDWORD) ? MAXDWORD : (DWORD)len;
    DWORD n = 0;

    if (ReadFile(winHandle(p), buf, want, &n, NULL) == FALSE)
    {
        return -1;
    }
    return (int)n;
}

static void winYield(void *userData)
{
    (void)userData;
    Sleep(1);
}

int atPortOpen(atPortHandle_t *p, atPortDevice_t device, uint32_t baud)
{
    char name[WIN_PORT_NAME_MAX];
    DCB dcb;
    COMMTIMEOUTS timeouts;
    HANDLE h = INVALID_HANDLE_VALUE;
    int written = 0;

    if (p == NULL)
    {
        return -1;
    }

    /* Mark the port closed first, so atPortClose() is safe after any
     * failure below. */
    memset(p, 0, sizeof(*p));
    p->sys.ptr = INVALID_HANDLE_VALUE;

    if ((device == NULL) || (baud == 0U))
    {
        return -1;
    }

    written = (strncmp(device, "\\\\.\\", 4) == 0) ? snprintf(name, sizeof(name), "%s", device)
                                                   : snprintf(name, sizeof(name), "\\\\.\\%s", device);
    if ((written < 0) || ((size_t)written >= sizeof(name)))
    {
        return -1;
    }

    h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        return -1;
    }
    p->sys.ptr = h;

    /* 8N1, no hardware or software flow control, DTR and RTS held on:
     * the same as the POSIX port's raw mode without CRTSCTS. */
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (GetCommState(h, &dcb) == FALSE)
    {
        atPortClose(p);
        return -1;
    }
    dcb.BaudRate = (DWORD)baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fNull = FALSE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fAbortOnError = FALSE;
    if (SetCommState(h, &dcb) == FALSE)
    {
        atPortClose(p);
        return -1;
    }

    /* ReadFile returns at once with whatever is waiting, even nothing.
     * Writes wait as long as they need. */
    memset(&timeouts, 0, sizeof(timeouts));
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;
    if (SetCommTimeouts(h, &timeouts) == FALSE)
    {
        atPortClose(p);
        return -1;
    }
    (void)PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

    p->port.userData = p;
    p->port.write = winWrite;
    p->port.read = winRead;
    p->port.yield = winYield;
    p->port.lock = NULL;
    p->port.unlock = NULL;
    return 0;
}

void atPortClose(atPortHandle_t *p)
{
    if ((p != NULL) && (p->sys.ptr != NULL) && (p->sys.ptr != INVALID_HANDLE_VALUE))
    {
        (void)CloseHandle(winHandle(p));
        p->sys.ptr = INVALID_HANDLE_VALUE;
    }
}

const atPort_t *atPortGet(const atPortHandle_t *p)
{
    return (p != NULL) ? &p->port : NULL;
}
