#include "crossplatform.h"

#if defined(_WIN32)
int usleep(unsigned int microseconds)
{
    Sleep(microseconds / 1000); // Sleep takes milliseconds
    return 0;
}

char * stpncpy (char * dst, const char * src, size_t len)
{
    size_t n = strlen (src);
    if (n > len)
    {
        n = len;
    }

    return strncpy (dst, src, len) + n;
}

unsigned long millis(void)
{
    /* unsigned long is 32 bits on Windows, so this wraps every 49.7 days,
     * like Arduino's millis(). The driver's time maths allows for that. */
    return (unsigned long)GetTickCount64();
}

void delay(uint32_t ms)
{
    Sleep(ms);
}

#elif defined(__linux__) || defined(__APPLE__)

#include <time.h>
#include <unistd.h>

unsigned long millis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    /* CLOCK_MONOTONIC never goes negative, so the casts are safe. Do the
     * maths unsigned so it wraps like Arduino's millis() would. */
    return (unsigned long)ts.tv_sec * 1000UL + (unsigned long)ts.tv_nsec / 1000000UL;
}

void delay(uint32_t ms)
{
    usleep(ms * 1000); // usleep takes microseconds
}

#endif