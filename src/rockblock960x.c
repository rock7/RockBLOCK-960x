/**
 * @file rockblock960x.c
 * @brief RockBLOCK 9602 / 9603 / 9604 driver: implementation.
 *
 * Rules for this file (they keep the AVR and other embedded builds lean):
 *   - All AT command and response handling goes through at_client.h. Don't
 *     reimplement sending, line reading or timeouts here.
 *   - Avoid <stdio.h>: sscanf() and snprintf() pull vfprintf into AVR
 *     builds, which costs several KB of flash. Write small int/string
 *     helpers by hand for parsing at_client doesn't already cover (e.g. the
 *     +SBDIX fields).
 *   - Never block without a deadline. at_client's wait loops already time
 *     out, so pass timeouts through rather than adding new blocking loops.
 */
#include "rockblock960x.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

#if (RB960X_SBD_SESSION_TIMEOUT_S < 1) || (RB960X_SBD_SESSION_TIMEOUT_S > 65535)
#error "RB960X_SBD_SESSION_TIMEOUT_S must be 1 to 65535 seconds"
#endif

#if (RB960X_DTR_OPTION < 0) || (RB960X_DTR_OPTION > RB960X_DTR_MAX)
#error "RB960X_DTR_OPTION must be 0 to 3 (0 to 2 on the 9604)"
#endif

#if (RB960X_FLOW_CONTROL != RB960X_FLOW_DISABLED) && (RB960X_FLOW_CONTROL != RB960X_FLOW_RTS_CTS)
#error "RB960X_FLOW_CONTROL must be 0 (none) or 3 (RTS/CTS)"
#endif

#if (RB960X_INIT_RAT != RB960X_INIT_RAT_NONE) && (RB960X_INIT_RAT != RB960X_INIT_RAT_CELLULAR) && \
    (RB960X_INIT_RAT != RB960X_INIT_RAT_SBD)
#error "RB960X_INIT_RAT must be RB960X_INIT_RAT_NONE, RB960X_INIT_RAT_CELLULAR or RB960X_INIT_RAT_SBD"
#endif

#if (RB960X_MIN_SIGNAL_BARS < 0) || (RB960X_MIN_SIGNAL_BARS > 5)
#error "RB960X_MIN_SIGNAL_BARS must be 0 to 5"
#endif

/* ==========================================================================
 * Internal helpers
 * ========================================================================== */

/* Convert an at_client result to the driver's status codes: a timeout stays
 * a timeout, and any other failure becomes RB960X_ERR_PROTOCOL. */
static rb960xStatus_t rb960xStatusFromAt(atStatus_t at)
{
    rb960xStatus_t status = RB960X_OK;

    if (at == AT_ERR_TIMEOUT)
    {
        status = RB960X_ERR_TIMEOUT;
    }
    else if (at != AT_OK)
    {
        status = RB960X_ERR_PROTOCOL;
    }

    return status;
}

/* Read the unsigned decimal number at the start of str, e.g. 12 from
 * "12, 0". Returns false if str doesn't start with a digit or the number is
 * too big for an unsigned int. */
static bool rb960xParseUint(const char *str, unsigned int *value)
{
    bool ok = false;
    unsigned int result = 0U;
    unsigned int digit = 0U;

    if ((str != NULL) && (value != NULL) && (*str >= '0') && (*str <= '9'))
    {
        ok = true;

        while ((ok == true) && (*str >= '0') && (*str <= '9'))
        {
            digit = (unsigned int)(*str - '0');

            if (result > ((UINT_MAX - digit) / 10U))
            {
                ok = false;
            }
            else
            {
                result = (result * 10U) + digit;
                str++;
            }
        }
    }

    if (ok == true)
    {
        *value = result;
    }

    return ok;
}

/* Send "AT" up to RB960X_INIT_ATTEMPTS times until the modem responds. Just
 * after power-up the modem may still be booting or charging its
 * supercapacitor. */
static rb960xStatus_t rb960xProbeRetry(rb960x_t *rb)
{
    rb960xStatus_t status = RB960X_ERR_TIMEOUT;
    size_t attempt = 0;

    while ((status != RB960X_OK) && (attempt < RB960X_INIT_ATTEMPTS))
    {
        status = rb960xAtProbe(rb, RB960X_AT_PROBE_TIMEOUT_MS);
        attempt++;
    }

    return status;
}

/* Get the model from the ATI7 reply. Matches on a substring because the
 * reply contains other text, e.g. "BOOT07d4/9603NGrevA/04/RAW0f". */
static rb960xModel_t rb960xParseModel(const char *resp)
{
    rb960xModel_t model = RB960X_MODEL_UNKNOWN;

    if (strstr(resp, "9602") != NULL)
    {
        model = RB960X_MODEL_9602;
    }
    else if (strstr(resp, "9603") != NULL)
    {
        model = RB960X_MODEL_9603;
    }
    else if (strstr(resp, "9604") != NULL)
    {
        model = RB960X_MODEL_9604;
    }

    return model;
}

/* Get the RAT mode from the AT+URAT? reply, e.g. "+URAT: 10".
 * 7 = LTE Cat M1, 10 = SBD. */
static rb960xRatMode_t rb960xParseUrat(const char *resp)
{
    rb960xRatMode_t rat = RB960X_RAT_UNKNOWN;

    if (strstr(resp, "7") != NULL)
    {
        rat = RB960X_RAT_LTE_CAT_M1;
    }
    else if (strstr(resp, "10") != NULL)
    {
        rat = RB960X_RAT_SBD;
    }

    return rat;
}

/* Read the satellite serial number and IMEI on the 9604, which doesn't
 * support AT+GSN. Sends AT+USATSN, whose reply is
 * "+USATSN: <serial>[,<sbdImei>]", and splits it at the comma. If there's
 * no IMEI in the reply, imeiDst is left unchanged. */
static rb960xStatus_t rb960xFetchSatelliteSerialImei(rb960x_t *rb, char *serialDst, size_t serialDstSize, char *imeiDst, size_t imeiDstSize)
{
    char resp[RB960X_LINE_BUF_SIZE];
    char *serial = resp;
    char *colon = NULL;
    char *comma = NULL;
    char *imei = NULL;

    rb960xStatus_t status = rb960xAtUsatsn(rb, resp, sizeof(resp));

    if (status == RB960X_OK)
    {
        colon = strchr(resp, ':');

        if (colon != NULL)
        {
            serial = colon + 1;
            while (*serial == ' ')
            {
                serial++;
            }
        }

        comma = strchr(serial, ',');
        if (comma != NULL)
        {
            *comma = '\0';
            imei = comma + 1;
        }

        utilStrCopy(serialDst, serial, serialDstSize);
        utilStrCopy(imeiDst, imei, imeiDstSize);
    }

    return status;
}

/* The MO write commands format the message length into fixed buffers with
 * room for up to 5 digits. */
#if RB960X_MO_MAX > 65535
#error "RB960X_MO_MAX is too large (maximum 65535)"
#endif

/* Write value as decimal digits at dst (no NULL terminator) and return how
 * many characters were written. value must be 65535 or less, so at most 5
 * characters are written. Done by hand to avoid snprintf(). */
static size_t rb960xFormatUint(char *dst, size_t value)
{
    char digits[5];
    size_t nDigits = 0U;
    size_t pos = 0U;

    do
    {
        digits[nDigits] = (char)('0' + (value % 10U));
        nDigits++;
        value /= 10U;
    } while ((value > 0U) && (nDigits < sizeof(digits)));

    while (nDigits > 0U)
    {
        nDigits--;
        dst[pos] = digits[nDigits];
        pos++;
    }

    return pos;
}

/* Write all length bytes to the modem without a line terminator. */
static rb960xStatus_t rb960xWriteRaw(rb960x_t *rb, const void *data, size_t length)
{
    rb960xStatus_t status = RB960X_OK;

    if (atClientWriteRaw(&rb->at, data, length) != (int)length)
    {
        status = rb960xStatusFromAt(AT_ERR_IO);
    }

    return status;
}

/* 9604: write an MO message into the modem's buffer with
 * AT+SBDW=<length>,"<hex data>". The 9604 has no AT+SBDWB; it takes the
 * message hex encoded, two characters per byte, inside the command.
 *
 * The command is streamed out in small chunks so a 340-byte message (a
 * 700-character command line) doesn't need a large buffer. The reply is
 * "+SBDW:<status>[,<length>]" then "OK". Status codes:
 *   0 = written, 1 = data length doesn't match <length>,
 *   2 = invalid characters in <data>, 3 = <length> out of range.
 *
 * length must already be checked: 1 to RB960X_MO_MAX. */
