/**
 * @file util.c
 * @brief Small internal helpers shared across the library's source files.
 */

#include "util.h"

#include <string.h>

bool utilStartsWith(const char *str, const char *prefix)
{
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

void utilStrCopy(char * const dst, const char * const src, const size_t dstSize)
{
    if ((dst != NULL) && (src != NULL) && (dstSize > 0U))
    {
        size_t i = 0U;

        while ((i < (dstSize - 1U)) && (src[i] != '\0'))
        {
            dst[i] = src[i];
            i++;
        }
        dst[i] = '\0';
    }
}
