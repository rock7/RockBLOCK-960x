/**
 * @file at_client.c
 * @brief Portable Hayes / 3GPP AT command client implementation.
 */

#include "at_client.h"
#include "util.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Chunk size for draining the port's read callback. */
#define AT_RX_CHUNK 32

/* Smallest line buffer atClientInit() will accept. */
#define AT_CLIENT_MIN_LINE_BUF_SIZE 16

/* Longest command accepted by atClientSendCmdFmt(). */
#ifndef AT_CLIENT_FMT_CMD_MAX
#define AT_CLIENT_FMT_CMD_MAX 256
#endif

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void atLock(const atClient_t *c)
{
    if (c != NULL)
    {
        if (c->port->lock != NULL)
        {
            c->port->lock(c->port->userData);
        }
    }
}

static void atUnlock(const atClient_t *c)
{
    if (c != NULL)
    {
        if (c->port->unlock != NULL)
        {
            c->port->unlock(c->port->userData);
        }
    }
}

static void atYield(const atClient_t *c)
{
    if (c != NULL)
    {
        if (c->port->yield != NULL)
        {
            c->port->yield(c->port->userData);
        }
    }
}

static uint32_t atNow(const atClient_t *c)
{
    uint32_t now = 0;

    if (c != NULL)
    {
        now = MILLIS();
    }

    return now;
}

static bool atExpired(const atClient_t *c, const uint32_t start, const uint32_t timeoutMs)
{
    bool expired = false;

    if (c != NULL)
    {
        const uint32_t now = (const uint32_t)atNow(c);
        const uint32_t diff  = now - start;

        if (diff >= timeoutMs)
        {
            expired = true;
        }
    }

    return expired;
}