static rb960xStatus_t rb960xWriteMoBufferHex(rb960x_t *rb, const char *data, size_t length)
{
    static const char hexDigits[] = "0123456789ABCDEF";
    rb960xStatus_t status = RB960X_OK;
    char buf[32];
    size_t pos = 0U;
    size_t i = 0U;
    uint8_t byte = 0U;
    char resp[RB960X_LINE_BUF_SIZE];
    const char *valuePtr = NULL;
    unsigned int result = 0U;

    /* "AT+SBDW=<length>,\"": at most 15 characters, so it fits in buf. */
    (void)strcpy(buf, "AT+SBDW=");
    pos = strlen(buf);
    pos += rb960xFormatUint(&buf[pos], length);
    buf[pos] = ',';
    pos++;
    buf[pos] = '"';
    pos++;

    for (i = 0U; (i < length) && (status == RB960X_OK); i++)
    {
        if ((pos + 2U) > sizeof(buf))
        {
            status = rb960xWriteRaw(rb, buf, pos);
            pos = 0U;
        }

        byte = (uint8_t)data[i];
        buf[pos] = hexDigits[byte >> 4];
        pos++;
        buf[pos] = hexDigits[byte & 0x0FU];
        pos++;
    }

    /* Closing quote and line terminator. */
    if ((status == RB960X_OK) && ((pos + 2U) > sizeof(buf)))
    {
        status = rb960xWriteRaw(rb, buf, pos);
        pos = 0U;
    }

    if (status == RB960X_OK)
    {
        buf[pos] = '"';
        pos++;
        buf[pos] = '\r';
        pos++;
        status = rb960xWriteRaw(rb, buf, pos);
    }

    if (status == RB960X_OK)
    {
        status = rb960xStatusFromAt(atClientWaitFinal(&rb->at, resp, sizeof(resp), RB960X_DEFAULT_TIMEOUT_MS));
    }

    if (status == RB960X_OK)
    {
        AT_VALUE_AFTER_COLON(valuePtr, resp);

        if (rb960xParseUint(valuePtr, &result) != true)
        {
            status = RB960X_ERR_PROTOCOL;
        }
        else if (result == 3U)
        {
            status = RB960X_ERR_TOO_LARGE;
        }
        else if (result != 0U)
        {
            status = RB960X_ERR_PROTOCOL;
        }
    }

    return status;
}

/* 9602 / 9603: write an MO message into the modem's buffer with
 * AT+SBDWB=<length>.
 *
 * AT+SBDWB doesn't fit atClientSendCmd(): the modem replies "READY" (not a
 * final result) and then waits for the data. So the command is written raw,
 * then:
 *   1. Wait for "READY".
 *   2. Write the <length> data bytes, then a 2-byte checksum: the low 16
 *      bits of the sum of all data bytes, high byte first.
 *   3. Wait for the result code and "OK". Result codes:
 *        0 = written, 1 = timed out waiting for the data,
 *        2 = checksum mismatch, 3 = length was 0 or too large.
 *      Result 1 isn't followed by "OK" (a long-standing modem quirk), so
 *      it shows up here as RB960X_ERR_TIMEOUT.
 *
 * length must already be checked: 1 to RB960X_MO_MAX. */
static rb960xStatus_t rb960xWriteMoBufferBinary(rb960x_t *rb, const char *data, size_t length)
{
    rb960xStatus_t status = RB960X_OK;
    char sbdwbCmd[16]; /* AT+SBDWB=nnnnn\r\0 */
    size_t pos = 0U;
    size_t i = 0U;
    uint16_t checksum = 0U;
    uint8_t checksumBytes[2];
    char resp[RB960X_LINE_BUF_SIZE];
    unsigned int result = 0U;

    (void)strcpy(sbdwbCmd, "AT+SBDWB=");
    pos = strlen(sbdwbCmd);
    pos += rb960xFormatUint(&sbdwbCmd[pos], length);
    sbdwbCmd[pos] = '\r';
    pos++;

    status = rb960xWriteRaw(rb, sbdwbCmd, pos);

    if (status == RB960X_OK)
    {
        status = rb960xStatusFromAt(atClientWaitFor(&rb->at, "READY\r\n", RB960X_DEFAULT_TIMEOUT_MS));
    }

    if (status == RB960X_OK)
    {
        for (i = 0U; i < length; i++)
        {
            checksum = (uint16_t)(checksum + (uint8_t)data[i]);
        }

        checksumBytes[0] = (uint8_t)(checksum >> 8);
        checksumBytes[1] = (uint8_t)(checksum & 0xFFU);

        status = rb960xWriteRaw(rb, data, length);

        if (status == RB960X_OK)
        {
            status = rb960xWriteRaw(rb, checksumBytes, sizeof(checksumBytes));
        }
    }

    if (status == RB960X_OK)
    {
        status = rb960xStatusFromAt(atClientWaitFinal(&rb->at, resp, sizeof(resp), RB960X_DEFAULT_TIMEOUT_MS));
    }

    if (status == RB960X_OK)
    {
        if (rb960xParseUint(resp, &result) != true)
        {
            status = RB960X_ERR_PROTOCOL;
        }
        else if (result == 3U)
        {
            status = RB960X_ERR_TOO_LARGE;
        }
        else if (result != 0U)
        {
            status = RB960X_ERR_PROTOCOL;
        }
    }

    return status;
}

/* Turn the reply to AT+SBDIX into a result.
 *
 * Reply: "+SBDIX: <MO status>, <MOMSN>, <MT status>, <MTMSN>,
 * <MT length>, <MT queued>". The 9604 guide shows the prefix as "+SBDI:",
 * so only the part after the colon is used.
 *
 * All six fields are parsed into @p result (which may be NULL). It's only
 * written when every field parses and is in range; otherwise the reply is
 * treated as a protocol error.
 *
 * An MO status of 0 to 4 means the message was sent; 5 or higher means it
 * wasn't. That's from the 9602/9603 guide. The 9604 guide only describes 3
 * to 14 as "location update failure" without saying whether the message
 * was sent, so the same 5-or-higher rule is used for both, to be safe. */
static rb960xStatus_t rb960xParseSbdixReply(const char *resp, rb960xSessionResult_t *result)
{
    /* Largest value each field can hold in rb960xSessionResult_t. */
    static const unsigned int fieldMax[6] = { UINT8_MAX, UINT16_MAX, UINT8_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX };
    unsigned int values[6] = { 0U };
    rb960xStatus_t status = RB960X_OK;
    const char *fields = NULL;
    const char *start = NULL;
    const char *end = NULL;
    unsigned int i = 0U;

    AT_VALUE_AFTER_COLON(fields, resp);

    if (fields == NULL)
    {
        status = RB960X_ERR_PROTOCOL;
    }

    for (i = 0U; (i < 6U) && (status == RB960X_OK); i++)
    {
        AT_GET_FIELD(fields, i, start, end);
        (void)end;

        if (start == NULL)
        {
            status = RB960X_ERR_PROTOCOL;
        }
        else
        {
            AT_SKIP_WS(start);

            if ((rb960xParseUint(start, &values[i]) != true) || (values[i] > fieldMax[i]))
            {
                status = RB960X_ERR_PROTOCOL;
            }
        }
    }

    if (status == RB960X_OK)
    {
        if (result != NULL)
        {
            result->moStatus = (uint8_t)values[0];
            result->momsn = (uint16_t)values[1];
            result->mtStatus = (uint8_t)values[2];
            result->mtmsn = (uint16_t)values[3];
            result->mtLength = (uint16_t)values[4];
            result->mtQueued = (uint16_t)values[5];
        }

        if (values[0] >= 5U)
        {
            status = RB960X_ERR_SESSION;
        }
    }

    return status;
}

/* Checks that don't need the modem: the model is known, a 9604 is in SBD
 * mode, and no asynchronous session is running. Shared by every SBD
 * function so they always agree. */
static rb960xStatus_t rb960xCheckSessionPreconditions(const rb960x_t *rb)
{
    rb960xStatus_t status = RB960X_OK;

    if (rb->sessionPending == true)
    {
        /* The AT client is mid-command, so anything sent now would land
         * in the middle of the running session. */
        status = RB960X_ERR_BUSY;
    }
    else if (rb->model == RB960X_MODEL_UNKNOWN)
    {
        status = RB960X_ERR_INVALID_MODEL;
    }
    else if ((rb->model == RB960X_MODEL_9604) && (rb->ratMode != RB960X_RAT_SBD))
    {
        status = RB960X_ERR_WRONG_RAT;
    }
    else
    {
        /* Ready to talk to the modem. */
    }

    return status;
}

