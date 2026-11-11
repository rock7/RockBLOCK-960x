/**
 * @file at_port_posix.c
 * @brief POSIX serial (termios) port for the AT client, used on Linux and
 *        macOS: implements the portable API in at_port.h.
 *
 * On macOS, open the callout device (/dev/cu.usbserial-XXXX), not the
 * dial-in one (/dev/tty.usbserial-XXXX): opening the dial-in device waits
 * for a carrier signal the RockBLOCK never sends.
 */

#include "at_port.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static int baudToFlag(uint32_t baud, speed_t *out)
{
    switch (baud)
    {
        case 9600:
            *out = B9600;
            break;
        case 19200:
            *out = B19200;
            break;
        case 38400:
            *out = B38400;
            break;
        case 57600:
            *out = B57600;
            break;
        case 115200:
            *out = B115200;
            break;
        case 230400:
            *out = B230400;
            break;
#if defined(B460800) && defined(B921600)
        /* Linux only: macOS's termios stops at 230400. */
        case 460800:
            *out = B460800;
            break;
        case 921600:
            *out = B921600;
            break;
#endif
        default:
            return -1;
    }
    return 0;
}

static int posixWrite(void *userData, const void *data, size_t len)
{
    atPortHandle_t *p = (atPortHandle_t *)userData;
    size_t sent = 0;

    while (sent < len)
    {
        ssize_t n = write(p->sys.fd, (const char *)data + sent, len - sent);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
        sent += (size_t)n;
    }
    return (int)sent;
}

static int posixRead(void *userData, void *buf, size_t len)
{
    atPortHandle_t *p = (atPortHandle_t *)userData;
    ssize_t n = read(p->sys.fd, buf, len);

    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        {
            return 0;
        }
        return -1;
    }
    return (int)n;
}

static void posixYield(void *userData)
{
    (void)userData;
    usleep(1000);
}

int atPortOpen(atPortHandle_t *p, atPortDevice_t device, uint32_t baud)
{
    struct termios tio;
    speed_t speed;

    if (p == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    /* Mark the port closed first, so atPortClose() is safe after any
     * failure below. */
    memset(p, 0, sizeof(*p));
    p->sys.fd = -1;

    if (device == NULL || baudToFlag(baud, &speed) != 0)
    {
        errno = EINVAL;
        return -1;
    }

    p->sys.fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (p->sys.fd < 0)
    {
        return -1;
    }

    if (tcgetattr(p->sys.fd, &tio) != 0)
    {
        atPortClose(p);
        return -1;
    }

    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~(unsigned)CRTSCTS;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    cfsetispeed(&tio, speed);
    cfsetospeed(&tio, speed);

    if (tcsetattr(p->sys.fd, TCSANOW, &tio) != 0)
    {
        atPortClose(p);
        return -1;
    }
    tcflush(p->sys.fd, TCIOFLUSH);

    p->port.userData = p;
    p->port.write = posixWrite;
    p->port.read = posixRead;
    p->port.yield = posixYield;
    p->port.lock = NULL;
    p->port.unlock = NULL;
    return 0;
}

void atPortClose(atPortHandle_t *p)
{
    if (p != NULL && p->sys.fd >= 0)
    {
        close(p->sys.fd);
        p->sys.fd = -1;
    }
}

const atPort_t *atPortGet(const atPortHandle_t *p)
{
    return (p != NULL) ? &p->port : NULL;
}