static void atDebug(const atClient_t *c, atDebugDir_t dir, const void *data, size_t len)
{
    if (c != NULL)
    {
        if (c->onDebug != NULL)
        {
            c->onDebug(dir, (const char *)data, len, c->onDebugUserData);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Line handling                                                       */
/* ------------------------------------------------------------------ */

static bool dispatchUrc(const atClient_t *c, const char *line)
{
    bool handled = false;
    if (c != NULL)
    {
        for (size_t i = 0; i < AT_CLIENT_MAX_URC_HANDLERS; i++)
        {
            if ((c->urc[i].prefix != NULL) && (utilStartsWith(line, c->urc[i].prefix)))
            {
                c->urc[i].handler(line, c->urc[i].userData);
                handled = true;
            }
        }
    }
    return handled;
}

static void appendResponse(atClient_t *c, const char *line)
{
    size_t len = 0;

    if (c != NULL)
    {
        if ((c->respBuf != NULL) && (c->respSize > 0))
        {
            c->respOverflow = false;
            len = strlen(line);

            /* '\n' separator between information lines. */
            if (c->respLen > 0)
            {
                if ((c->respLen + 1) < c->respSize)
                {
                    c->respBuf[c->respLen++] = '\n';
                }
                else
                {
                    c->respOverflow = true;
                }
            }

            if (c->respOverflow != true)
            {
                if ((c->respLen + len) < c->respSize)
                {
                    memcpy(&c->respBuf[c->respLen], line, len);
                    c->respLen += len;
                }
                else
                {
                    /* Line doesn't fully fit: copy as much as fits,
                     * reserving one byte for the '\0' written below,
                     * and flag the overflow so the caller knows the
                     * response was truncated. */
                    const size_t avail = c->respSize - c->respLen - 1;
                    memcpy(&c->respBuf[c->respLen], line, avail);
                    c->respLen += avail;
                    c->respOverflow = true;
                }
            }

            c->respBuf[c->respLen] = '\0';
        }
    }
}

static void finishCmd(atClient_t *c, atStatus_t status)
{
    if (c != NULL)
    {
        c->gotFinal = true;
        c->finalStatus = status;
    }
}

/* Parse the numeric code of "+CME ERROR: 100" style finals. Verbose string
 * codes (AT+CMEE=2) yield -1. */
static int parseErrorCode(const char *line)
{
    int code = -1;
    const char *p = strchr(line, ':');
    char *end = NULL;

    if (p != NULL)
    {
        /* p points at the ':' itself, so +1 skips it to reach the code
         * (e.g. "+CME ERROR: 100" -> codePtr is " 100"; strtol() skips
         * the leading space on its own). */
        const char *codePtr = p + 1;
        long parsed = strtol(codePtr, &end, 10);

        /* Reject both "no digits parsed" and values outside int range
         * (e.g. strtol() clamps to LONG_MAX/MIN with errno = ERANGE on
         * overflow) rather than silently truncating via the cast. */
        if ((end != codePtr) && (parsed >= INT_MIN) && (parsed <= INT_MAX))
        {
            code = (int)parsed;
        }
    }

    return code;
}

static void handleLine(atClient_t *c, const char *line)
{
    if (c != NULL)
    {
        atDebug(c, AT_DEBUG_RX, line, strlen(line));

        /* Mid returns are undesired but in this case I think it makes
         * the code easier to follow. */

        /* Check for URCs */
        if (dispatchUrc(c, line))
        {
            return;
        }

        if (c->inCmd != true)
        {
            /* No command is pending, so this line is unsolicited but
             * didn't match a registered URC prefix above: hand it to
             * the catch-all line handler instead of treating it as
             * part of a command response. */
            if (c->onLine != NULL)
            {
                c->onLine(line, c->onLineUserData);
            }
            return;
        }

        /* Suppress the command echo (in case ATE0 is not in effect). */
        if ((c->pendingCmd != NULL) && (strcmp(line, c->pendingCmd) == 0))
        {
            return;
        }

        if (strcmp(line, "OK") == 0)
        {
            finishCmd(c, AT_OK);
        }
        else if (strcmp(line, "ERROR") == 0 || strcmp(line, "ABORTED") == 0 ||
                 strcmp(line, "NO CARRIER") == 0 || strcmp(line, "BUSY") == 0 ||
                 strcmp(line, "NO DIALTONE") == 0 ||
                 strcmp(line, "NO ANSWER") == 0)
        {
            finishCmd(c, AT_ERR_ERROR);
        }
        else if (utilStartsWith(line, "+CME ERROR"))
        {
            c->lastErrorCode = parseErrorCode(line);
            finishCmd(c, AT_ERR_CME);
        }
        else if (utilStartsWith(line, "+CMS ERROR"))
        {
            c->lastErrorCode = parseErrorCode(line);
            finishCmd(c, AT_ERR_CMS);
        }
        else if (utilStartsWith(line, "CONNECT"))
        {
            /* Data-mode entry (ATD, AT+USODL, ...) is a successful final. */
            appendResponse(c, line);
            finishCmd(c, AT_OK);
        }
        else
        {
            appendResponse(c, line);
        }
    }
}

static void processByte(atClient_t *c, char byte)
{
    if ((byte == '\r') || (byte == '\n'))
    {
        if (c->lineLen > 0)
        {
            c->lineBuf[c->lineLen] = '\0';
            handleLine(c, c->lineBuf);
            c->lineLen = 0;
            c->lineTruncated = false;
        }
    }
    else if ((c->lineLen + 1) < c->lineSize)
    {
        c->lineBuf[c->lineLen++] = byte;
    }
    else
    {
        /* Line longer than the buffer: keep what fits, drop the rest. */
        c->lineTruncated = true;
    }
}

/* Drain everything currently available from the port.
 * Returns bytes consumed, or a negative value on port error. */
static int pump(atClient_t *c)
{
    uint8_t chunk[AT_RX_CHUNK];
    int total = 0;
    bool reading = true;
    int n = -1;

    while (reading)
    {
        n = c->port->read(c->port->userData, chunk, sizeof(chunk));

        if (n < 0)
        {
            total = n;
            reading = false;
        }
        else if (n == 0)
        {
            reading = false;
        }
        else
        {
            for (int i = 0; i < n; i++)
            {
                processByte(c, (char)chunk[i]);
            }
            total += n;

            /* Stop early once the command completed so trailing bytes (e.g.
             * a URC right after "OK") stay buffered in the UART for later. */
            if ((c->inCmd == true) && (c->gotFinal == true))
            {
                reading = false;
            }
        }
    }

    return total;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

atStatus_t atClientInit(atClient_t *client, const atPort_t *port, char *lineBuf,
                        size_t lineBufSize)
{
    if ((client == NULL) || (port == NULL) || (port->write == NULL) ||
        (port->read == NULL) || (lineBuf == NULL) ||
        (lineBufSize < AT_CLIENT_MIN_LINE_BUF_SIZE))
    {
        return AT_ERR_PARAM;
    }

    memset(client, 0, sizeof(*client));
    client->port = port;
    client->lineBuf = lineBuf;
    client->lineSize = lineBufSize;
    client->lastErrorCode = -1;
    return AT_OK;
}

/* The result of a command once its final result code has arrived: the
 * final status, or AT_ERR_OVERFLOW if it was "OK" but the response didn't
 * fit. */
static atStatus_t finalResult(const atClient_t *c)
{
    atStatus_t status = c->finalStatus;

    if ((c->finalStatus == AT_OK) && (c->respOverflow))
    {
        status = AT_ERR_OVERFLOW;
    }

    return status;
}

static atStatus_t waitFinalLocked(atClient_t *c, const uint32_t timeoutMs)
{
    uint32_t start = 0;
    atStatus_t status = AT_OK;
    bool waiting = true;
    int n = -1;

    if (c != NULL)
    {
        start = atNow(c);

        while (waiting)
        {
            n = pump(c);
    
            if (n < 0)
            {
                status = AT_ERR_IO;
                waiting = false;
            }
            else if (c->gotFinal)
            {
                waiting = false;
            }
            else if (atExpired(c, start, timeoutMs))
            {
                status = AT_ERR_TIMEOUT;
                waiting = false;
            }
            else if (n == 0)
            {
                atYield(c);
            }
        }

        /* status is only still AT_OK here if we stopped because gotFinal
         * became true, never on IO error or timeout, so it's safe to
         * overwrite it with the command's actual final result. */
        if (status == AT_OK)
        {
            status = finalResult(c);
        }
    }

    return status;
}

static void beginCmd(atClient_t *c, const char *cmd, char *resp,
                     size_t respSize)
{
    if (c != NULL)
    {
        c->inCmd = true;
        c->pendingCmd = cmd;
        c->respBuf = resp;
        c->respSize = respSize;
        c->respLen = 0;
        c->respOverflow = false;
        c->gotFinal = false;
        c->finalStatus = AT_ERR_TIMEOUT;

        if ((resp != NULL) && (respSize > 0))
        {
            resp[0] = '\0';
        }
    }
}

static void endCmd(atClient_t *c)
{
    if (c != NULL)
    {
        c->inCmd = false;
        c->pendingCmd = NULL;
        c->respBuf = NULL;
        c->respSize = 0;
        c->asyncCmd = false;
    }
}

/* Claim the line and write cmd plus the terminator. The caller holds the
 * lock and has checked that no command is in flight. On return the client
 * is in the command (inCmd is true) even if the write failed. */
static atStatus_t startCmdLocked(atClient_t *c, const char *cmd, char *resp,
                                 size_t respSize)
{
    atStatus_t status = AT_OK;
    int rc = -1;

    /* Dispatch any URCs that arrived while idle before we claim the line. */
    (void)pump(c);

    beginCmd(c, cmd, resp, respSize);

    atDebug(c, AT_DEBUG_TX, cmd, strlen(cmd));

    rc = c->port->write(c->port->userData, cmd, strlen(cmd));
    if (rc >= 0)
    {
        rc = c->port->write(c->port->userData, AT_CLIENT_CMD_EOL,
                            strlen(AT_CLIENT_CMD_EOL));
    }

    if (rc < 0)
    {
        status = AT_ERR_IO;
    }

    return status;
}

atStatus_t atClientSendCmd(atClient_t *client, const char *cmd,
                           char *resp, size_t respSize,
                           uint32_t timeoutMs)
{
    atStatus_t status = AT_ERR_ERROR;

    if ((client == NULL) || (cmd == NULL))
    {
        return AT_ERR_PARAM;
    }

    /* Reject nested sends: this client has no command queue, and
     * interleaving would corrupt the in-flight response/URC parsing. */
    if (client->inCmd == true)
    {
        return AT_ERR_BUSY;
    }

    atLock(client);

    status = startCmdLocked(client, cmd, resp, respSize);
    if (status == AT_OK)
    {
        status = waitFinalLocked(client, timeoutMs);
    }

    endCmd(client);
    atUnlock(client);

    return status;
}

atStatus_t atClientSendCmdAsync(atClient_t *client, const char *cmd,
                                char *resp, size_t respSize,
                                uint32_t timeoutMs)
{
    atStatus_t status = AT_ERR_ERROR;

    if ((client == NULL) || (cmd == NULL))
    {
        status = AT_ERR_PARAM;
    }
    else if (client->inCmd == true)
    {
        status = AT_ERR_BUSY;
    }
    else
    {
        atLock(client);

        status = startCmdLocked(client, cmd, resp, respSize);
        if (status == AT_OK)
        {
            /* Leave the command in flight for atClientPollCmd(). */
            client->asyncCmd = true;
            client->asyncStartMs = atNow(client);
            client->asyncTimeoutMs = timeoutMs;
        }
        else
        {
            endCmd(client);
        }

        atUnlock(client);
    }

    return status;
}

atStatus_t atClientPollCmd(atClient_t *client)
{
    atStatus_t status = AT_PENDING;
    int n = -1;

    if ((client == NULL) || (client->inCmd != true) || (client->asyncCmd != true))
    {
        status = AT_ERR_PARAM;
    }
    else
    {
        atLock(client);

        n = pump(client);

        if (n < 0)
        {
            status = AT_ERR_IO;
        }
        else if (client->gotFinal)
        {
            status = finalResult(client);
        }
        else if (atExpired(client, client->asyncStartMs, client->asyncTimeoutMs))
        {
            status = AT_ERR_TIMEOUT;
        }

        if (status != AT_PENDING)
        {
            endCmd(client);
        }

        atUnlock(client);
    }

    return status;
}

atStatus_t atClientSendCmdFmt(atClient_t *client, char *resp,
                              size_t respSize, uint32_t timeoutMs,
                              const char *fmt, ...)
{
    char cmd[AT_CLIENT_FMT_CMD_MAX];
    va_list ap;
    int n = -1;
    atStatus_t status = AT_ERR_ERROR;

    if ((client != NULL) && (fmt != NULL))
    {
        va_start(ap, fmt);
        n = vsnprintf(cmd, sizeof(cmd), fmt, ap);
        va_end(ap);

        if ((n > 0) && ((size_t)n <= sizeof(cmd)))
        {
            status = atClientSendCmd(client, cmd, resp, respSize, timeoutMs);
        }
        else
        {
            status  = AT_ERR_PARAM;
        }
    }
    else
    {
        status = AT_ERR_PARAM;
    }

    return status;
}

atStatus_t atClientWaitFinal(atClient_t *client, char *resp,
                             size_t respSize, uint32_t timeoutMs)
{
    atStatus_t status = AT_ERR_ERROR;

    if (client == NULL)
    {
        status = AT_ERR_PARAM;
    }
    else if (client->inCmd)
    {
        status = AT_ERR_BUSY;
    }
    else
    {
        atLock(client);
        beginCmd(client, NULL, resp, respSize);
        status = waitFinalLocked(client, timeoutMs);
        endCmd(client);
        atUnlock(client);
    }
    return status;
}

atStatus_t atClientWaitFor(const atClient_t *client, const char *pattern,
                           uint32_t timeoutMs)
{
    size_t matched = 0;
    size_t patLen = 0;
    uint32_t start = 0;
    uint8_t byte = 0;
    int n = -1;
    atStatus_t status = AT_ERR_TIMEOUT;

    if ((client == NULL) || (pattern == NULL) || (pattern[0] == '\0'))
    {
        status = AT_ERR_PARAM;
    }
    else
    {
        patLen = strlen(pattern);

        atLock(client);
        start = atNow(client);

        while (atExpired(client, start, timeoutMs) != true)
        {
            n = client->port->read(client->port->userData, &byte, 1);

            if (n < 0)
            {
                atUnlock(client);
                status = AT_ERR_IO;
                break;
            }
            else if (n == 0)
            {
                atYield(client);
                continue;
            }

            if ((char)byte == pattern[matched])
            {
                matched++;
                if (matched == patLen)
                {
                    atUnlock(client);
                    status = AT_OK;
                    break;
                }
            }
            else
            {
                matched = ((char)byte == pattern[0]) ? 1 : 0;
            }
        }

        atUnlock(client);
    }

    return status;
}

int atClientWriteRaw(const atClient_t *client, const void *data, size_t len)
{
    int rc = -1;

    if ((client != NULL) && (data != NULL))
    {
        atDebug(client, AT_DEBUG_TX, data, len);
        rc = client->port->write(client->port->userData, data, len);
    }

    return rc;
}

int atClientReadRaw(const atClient_t *client, void *buf, size_t len,
                    uint32_t timeoutMs)
{
    uint32_t start = 0;
    int rc = -1;
    bool reading = true;

    if ((client != NULL) && (buf != NULL) && (len != 0))
    {

        start = atNow(client);
        while (reading)
        {
            int n = client->port->read(client->port->userData, buf, len);

            if (n != 0)
            {
                if (n > 0)
                {
                    atDebug(client, AT_DEBUG_RX, buf, (size_t)n);
                }
                rc = n;
                reading = false;
            }
            else if (atExpired(client, start, timeoutMs))
            {
                rc = 0;
                reading = false;
            }
            else
            {
                atYield(client);
            }
        }
    }

    return rc;
}

void atClientProcess(atClient_t *client)
{
    if ((client != NULL) && (client->inCmd != true))
    {
        atLock(client);
        (void)pump(client);
        atUnlock(client);
    }
}

atStatus_t atClientRegisterUrc(atClient_t *client, const char *prefix,
                               atLineHandler_t handler, void *userData)
{
    atStatus_t status = AT_ERR_PARAM;

    if ((client != NULL) && (prefix != NULL) && (prefix[0] != '\0') && (handler != NULL))
    {
        status = AT_ERR_OVERFLOW;

        for (size_t i = 0; i < AT_CLIENT_MAX_URC_HANDLERS; i++)
        {
            if (client->urc[i].prefix == NULL)
            {
                client->urc[i].prefix = prefix;
                client->urc[i].handler = handler;
                client->urc[i].userData = userData;
                status = AT_OK;
                break;
            }
        }
    }

    return status;
}

void atClientUnregisterUrc(atClient_t *client, const char *prefix)
{
    if ((client != NULL) && (prefix != NULL))
    {
        for (size_t i = 0; i < AT_CLIENT_MAX_URC_HANDLERS; i++)
        {
            if ((client->urc[i].prefix != NULL) &&
                (strcmp(client->urc[i].prefix, prefix) == 0))
            {
                client->urc[i].prefix = NULL;
                client->urc[i].handler = NULL;
                client->urc[i].userData = NULL;
            }
        }
    }
}

void atClientSetLineHandler(atClient_t *client, atLineHandler_t handler,
                            void *userData)
{
    if (client != NULL)
    {
        client->onLine = handler;
        client->onLineUserData = userData;
    }
}

void atClientSetDebugHandler(atClient_t *client, atDebugHandler_t handler,
                             void *userData)
{
    if (client != NULL)
    {
        client->onDebug = handler;
        client->onDebugUserData = userData;
    }
}

int atClientLastErrorCode(const atClient_t *client)
{
    return (client != NULL) ? client->lastErrorCode : -1;
}

const char *atStatusStr(atStatus_t status)
{
    switch (status)
    {
    case AT_OK:
        return "OK";
    case AT_ERR_ERROR:
        return "ERROR";
    case AT_ERR_CME:
        return "CME_ERROR";
    case AT_ERR_CMS:
        return "CMS_ERROR";
    case AT_ERR_TIMEOUT:
        return "TIMEOUT";
    case AT_ERR_IO:
        return "IO_ERROR";
    case AT_ERR_PARAM:
        return "INVALID_PARAM";
    case AT_ERR_OVERFLOW:
        return "OVERFLOW";
    case AT_ERR_BUSY:
        return "BUSY";
    case AT_PENDING:
        return "PENDING";
    default:
        return "UNKNOWN";
    }
}