/* SBDRING URC: the gateway has an MT message for us (ring alerts turned
 * on by AT+SBDMTA=1, or AT+URING=1 on the 9604, in rb960xInit()). */
static void rb960xOnSbdring(const char *line, void *userData)
{
    rb960x_t *rb = (rb960x_t *)userData;

    (void)line;
    rb->ringAlert = true;
    /* Reported by rb960xProcess(): this can run mid-command. */
    rb->ringNotify = true;
}

/* Record what a session said about MT messages. Called for every parsed
 * session result, whether or not the MO part worked. MT status 1 means a
 * message is now in the MT buffer. MT status 2 means the gateway couldn't
 * be checked, so any ring alert is still unanswered and the queued count
 * from before still stands. */
static void rb960xNoteSession(rb960x_t *rb, const rb960xSessionResult_t *session)
{
    if (session->mtStatus == 1U)
    {
        rb->mtWaiting = true;
    }

    if (session->mtStatus != 2U)
    {
        rb->ringAlert = false;
        rb->mtQueued = session->mtQueued;
    }
}

/* Decode a raw +SBDIX MO status for the model, following that model's AT
 * command guide (see rb960xMoStatus_t). The guides agree on every code
 * they both define; the 9602/9603 guide just defines more of them. Named
 * enum values equal the raw code, so a defined code maps to itself. */
static rb960xMoStatus_t rb960xDecodeMoStatus(rb960xModel_t model, uint8_t moStatus)
{
    bool defined = false;
    rb960xMoStatus_t moResult = RB960X_MO_RESERVED_FAILURE;

    if ((moStatus == 0U) || (moStatus == 2U) || ((moStatus >= 15U) && (moStatus <= 18U)) ||
        ((moStatus >= 32U) && (moStatus <= 38U)) || (moStatus == 65U))
    {
        /* Defined in both guides. */
        defined = true;
    }
    else if (model != RB960X_MODEL_9604)
    {
        /* Defined in the 9602/9603 guide only. */
        defined = ((moStatus == 1U) || ((moStatus >= 10U) && (moStatus <= 14U)) || (moStatus == 19U) ||
                   (moStatus == 64U));
    }
    else
    {
        /* 9604: nothing more is defined. */
    }

    if (defined == true)
    {
        moResult = (rb960xMoStatus_t)moStatus;
    }
    else if ((model == RB960X_MODEL_9604) && (moStatus >= 3U) && (moStatus <= 14U))
    {
        moResult = RB960X_MO_RESERVED_LOCATION_UPDATE_FAILURE;
    }
    else if (moStatus <= 4U)
    {
        /* 9602/9603: 3 and 4. 9604: 1, which its guide lists only for
         * AT+SBDI ("sent successfully"). */
        moResult = RB960X_MO_RESERVED_SUCCESS;
    }
    else
    {
        /* Every other undefined code: stays RB960X_MO_RESERVED_FAILURE. */
    }

    return moResult;
}

/* Parse a session's reply into session and record it. */
static rb960xStatus_t rb960xSessionFinish(rb960x_t *rb, const char *resp, rb960xSessionResult_t *session)
{
    rb960xStatus_t status = RB960X_OK;

    (void)memset(session, 0, sizeof(*session));
    status = rb960xParseSbdixReply(resp, session);

    if ((status == RB960X_OK) || (status == RB960X_ERR_SESSION))
    {
        session->moResult = rb960xDecodeMoStatus(rb->model, session->moStatus);
        rb960xNoteSession(rb, session);
    }

    return status;
}

/* Everything rb960xInitiateSession() and rb960xInitiateSessionAsync() do
 * before sending the session command: the local checks, the MT guard, and
 * AT+SBDST (the modem forgets it at power-up, so it's set every time).
 * Picks the command: AT+SBDIXA to answer a ring alert, AT+SBDIX otherwise;
 * the 9604 has no AT+SBDIXA. */
static rb960xStatus_t rb960xSessionPrepare(rb960x_t *rb, bool answerRing, const char **cmd)
{
    rb960xStatus_t status = rb960xCheckSessionPreconditions(rb);

    if ((status == RB960X_OK) && (rb->mtWaiting == true))
    {
        /* A session empties the MT buffer: don't lose the message. */
        status = RB960X_ERR_MT_UNREAD;
    }

    if (status == RB960X_OK)
    {
        status = rb960xAtSbdst(rb, (uint16_t)RB960X_SBD_SESSION_TIMEOUT_S);
    }

    *cmd = ((answerRing == true) && (rb->model != RB960X_MODEL_9604)) ? "AT+SBDIXA" : "AT+SBDIX";

    return status;
}

/* Read exactly length raw bytes into buf, bypassing the line parser.
 * Gives up if no byte arrives for RB960X_DEFAULT_TIMEOUT_MS. */
static rb960xStatus_t rb960xReadRawExact(rb960x_t *rb, void *buf, size_t length)
{
    rb960xStatus_t status = RB960X_OK;
    uint8_t *out = (uint8_t *)buf;
    size_t got = 0U;
    int n = 0;

    while ((got < length) && (status == RB960X_OK))
    {
        n = atClientReadRaw(&rb->at, &out[got], length - got, RB960X_DEFAULT_TIMEOUT_MS);

        if (n == 0)
        {
            status = RB960X_ERR_TIMEOUT;
        }
        else if (n < 0)
        {
            status = rb960xStatusFromAt(AT_ERR_IO);
        }
        else
        {
            got += (size_t)n;
        }
    }

    return status;
}

/* 9602 / 9603: read the MT buffer with AT+SBDRB.
 *
 * The reply is binary, straight after the command (echo is off): a 2-byte
 * length, that many message bytes, then a 2-byte checksum (the low 16 bits
 * of the sum of the message bytes), all high byte first. Then "OK".
 *
 * A message too big for buf is read and thrown away, so the reply is still
 * consumed in full. On a checksum mismatch rb->mtWaiting stays set and the
 * next call reads the buffer again. */
static rb960xStatus_t rb960xReadMtBufferBinary(rb960x_t *rb, char *buf, size_t bufSize, size_t *length)
{
    static const char cmd[] = "AT+SBDRB\r";
    rb960xStatus_t status = RB960X_OK;
    uint8_t header[2];
    uint8_t checksumBytes[2];
    uint8_t discard[16];
    size_t msgLength = 0U;
    size_t done = 0U;
    size_t chunk = 0U;
    size_t i = 0U;
    uint16_t checksum = 0U;
    bool tooLarge = false;

    status = rb960xWriteRaw(rb, cmd, sizeof(cmd) - 1U);

    if (status == RB960X_OK)
    {
        status = rb960xReadRawExact(rb, header, sizeof(header));
    }

    if (status == RB960X_OK)
    {
        msgLength = ((size_t)header[0] << 8) | (size_t)header[1];
        tooLarge = (msgLength > bufSize);

        while ((done < msgLength) && (status == RB960X_OK))
        {
            if (tooLarge == true)
            {
                chunk = msgLength - done;
                if (chunk > sizeof(discard))
                {
                    chunk = sizeof(discard);
                }
                status = rb960xReadRawExact(rb, discard, chunk);
                for (i = 0U; (i < chunk) && (status == RB960X_OK); i++)
                {
                    checksum = (uint16_t)(checksum + discard[i]);
                }
            }
            else
            {
                chunk = msgLength;
                status = rb960xReadRawExact(rb, buf, chunk);
                for (i = 0U; (i < chunk) && (status == RB960X_OK); i++)
                {
                    checksum = (uint16_t)(checksum + (uint8_t)buf[i]);
                }
            }
            done += chunk;
        }
    }

    if (status == RB960X_OK)
    {
        status = rb960xReadRawExact(rb, checksumBytes, sizeof(checksumBytes));
    }

    if (status == RB960X_OK)
    {
        status = rb960xStatusFromAt(atClientWaitFinal(&rb->at, NULL, 0U, RB960X_DEFAULT_TIMEOUT_MS));
    }

    if (status == RB960X_OK)
    {
        if (checksum != (uint16_t)(((uint16_t)checksumBytes[0] << 8) | checksumBytes[1]))
        {
            status = RB960X_ERR_PROTOCOL;
        }
        else
        {
            /* Read (or thrown away): either way it's been dealt with. */
            rb->mtWaiting = false;

            if (tooLarge == true)
            {
                status = RB960X_ERR_TOO_LARGE;
            }
            else if (msgLength == 0U)
            {
                status = RB960X_ERR_NO_MESSAGE;
            }
            else
            {
                *length = msgLength;
            }
        }
    }

    return status;
}

/* Value of a hex digit, or -1 if c isn't one. */
static int rb960xHexValue(char c)
{
    int value = -1;

    if ((c >= '0') && (c <= '9'))
    {
        value = c - '0';
    }
    else if ((c >= 'A') && (c <= 'F'))
    {
        value = (c - 'A') + 10;
    }
    else if ((c >= 'a') && (c <= 'f'))
    {
        value = (c - 'a') + 10;
    }
    else
    {
        /* Not a hex digit. */
    }

    return value;
}

/* 9604: read the MT buffer with AT+SBDR. The reply is one text line,
 * +SBDR:<length>,"<hex data>", then OK. A full 270-byte message makes a
 * 550-character line, far longer than the line buffer, so the reply is
 * read raw: the header a byte at a time, then the hex data, decoded
 * straight into buf. The closing quote and OK are left to
 * atClientWaitFinal().
 *
 * A message too big for buf is read and thrown away. On a malformed reply
 * rb->mtWaiting stays set and the next call reads the buffer again (AT+SBDR
 * doesn't empty the MT buffer). */
static rb960xStatus_t rb960xReadMtBufferHex(rb960x_t *rb, char *buf, size_t bufSize, size_t *length)
{
    static const char cmd[] = "AT+SBDR\r";
    static const char prefix[] = "+SBDR:";
    /* Bytes to search for the prefix before giving up: the blank line
     * before it, plus room for a stray URC. */
    static const size_t scanMax = 64U;
    rb960xStatus_t status = RB960X_OK;
    char line[24];
    char chunk[32];
    char c = '\0';
    size_t lineLen = 0U;
    size_t matched = 0U;
    size_t scanned = 0U;
    size_t msgLength = 0U;
    size_t hexLeft = 0U;
    size_t n = 0U;
    size_t i = 0U;
    size_t out = 0U;
    int nibble = 0;
    int high = -1;
    bool haveDigit = false;
    bool tooLarge = false;
    bool errorReply = false;

    status = rb960xWriteRaw(rb, cmd, sizeof(cmd) - 1U);

    /* Find "+SBDR:", watching for an ERROR reply instead. */
    while ((status == RB960X_OK) && (matched < (sizeof(prefix) - 1U)))
    {
        status = rb960xReadRawExact(rb, &c, 1U);
        scanned++;

        if (status != RB960X_OK)
        {
            /* Timed out or failed. */
        }
        else if ((c == '\r') || (c == '\n'))
        {
            line[lineLen] = '\0';

            if (strstr(line, "ERROR") != NULL) /* ERROR or +CME ERROR: <n> */
            {
                errorReply = true;
                status = RB960X_ERR_PROTOCOL;
            }
            lineLen = 0U;
            matched = 0U;
        }
        else
        {
            if (lineLen < (sizeof(line) - 1U))
            {
                line[lineLen] = c;
                lineLen++;
            }

            if (c == prefix[matched])
            {
                matched++;
            }
            else
            {
                matched = (c == prefix[0]) ? 1U : 0U;
            }
        }

        if ((status == RB960X_OK) && (matched < (sizeof(prefix) - 1U)) && (scanned >= scanMax))
        {
            status = RB960X_ERR_PROTOCOL;
        }
    }

    /* <length>, up to the comma. */
    while ((status == RB960X_OK) && (c != ','))
    {
        status = rb960xReadRawExact(rb, &c, 1U);

        if (status != RB960X_OK)
        {
            /* Timed out or failed. */
        }
        else if ((c >= '0') && (c <= '9'))
        {
            haveDigit = true;
            msgLength = (msgLength * 10U) + (size_t)(c - '0');

            if (msgLength > 0xFFFFU)
            {
                status = RB960X_ERR_PROTOCOL;
            }
        }
        else if ((c == ',') && (haveDigit == true))
        {
            /* End of <length>. */
        }
        else if ((c == ' ') && (haveDigit != true))
        {
            /* Space after the colon. */
        }
        else
        {
            status = RB960X_ERR_PROTOCOL;
        }
    }

    /* Skip the opening quote (and any space) up to the first hex digit. */
    if ((status == RB960X_OK) && (msgLength > 0U))
    {
        do
        {
            status = rb960xReadRawExact(rb, &c, 1U);
        } while ((status == RB960X_OK) && ((c == ' ') || (c == '"')));
    }

    /* The hex data: c already holds the first digit. */
    if (status == RB960X_OK)
    {
        tooLarge = (msgLength > bufSize);
        hexLeft = msgLength * 2U;
        chunk[0] = c;
        n = (hexLeft > 0U) ? 1U : 0U;

        while ((status == RB960X_OK) && (hexLeft > 0U))
        {
            for (i = 0U; (i < n) && (status == RB960X_OK); i++)
            {
                nibble = rb960xHexValue(chunk[i]);

                if (nibble < 0)
                {
                    status = RB960X_ERR_PROTOCOL;
                }
                else if (high < 0)
                {
                    high = nibble;
                }
                else
                {
                    if (tooLarge != true)
                    {
                        buf[out] = (char)(uint8_t)((high << 4) | nibble);
                    }
                    out++;
                    high = -1;
                }
            }

            hexLeft -= n;
            n = (hexLeft < sizeof(chunk)) ? hexLeft : sizeof(chunk);

            if ((status == RB960X_OK) && (n > 0U))
            {
                status = rb960xReadRawExact(rb, chunk, n);
            }
        }
    }

    /* Consume the rest of the reply, up to OK, unless it was ERROR (already
     * read). Done even after a bad reply, to keep the AT client in step. */
    if (errorReply != true)
    {
        rb960xStatus_t finalStatus = rb960xStatusFromAt(atClientWaitFinal(&rb->at, NULL, 0U, RB960X_DEFAULT_TIMEOUT_MS));

        if (status == RB960X_OK)
        {
            status = finalStatus;
        }
    }

    if (status == RB960X_OK)
    {
        /* Read (or thrown away): either way it's been dealt with. */
        rb->mtWaiting = false;

        if (tooLarge == true)
        {
            status = RB960X_ERR_TOO_LARGE;
        }
        else if (msgLength == 0U)
        {
            status = RB960X_ERR_NO_MESSAGE;
        }
        else
        {
            *length = msgLength;
        }
    }

    return status;
}

/* Settings rb960xInit() makes, and rb960xSwitchRat() makes again after
 * the module resets: the DTR option, flow control and the ring alert URC.
 * The 9604 doesn't support AT+SBDMTA (it replies ERROR); in Satellite mode
 * a non-zero AT+URING enables its incoming message URC instead. */
static rb960xStatus_t rb960xApplySettings(rb960x_t *rb)
{
    rb960xStatus_t status = rb960xAtDn(rb, RB960X_DTR_OPTION);

    if (status == RB960X_OK)
    {
        status = rb960xAtKn(rb, RB960X_FLOW_CONTROL);
    }

    if (status == RB960X_OK)
    {
        if (rb->model == RB960X_MODEL_9604)
        {
            status = rb960xStatusFromAt(atClientSendCmd(&rb->at, "AT+URING=1", NULL, 0, RB960X_DEFAULT_TIMEOUT_MS));
        }
        else
        {
            status = rb960xAtSbdMta(rb, true);
        }
    }

    return status;
}

/* Switch the 9604's RAT (see rb960xSwitchRat()) without restoring the
 * settings afterwards. rb960xInit() uses this, as it makes the settings
 * straight after. */
static rb960xStatus_t rb960xSwitchRatOnly(rb960x_t *rb, rb960xRatMode_t mode)
{
    rb960xStatus_t status = RB960X_ERR_ARG;
    uint8_t probeCount = 0;
    char resp[RB960X_LINE_BUF_SIZE];

    if ((rb != NULL) &&
        ((mode == RB960X_RAT_LTE_CAT_M1 || mode == RB960X_RAT_SBD)))
    {
        /* Only the 9604 supports RAT switching. */
        if (rb->model == RB960X_MODEL_9604)
        {
            status = RB960X_OK;

            if (rb->ratMode != mode)
            {
                /* The RAT can only be changed with the radio off, and
                 * only takes effect after a reset. */
                status = rb960xAtCfun(rb, (uint8_t)RB960X_CFUN_0);
                if (status == RB960X_OK)
                {
                    status = rb960xAtUrat(rb, (mode == RB960X_RAT_LTE_CAT_M1) ? 7 : 10);

                    if (status != RB960X_OK)
                    {
                        /* Don't leave the radio off; the error from
                         * AT+URAT is the one reported. */
                        (void)rb960xAtCfun(rb, (uint8_t)RB960X_CFUN_1);
                    }
                }

                if (status == RB960X_OK)
                {
                    status = rb960xAtCfun(rb, (uint8_t)RB960X_CFUN_16);
                }

                /* Wait for the module to come back after the reset. */
                if (status == RB960X_OK)
                {
                    status = rb960xAtProbe(rb, 100);

                    while ((status != RB960X_OK) && (++probeCount < 20U))
                    {
                        status = rb960xAtProbe(rb, 100);
                    }
                }

                /* The reset can turn echo back on, so turn it off again,
                 * then read the RAT back to confirm the switch. */
                if (status == RB960X_OK)
                {
                    status = rb960xAtEcho(rb, false);
                }

                if (status == RB960X_OK)
                {
                    status = rb960xAtReadUrat(rb, resp, sizeof(resp));
                }

                if (status == RB960X_OK)
                {
                    rb->ratMode = rb960xParseUrat(resp);

                    if (rb->ratMode != mode)
                    {
                        /* Every command worked, but the module didn't
                         * change mode. */
                        status = RB960X_ERR_PROTOCOL;
                    }
                }
            }
        }
        else
        {
            status = RB960X_ERR_INVALID_MODEL;
        }
    }

    return status;
}

/* ==========================================================================
 * High-level API (documented in rockblock960x.h)
 * ========================================================================== */

rb960xStatus_t rb960xInit(rb960x_t *rb, const atPort_t *port,
                            atDebugHandler_t debugHandler, void *debugUserData)
{
    rb960xStatus_t status = RB960X_ERR_ARG;

    if ((rb != NULL) && (port != NULL))
    {
        /* Called from rb960xBegin() on the port it opened inside rb: keep
         * that port across the clear. Otherwise clear everything, so
         * rb960xEnd() won't close a port this context doesn't own. */
        bool ownPort = (port == &rb->portHandle.port);
        atPortHandle_t handle;
        bool ownsPort = false;

        (void)memset(&handle, 0, sizeof(handle));

        if (ownPort == true)
        {
            handle = rb->portHandle;
            ownsPort = rb->ownsPort;
        }

        memset(rb, 0, sizeof(*rb));

        if (ownPort == true)
        {
            rb->portHandle = handle;
            rb->ownsPort = ownsPort;
        }

        if (atClientInit(&rb->at, port, rb->lineBuf, sizeof(rb->lineBuf)) != AT_OK)
        {
            status = RB960X_ERR_HAL;
        }
        else
        {
            /* Set up before the first "AT" so the caller's debug handler
             * also sees the commands rb960xInit() sends itself. */
            atClientSetDebugHandler(&rb->at, debugHandler, debugUserData);
            status = rb960xProbeRetry(rb);
        }

        if (status == RB960X_OK)
        {
            /* Turn echo off before anything else. With echo on, the
             * modem repeats each command back, and at_client only filters
             * out an exact copy of the command it just sent, so echoed
             * text could end up in the replies below. */
            status = rb960xAtEcho(rb, false);
        }

        if (status == RB960X_OK)
        {
            char resp[RB960X_LINE_BUF_SIZE];

            status = rb960xAtI7(rb, resp, sizeof(resp));

            if (status == RB960X_OK)
            {
                rb->model = rb960xParseModel(resp);
            }
        }

        if (status == RB960X_OK)
        {
            char resp[RB960X_LINE_BUF_SIZE];

            status = rb960xAtCgsn(rb, resp, sizeof(resp));

            if (status == RB960X_OK)
            {
                /* On the 9604, AT+CGSN gives the cellular IMEI. The
                 * satellite IMEI comes from AT+USATSN below. */
                if (rb->model == RB960X_MODEL_9604)
                {
                    utilStrCopy(rb->cellularImei, resp, sizeof(rb->cellularImei));
                }
                else
                {
                    utilStrCopy(rb->imei, resp, sizeof(rb->imei));
                }
            }
        }

        if (status == RB960X_OK)
        {
            if (rb->model == RB960X_MODEL_9604)
            {
                /* AT+GSN isn't supported on the 9604. */
                status = rb960xFetchSatelliteSerialImei(rb, rb->serial, sizeof(rb->serial), rb->imei, sizeof(rb->imei));
            }
            else
            {
                char resp[RB960X_LINE_BUF_SIZE];

                status = rb960xAtGsn(rb, resp, sizeof(resp));

                if (status == RB960X_OK)
                {
                    utilStrCopy(rb->serial, resp, sizeof(rb->serial));
                }
            }
        }

        if ((status == RB960X_OK) && (rb->model == RB960X_MODEL_9604))
        {
            char resp[RB960X_LINE_BUF_SIZE];
            status = rb960xAtReadUrat(rb, resp, sizeof(resp));

            if (status == RB960X_OK)
            {
                rb->ratMode = rb960xParseUrat(resp);
            }

#if RB960X_INIT_RAT != RB960X_INIT_RAT_NONE
            /* Switch before the settings below, as the switch resets the
             * module. Does nothing if it's already in the mode. */
            if (status == RB960X_OK)
            {
                status = rb960xSwitchRatOnly(rb, (rb960xRatMode_t)RB960X_INIT_RAT);
            }
#endif
        }

        if (status == RB960X_OK)
        {
            status = rb960xApplySettings(rb);
        }

        if ((status == RB960X_OK) &&
            (atClientRegisterUrc(&rb->at, "SBDRING", rb960xOnSbdring, rb) != AT_OK))
        {
            status = RB960X_ERR_HAL;
        }

        if (status == RB960X_OK)
        {
            rb->initialised = true;
        }
    }

    return status;
}

rb960xStatus_t rb960xSwitchRat(rb960x_t *rb, rb960xRatMode_t mode)
{
    bool reset = ((rb != NULL) && (rb->ratMode != mode));
    rb960xStatus_t status = rb960xSwitchRatOnly(rb, mode);

    /* The reset may have undone the settings rb960xInit() made. */
    if ((status == RB960X_OK) && (reset == true))
    {
        status = rb960xApplySettings(rb);
    }

    return status;
}

rb960xStatus_t rb960xGetCellularIccid(rb960x_t *rb, char * const iccid, const size_t iccidLength)
{
    rb960xStatus_t status = RB960X_ERR_ARG;
    const uint8_t iccidCountMax = 10;
    const uint32_t iccidCountDelayMs = 500;
    uint8_t iccidCount = 0;
    char resp[RB960X_LINE_BUF_SIZE];
    char *iccidValuePtr = NULL;

    if ((rb != NULL) && (iccid != NULL) && (iccidLength > 0))
    {
        if ((rb->model == RB960X_MODEL_9604) && (rb->ratMode == RB960X_RAT_LTE_CAT_M1))
        {
            do
            {
                status = rb960xAtCcid(rb, resp, sizeof(resp));
                if (status == RB960X_OK)
                {
                    AT_VALUE_AFTER_COLON(iccidValuePtr, resp);
                    if (iccidValuePtr != NULL)
                    {
                        utilStrCopy(iccid, iccidValuePtr, iccidLength);
                    }
                    break;
                }
                else
                {
                    /* Just after a RAT switch the SIM may not be ready
                     * yet, so wait and try again. */
                    DELAY_MS(iccidCountDelayMs);
                    iccidCount++;
                }
            } while (iccidCount < iccidCountMax);
        }
        else if ((rb->model == RB960X_MODEL_9604) && (rb->ratMode != RB960X_RAT_LTE_CAT_M1))
        {
            status = RB960X_ERR_WRONG_RAT;
        }
        else if (rb->model != RB960X_MODEL_9604)
        {
            status = RB960X_ERR_INVALID_MODEL;
        }
    }

    return status;
}

rb960xStatus_t rb960xGetCellularVersion(rb960x_t *rb, char * const version, const size_t versionLength)
{
    rb960xStatus_t status = RB960X_ERR_ARG;
    char resp[RB960X_LINE_BUF_SIZE];

    if ((rb != NULL) && (version != NULL) && (versionLength > 0))
    {
        if (rb->model == RB960X_MODEL_9604)
        {
            status = rb960xAtI9(rb, resp, sizeof(resp));
            if (status == RB960X_OK)
            {
                utilStrCopy(version, resp, versionLength);
            }
        }
        else
        {
            status = RB960X_ERR_INVALID_MODEL;
        }
    }

    return status;
}

rb960xStatus_t rb960xGetSatelliteVersion(rb960x_t *rb, char * const version, const size_t versionLength)
{
    rb960xStatus_t status = RB960X_ERR_ARG;
    char resp[RB960X_LINE_BUF_SIZE];
    const char * versionStartPtr = NULL;
    const char * versionEndPtr = NULL;
    size_t length = 0;

    if ((rb != NULL) && (version != NULL) && (versionLength > 0))
    {
        if (rb->model == RB960X_MODEL_9604)
        {
            if (rb->ratMode == RB960X_RAT_SBD)
            {
                status = rb960xAtUsatinfo(rb, resp, sizeof(resp));
                if (status == RB960X_OK)
                {
                    /* The firmware version is field 2 (counting from 0),
                     * e.g. "03.00.10" in "1,SBD,03.00.10". */
                    AT_GET_FIELD(resp, 2, versionStartPtr, versionEndPtr);
                    if (versionStartPtr != NULL)
                    {
                        length = (size_t)(versionEndPtr - versionStartPtr);
                        if (length >= versionLength)
                        {
                            length = versionLength - 1U;
                        }
                        memcpy(version, versionStartPtr, length);
                        version[length] = '\0';
                    }
                }
            }
            else
            {
                status = RB960X_ERR_WRONG_RAT;
            }
        }
        else if ((rb->model == RB960X_MODEL_9602) || (rb->model == RB960X_MODEL_9603))
        {
            status = rb960xAtI3(rb, resp, sizeof(resp));
            if (status == RB960X_OK)
            {
                utilStrCopy(version, resp, versionLength);
            }
        }
        else
        {
            status = RB960X_ERR_INVALID_MODEL;
        }
    }

    return status;
}

rb960xStatus_t rb960xGetSignal(rb960x_t *rb, bool fresh, uint8_t *bars)
{
    rb960xStatus_t status = RB960X_ERR_ARG;
    char resp[RB960X_LINE_BUF_SIZE];
    const char *valuePtr = NULL;
    unsigned int value = 0U;

    if (bars != NULL)
    {
        *bars = 0U;
    }

    if ((rb != NULL) && (bars != NULL))
    {
        status = rb960xCheckSessionPreconditions(rb);

        if (status == RB960X_OK)
        {
            status = (fresh == true) ? rb960xAtCsq(rb, resp, sizeof(resp)) : rb960xAtCsqf(rb, resp, sizeof(resp));
        }

        if (status == RB960X_OK)
        {
            /* "+CSQ:<bars>" from both commands (some firmware adds a space
             * after the colon, or text after the digit). */
            AT_VALUE_AFTER_COLON(valuePtr, resp);

            if ((rb960xParseUint(valuePtr, &value) != true) || (value > 5U))
            {
                status = RB960X_ERR_PROTOCOL;
            }
            else
            {
                *bars = (uint8_t)value;
            }
        }
    }

    return status;
}

rb960xStatus_t rb960xSessionReady(rb960x_t *rb)
{
    rb960xStatus_t status = RB960X_ERR_ARG;
    uint8_t bars = 0U;

    if (rb != NULL)
    {
        status = rb960xCheckSessionPreconditions(rb);

        if ((status == RB960X_OK) && (rb->mtWaiting == true))
        {
            status = RB960X_ERR_MT_UNREAD;
        }

        if (status == RB960X_OK)
        {
            status = rb960xGetSignal(rb, false, &bars);
        }

        if ((status == RB960X_OK) && (bars < (uint8_t)RB960X_MIN_SIGNAL_BARS))
        {
            status = RB960X_ERR_NO_SIGNAL;
        }
    }

    return status;
}

const char *rb960xMoStatusText(rb960xMoStatus_t moResult)
{
    /* Wording from the AT command guides. */
    static const struct
    {
        rb960xMoStatus_t moResult;
        const char *text;
    } moStatusTexts[] = {
        { RB960X_MO_OK, "MO message, if any, transferred successfully" },
        { RB960X_MO_OK_MT_TOO_BIG, "MO message, if any, transferred successfully, but the MT message in the queue was too big to be transferred" },
        { RB960X_MO_OK_LOCATION_UPDATE_REJECTED, "Session completed, but the requested location update was not accepted" },
        { RB960X_MO_GSS_CALL_TIMEOUT, "Gateway reported that the call did not complete in the allowed time" },
        { RB960X_MO_GSS_QUEUE_FULL, "MO message queue at the gateway is full" },
        { RB960X_MO_TOO_MANY_SEGMENTS, "MO message has too many segments" },
        { RB960X_MO_GSS_SESSION_INCOMPLETE, "Gateway reported that the session did not complete" },
        { RB960X_MO_INVALID_SEGMENT_SIZE, "Invalid segment size" },
        { RB960X_MO_ACCESS_DENIED, "Access is denied" },
        { RB960X_MO_LOCKED, "Modem has been locked and may not make SBD calls (see AT+CULK)" },
        { RB960X_MO_GATEWAY_NOT_RESPONDING, "Gateway not responding (local session timeout)" },
        { RB960X_MO_RF_DROP, "Connection lost (RF drop)" },
        { RB960X_MO_LINK_FAILURE, "Link failure (a protocol error caused termination of the call)" },
        { RB960X_MO_NO_SERVICE, "No network service, unable to initiate call" },
        { RB960X_MO_ANTENNA_FAULT, "Antenna fault, unable to initiate call" },
        { RB960X_MO_RADIO_DISABLED, "Radio is disabled, unable to initiate call (see AT*Rn)" },
        { RB960X_MO_BUSY, "Modem is busy, unable to initiate call" },
        { RB960X_MO_WAIT_REGISTRATION, "Try later, must wait 3 minutes since last registration" },
        { RB960X_MO_SERVICE_DISABLED, "SBD service is temporarily disabled" },
        { RB960X_MO_TRAFFIC_MANAGEMENT, "Try later, traffic management period (see AT+SBDLOE)" },
        { RB960X_MO_BAND_VIOLATION, "Band violation (attempt to transmit outside permitted frequency band)" },
        { RB960X_MO_HARDWARE_ERROR, "Hardware error during attempted transmit" },
        { RB960X_MO_RESERVED_SUCCESS, "Reserved, but indicates MO session success" },
        { RB960X_MO_RESERVED_LOCATION_UPDATE_FAILURE, "Reserved, but indicates location update failure" },
        { RB960X_MO_RESERVED_FAILURE, "Reserved, but indicates failure" },
    };
    const char *text = NULL;
    size_t i = 0U;

    for (i = 0U; (i < (sizeof(moStatusTexts) / sizeof(moStatusTexts[0]))) && (text == NULL); i++)
    {
        if (moStatusTexts[i].moResult == moResult)
        {
            text = moStatusTexts[i].text;
        }
    }

    if (text == NULL)
    {
        text = "Unknown MO status";
    }

    return text;
}

rb960xStatus_t rb960xGetSbdStatus(rb960x_t *rb, rb960xSbdStatus_t *sbdStatus)
{
    /* Largest value each field can hold; MTMSN (field 3) may also be -1. */
    static const unsigned int fieldMax[6] = { 1U, UINT16_MAX, 1U, UINT16_MAX, 1U, UINT16_MAX };
    unsigned int values[6] = { 0U };
    rb960xStatus_t status = RB960X_ERR_ARG;
    char resp[RB960X_LINE_BUF_SIZE];
    const char *fields = NULL;
    const char *start = NULL;
    const char *end = NULL;
    bool mtmsnNone = false;
    unsigned int i = 0U;

    if (sbdStatus != NULL)
    {
        (void)memset(sbdStatus, 0, sizeof(*sbdStatus));
    }

    if ((rb != NULL) && (sbdStatus != NULL))
    {
        status = rb960xCheckSessionPreconditions(rb);
    }

    if (status == RB960X_OK)
    {
        status = rb960xStatusFromAt(atClientSendCmd(&rb->at, "AT+SBDSX", resp, sizeof(resp), RB960X_DEFAULT_TIMEOUT_MS));
    }

    if (status == RB960X_OK)
    {
        AT_VALUE_AFTER_COLON(fields, resp);

        if (fields == NULL)
        {
            status = RB960X_ERR_PROTOCOL;
        }
    }

    for (i = 0U; (i < 6U) && (status == RB960X_OK); i++)
    {
        AT_GET_FIELD(fields, i, start, end);
        (void)end;

        if (start == NULL)
        {
            status = RB960X_ERR_PROTOCOL;
        }
        else
        {
            AT_SKIP_WS(start);

            if ((i == 3U) && (strncmp(start, "-1", 2U) == 0))
            {
                mtmsnNone = true;
            }
            else if ((rb960xParseUint(start, &values[i]) != true) || (values[i] > fieldMax[i]))
            {
                status = RB960X_ERR_PROTOCOL;
            }
            else
            {
                /* Field read. */
            }
        }
    }

    if (status == RB960X_OK)
    {
        sbdStatus->moWaiting = (values[0] == 1U);
        sbdStatus->momsn = (uint16_t)values[1];
        sbdStatus->mtWaiting = (values[2] == 1U);
        sbdStatus->mtmsn = (mtmsnNone == true) ? -1 : (int32_t)values[3];
        sbdStatus->ringAlert = (values[4] == 1U);
        sbdStatus->mtQueued = (uint16_t)values[5];
    }

    return status;
}

rb960xStatus_t rb960xWriteMoBuffer(rb960x_t *rb, const char * const data, const size_t length)
{
    rb960xStatus_t status = RB960X_ERR_ARG;

    if ((rb != NULL) && (data != NULL) && (length > 0U))
    {
        status = rb960xCheckSessionPreconditions(rb);

        if (status != RB960X_OK)
        {
            /* Nothing more to do. */
        }
        else if (length > RB960X_MO_MAX)
        {
            status = RB960X_ERR_TOO_LARGE;
        }
        else if (rb->model == RB960X_MODEL_9604)
        {
            /* The 9604 has no AT+SBDWB; it takes the data hex encoded. */
            status = rb960xWriteMoBufferHex(rb, data, length);
        }
        else
        {
            status = rb960xWriteMoBufferBinary(rb, data, length);
        }
    }

    return status;
}

rb960xStatus_t rb960xClearBuffers(rb960x_t *rb, rb960xBuffer_t which)
{
    rb960xStatus_t status = RB960X_ERR_ARG;
    char cmd[] = "AT+SBDD=0";
    char resp[RB960X_LINE_BUF_SIZE];
    const char *value = resp;
    unsigned int result = 1U;

    if ((rb != NULL) &&
        ((which == RB960X_BUFFER_MO) || (which == RB960X_BUFFER_MT) || (which == RB960X_BUFFER_BOTH)))
    {
        status = rb960xCheckSessionPreconditions(rb);
    }

    if (status == RB960X_OK)
    {
        /* AT+SBDDn (reply "<result>") on the 9602 / 9603, AT+SBDD=n (reply
         * "+SBDD:<result>") on the 9604. Result 0 means cleared. */
        if (rb->model == RB960X_MODEL_9604)
        {
            cmd[8] = (char)('0' + (int)which);
        }
        else
        {
            cmd[7] = (char)('0' + (int)which);
            cmd[8] = '\0';
        }

        status = rb960xStatusFromAt(atClientSendCmd(&rb->at, cmd, resp, sizeof(resp), RB960X_DEFAULT_TIMEOUT_MS));
    }

    if ((status == RB960X_OK) && (rb->model == RB960X_MODEL_9604))
    {
        AT_VALUE_AFTER_COLON(value, resp);
    }

    if ((status == RB960X_OK) && ((rb960xParseUint(value, &result) != true) || (result != 0U)))
    {
        status = RB960X_ERR_PROTOCOL;
    }

    if ((status == RB960X_OK) && (which != RB960X_BUFFER_MO))
    {
        rb->mtWaiting = false;
    }

    return status;
}

rb960xStatus_t rb960xInitiateSession(rb960x_t *rb, bool answerRing, uint32_t timeoutMs, rb960xSessionResult_t *result)
{
    rb960xStatus_t status = RB960X_ERR_ARG;
    rb960xSessionResult_t session;
    const char *cmd = NULL;
    char resp[RB960X_LINE_BUF_SIZE];

    (void)memset(&session, 0, sizeof(session));

    if (rb != NULL)
    {
        status = rb960xSessionPrepare(rb, answerRing, &cmd);

        if (status == RB960X_OK)
        {
            rb->sessionCount++;
            status = rb960xStatusFromAt(atClientSendCmd(&rb->at, cmd, resp, sizeof(resp), timeoutMs));
        }

        if (status == RB960X_OK)
        {
            status = rb960xSessionFinish(rb, resp, &session);
        }
    }

    if (result != NULL)
    {
        *result = session;
    }

    return status;
}

rb960xStatus_t rb960xInitiateSessionAsync(rb960x_t *rb, bool answerRing, uint32_t timeoutMs,
                                          rb960xSessionCallback_t callback, void *userData)
{
    rb960xStatus_t status = RB960X_ERR_ARG;
    const char *cmd = NULL;

    if ((rb != NULL) && (callback != NULL))
    {
        status = rb960xSessionPrepare(rb, answerRing, &cmd);

        if (status == RB960X_OK)
        {
            rb->sessionCount++;
            status = rb960xStatusFromAt(atClientSendCmdAsync(&rb->at, cmd, rb->sessionResp, sizeof(rb->sessionResp),
                                                             timeoutMs));
        }

        if (status == RB960X_OK)
        {
            /* rb960xProcess() finishes the job. */
            rb->sessionCb = callback;
            rb->sessionCbUserData = userData;
            rb->sessionPending = true;
        }
    }

    return status;
}

rb960xStatus_t rb960xReadMtBuffer(rb960x_t *rb, char *buf, size_t bufSize, size_t *length)
{
    rb960xStatus_t status = RB960X_ERR_ARG;

    if (length != NULL)
    {
        *length = 0U;
    }

    if ((rb != NULL) && (buf != NULL) && (length != NULL))
    {
        status = rb960xCheckSessionPreconditions(rb);

        if (status != RB960X_OK)
        {
            /* Nothing more to do. */
        }
        else if (rb->model == RB960X_MODEL_9604)
        {
            status = rb960xReadMtBufferHex(rb, buf, bufSize, length);
        }
        else
        {
            status = rb960xReadMtBufferBinary(rb, buf, bufSize, length);
        }
    }

    return status;
}

void rb960xSetSignalCallback(rb960x_t *rb, rb960xSignalCallback_t callback, void *userData)
{
    if (rb != NULL)
    {
        rb->signalCb = callback;
        rb->signalCbUserData = userData;
        /* Poll on the next rb960xProcess(). */
        rb->signalPolled = false;
    }
}

void rb960xSetRingCallback(rb960x_t *rb, rb960xRingCallback_t callback, void *userData)
{
    if (rb != NULL)
    {
        rb->ringCb = callback;
        rb->ringCbUserData = userData;
    }
}

/* Finish an asynchronous session if its reply has arrived (or timed out),
 * and call its callback. */
static void rb960xProcessSession(rb960x_t *rb)
{
    atStatus_t atStatus = atClientPollCmd(&rb->at);
    rb960xStatus_t status = RB960X_OK;
    rb960xSessionResult_t result;
    rb960xSessionCallback_t callback = rb->sessionCb;
    void *userData = rb->sessionCbUserData;

    if (atStatus != AT_PENDING)
    {
        (void)memset(&result, 0, sizeof(result));

        status = rb960xStatusFromAt(atStatus);
        if (status == RB960X_OK)
        {
            status = rb960xSessionFinish(rb, rb->sessionResp, &result);
        }

        /* Clear before the callback so it can start another session. */
        rb->sessionPending = false;
        rb->sessionCb = NULL;
        rb->sessionCbUserData = NULL;

        if (callback != NULL)
        {
            callback(rb, status, ((status == RB960X_OK) || (status == RB960X_ERR_SESSION)) ? &result : NULL, userData);
        }
    }
}

/* Poll the signal with AT+CSQF if a callback is set and a poll is due. */
static void rb960xProcessSignal(rb960x_t *rb)
{
    rb960xStatus_t status = RB960X_OK;
    uint8_t bars = 0U;

    if ((RB960X_SIGNAL_POLL_MS > 0UL) && (rb->signalCb != NULL) &&
        ((rb->signalPolled != true) || ((uint32_t)(MILLIS() - rb->lastSignalPollMs) >= (uint32_t)RB960X_SIGNAL_POLL_MS)))
    {
        rb->signalPolled = true;
        rb->lastSignalPollMs = MILLIS();
        status = rb960xGetSignal(rb, false, &bars);
        rb->signalCb(rb, status, bars, rb->signalCbUserData);
    }
}

/* Report a ring alert to the ring callback, unless a session has answered
 * it since it arrived. */
static void rb960xProcessRing(rb960x_t *rb)
{
    if ((rb->ringNotify == true) && (rb->ringCb != NULL))
    {
        rb->ringNotify = false;
        if (rb->ringAlert == true)
        {
            rb->ringCb(rb, rb->ringCbUserData);
        }
    }
}

void rb960xProcess(rb960x_t *rb)
{
    if (rb != NULL)
    {
        if (rb->sessionPending == true)
        {
            rb960xProcessSession(rb);
        }
        else
        {
            atClientProcess(&rb->at);
            rb960xProcessRing(rb);

            /* The ring callback may have started an asynchronous session. */
            if (rb->sessionPending != true)
            {
                rb960xProcessSignal(rb);
            }
        }
    }
}

/* ==========================================================================
 * Low-level AT commands (documented in rockblock960x.h)
 *
 * Each one wraps a single atClientSendCmd() call: send the command, convert
 * the status, and return the reply as-is. No parsing, and nothing in *rb is
 * changed apart from rb->at.
 * ========================================================================== */

rb960xStatus_t rb960xAtProbe(rb960x_t *rb, uint32_t timeoutMs)
{
    return rb960xStatusFromAt(atClientSendCmd(&rb->at, "AT", NULL, 0, timeoutMs));
}

rb960xStatus_t rb960xAtEcho(rb960x_t *rb, bool enable)
{
    return rb960xStatusFromAt(atClientSendCmd(&rb->at, (enable == true) ? "ATE1" : "ATE0", NULL, 0, RB960X_DEFAULT_TIMEOUT_MS));
}

rb960xStatus_t rb960xAtI3(rb960x_t *rb, char *resp, size_t respSize)
{
    return rb960xStatusFromAt(atClientSendCmd(&rb->at, "ATI3", resp, respSize, RB960X_DEFAULT_TIMEOUT_MS));
}

rb960xStatus_t rb960xAtI7(rb960x_t *rb, char *resp, size_t respSize)
{
    return rb960xStatusFromAt(atClientSendCmd(&rb->at, "ATI7", resp, respSize, RB960X_DEFAULT_TIMEOUT_MS));
}

rb960xStatus_t rb960xAtI9(rb960x_t *rb, char *resp, size_t respSize)
{
    return rb960xStatusFromAt(atClientSendCmd(&rb->at, "ATI9", resp, respSize, RB960X_DEFAULT_TIMEOUT_MS));
}

rb960xStatus_t rb960xAtI11(rb960x_t *rb, char *resp, size_t respSize)
{
    return rb960xStatusFromAt(atClientSendCmd(&rb->at, "ATI11", resp, respSize, RB960X_DEFAULT_TIMEOUT_MS));
}


rb960xStatus_t rb960xAtCgsn(rb960x_t *rb, char *resp, size_t respSize)
{
    return rb960xStatusFromAt(atClientSendCmd(&rb->at, "AT+CGSN", resp, respSize, RB960X_DEFAULT_TIMEOUT_MS));
}

rb960xStatus_t rb960xAtGsn(rb960x_t *rb, char *resp, size_t respSize)
{
    return rb960xStatusFromAt(atClientSendCmd(&rb->at, "AT+GSN", resp, respSize, RB960X_DEFAULT_TIMEOUT_MS));
}

rb960xStatus_t rb960xAtUsatsn(rb960x_t *rb, char *resp, size_t respSize)
{
    return rb960xStatusFromAt(atClientSendCmd(&rb->at, "AT+USATSN", resp, respSize, RB960X_DEFAULT_TIMEOUT_MS));
}

rb960xStatus_t rb960xAtUsatinfo(rb960x_t *rb, char *resp, size_t respSize)
{
    return rb960xStatusFromAt(atClientSendCmd(&rb->at, "AT+USATINFO", resp, respSize, RB960X_DEFAULT_TIMEOUT_MS));
}

rb960xStatus_t rb960xAtCsqf(rb960x_t *rb, char *resp, size_t respSize)
{
    return rb960xStatusFromAt(atClientSendCmd(&rb->at, "AT+CSQF", resp, respSize, RB960X_DEFAULT_TIMEOUT_MS));
}

rb960xStatus_t rb960xAtCsq(rb960x_t *rb, char *resp, size_t respSize)
{
    return rb960xStatusFromAt(atClientSendCmd(&rb->at, "AT+CSQ", resp, respSize, RB960X_CSQ_TIMEOUT_MS));
}

rb960xStatus_t rb960xAtDn(rb960x_t *rb, const uint32_t dtrOption)
{
    rb960xStatus_t status = RB960X_ERR_ARG;
    char dtrCmd[] = "AT&D0";
    const uint32_t dtrMax = (rb->model == RB960X_MODEL_9604) ? RB960X_DTR_MAX_9604 : RB960X_DTR_MAX;

    if (dtrOption <= dtrMax)
    {
        /* Every option is a single digit. */
        dtrCmd[4] = (char)('0' + dtrOption);
        status = rb960xStatusFromAt(atClientSendCmd(&rb->at, dtrCmd, NULL, 0, RB960X_DEFAULT_TIMEOUT_MS));
    }

    return status;
}

rb960xStatus_t rb960xAtKn(rb960x_t *rb, const uint32_t flowOption)
{
    rb960xStatus_t status = RB960X_ERR_ARG;
    char flowCmd[] = "AT&K0";

    /* No model supports XON/XOFF (AT&K4 to AT&K6) according to the AT
     * command guides, so only these two are allowed. */
    if ((flowOption == RB960X_FLOW_DISABLED) || (flowOption == RB960X_FLOW_RTS_CTS))
    {
        flowCmd[4] = (char)('0' + flowOption);
        status = rb960xStatusFromAt(atClientSendCmd(&rb->at, flowCmd, NULL, 0, RB960X_DEFAULT_TIMEOUT_MS));
    }

    return status;
}

rb960xStatus_t rb960xAtSbdMta(rb960x_t *rb, const bool enable)
{
    return rb960xStatusFromAt(atClientSendCmd(&rb->at, (enable == true) ? "AT+SBDMTA=1" : "AT+SBDMTA=0", NULL, 0, RB960X_DEFAULT_TIMEOUT_MS));
}

rb960xStatus_t rb960xAtSbdst(rb960x_t *rb, const uint16_t seconds)
{
    char sbdstCmd[15]; /* AT+SBDST=nnnnn\0 */
    size_t pos = 0U;

    (void)strcpy(sbdstCmd, "AT+SBDST=");
    pos = strlen(sbdstCmd);
    pos += rb960xFormatUint(&sbdstCmd[pos], seconds);
    sbdstCmd[pos] = '\0';

    return rb960xStatusFromAt(atClientSendCmd(&rb->at, sbdstCmd, NULL, 0, RB960X_DEFAULT_TIMEOUT_MS));
}

rb960xStatus_t rb960xAtCfun(rb960x_t *rb, const uint8_t function)
{
    char cfunCmd[12]; /* AT+CFUN=nnn\0 */
    (void)snprintf(cfunCmd, sizeof(cfunCmd), "AT+CFUN=%u", (unsigned int)function);
    return rb960xStatusFromAt(atClientSendCmd(&rb->at, cfunCmd, NULL, 0, RB960X_DEFAULT_TIMEOUT_MS));
}

rb960xStatus_t rb960xAtUrat(rb960x_t *rb, const uint8_t mode)
{
    char uratCmd[12]; /* AT+URAT=nnn\0 */
    (void)snprintf(uratCmd, sizeof(uratCmd), "AT+URAT=%u", (unsigned int)mode);
    return rb960xStatusFromAt(atClientSendCmd(&rb->at, uratCmd, NULL, 0, RB960X_DEFAULT_TIMEOUT_MS));
}

rb960xStatus_t rb960xAtCcid(rb960x_t *rb, char *resp, size_t respSize)
{
    return rb960xStatusFromAt(atClientSendCmd(&rb->at, "AT+CCID", resp, respSize, RB960X_DEFAULT_TIMEOUT_MS));
}

rb960xStatus_t rb960xAtReadUrat(rb960x_t *rb, char *resp, size_t respSize)
{
    return rb960xStatusFromAt(atClientSendCmd(&rb->at, "AT+URAT?", resp, respSize, RB960X_DEFAULT_TIMEOUT_MS));
}

/* TODO: rb960xResultStr(): return a fixed string for each rb960xStatus_t. */

/* TODO: rb960xVersion(): return RB960X_VERSION_STRING. */

/* TODO: rb960xSizeof(): return sizeof(rb960x_t). */
