/**
 * @file at_client.h
 * @brief Portable Hayes / 3GPP AT command client.
 *
 * A small, allocation-free AT command client intended for cellular modules
 * (primarily u-blox, but any Hayes-style modem works). All platform I/O is
 * done through user-supplied callbacks in ::atPort_t, so the core builds
 * unchanged on Linux, Windows, Arduino, PlatformIO and FreeRTOS/STM32.
 *
 * Usage outline:
 *   1. Implement (or reuse) an ::atPort_t for your platform.
 *   2. Call atClientInit() with a line buffer you own.
 *   3. Register URC handlers with atClientRegisterUrc().
 *   4. Send commands with atClientSendCmd() / atClientSendCmdFmt().
 *   5. When idle, call atClientProcess() regularly so URCs are dispatched.
 */

#ifndef AT_CLIENT_H
#define AT_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of URC handlers per client (override at compile time). */
#ifndef AT_CLIENT_MAX_URC_HANDLERS
#define AT_CLIENT_MAX_URC_HANDLERS 8
#endif

/** Command line terminator sent after each command. */
#ifndef AT_CLIENT_CMD_EOL
#define AT_CLIENT_CMD_EOL "\r"
#endif


/**
 * @brief Advance a pointer past any leading spaces or tabs.
 *
 * Moves @p p forward while it points at <tt>' '</tt> or <tt>'\\t'</tt>.
 * Stops at the first other character, including the NUL terminator.
 *
 * @param[in,out] p  Character pointer to advance. Must be a modifiable
 *                   lvalue and must not be NULL.
 *
 * @warning @p p is evaluated more than once. Don't pass an expression
 *          with side effects (e.g. @c ptr++).
 */
#define AT_SKIP_WS(p)                               \
    do                                              \
    {                                               \
        while ((*(p) == ' ') || (*(p) == '\t'))     \
        {                                           \
            (p)++;                                  \
        }                                           \
    } while (0)

/**
 * @brief Find the value part of an AT response, after the colon.
 *
 * Finds the first ':' in @p src and sets @p dst to the first character
 * after it that isn't a space or tab. For example, given
 * @c "+CGSN: 123456789", @p dst ends up pointing at @c "123456789".
 *
 * @param[out] dst  Character pointer that receives the result. Set to NULL
 *                  if @p src has no ':'. Must be a modifiable lvalue.
 * @param[in]  src  NUL-terminated AT response string. Must not be NULL.
 *
 * @note @p dst points into @p src. No copy is made, so @p src must stay
 *       valid for as long as @p dst is used.
 *
 * @warning @p dst is evaluated more than once. Don't pass an expression
 *          with side effects.
 *
 * @see AT_SKIP_WS
 */
#define AT_VALUE_AFTER_COLON(dst, src)              \
    do                                              \
    {                                               \
        (dst) = strchr((src), ':');                 \
        if ((dst) != NULL)                          \
        {                                           \
            (dst)++;                                \
            AT_SKIP_WS(dst);                        \
        }                                           \
    } while (0)

/**
 * @brief Find the Nth field in a comma-separated string.
 *
 * Sets @p start to the first character of field @p idx (counting from 0)
 * and @p end to one past its last character. @p end points at the next
 * <tt>','</tt>, <tt>'\\r'</tt>, <tt>'\\n'</tt> or the NUL terminator.
 * The field isn't copied or NUL-terminated. Its length is <tt>(end - start)</tt>.
 *
 * Example: in @c "1,SBD,03.00.04", index 2 gives @c "03.00.04".
 *
 * @param[in]  src    NUL-terminated source string. Must not be NULL.
 * @param[in]  idx    Field index, counting from 0. Evaluated only once.
 * @param[out] start  <tt>const char *</tt> set to the start of the field,
 *                    or NULL if the field doesn't exist.
 * @param[out] end    <tt>const char *</tt> set to one past the end of the
 *                    field, or NULL if the field doesn't exist.
 *
 * @note An empty field (e.g. @c "1,,3" at index 1) is valid. It gives
 *       <tt>start == end</tt>, not NULL.
 * @note Scanning stops at <tt>'\\r'</tt> or <tt>'\\n'</tt>, so a trailing
 *       <tt>"\r\nOK"</tt> in an AT response is ignored.
 */
#define AT_GET_FIELD(src, idx, start, end)                                  \
    do                                                                      \
    {                                                                       \
        const char *p_ = (src);                                             \
        unsigned int n_ = (unsigned int)(idx);                              \
                                                                            \
        while ((n_ > 0U) && (*p_ != '\0') && (*p_ != '\r') && (*p_ != '\n'))\
        {                                                                   \
            if (*p_ == ',')                                                 \
            {                                                               \
                n_--;                                                       \
            }                                                               \
            p_++;                                                           \
        }                                                                   \
                                                                            \
        if (n_ == 0U)                                                       \
        {                                                                   \
            (start) = p_;                                                   \
            while ((*p_ != ',') && (*p_ != '\0') &&                         \
                    (*p_ != '\r') && (*p_ != '\n'))                          \
            {                                                               \
                p_++;                                                       \
            }                                                               \
            (end) = p_;                                                     \
        }                                                                   \
        else                                                                \
        {                                                                   \
            (start) = NULL;                                                 \
            (end)   = NULL;                                                 \
        }                                                                   \
    } while (0)

/** Result of an AT operation. */
typedef enum
{
    AT_OK = 0,           /**< Final result "OK" (or "CONNECT"). */
    AT_ERR_ERROR,        /**< Final result "ERROR", "ABORTED", "NO CARRIER", ... */
    AT_ERR_CME,          /**< "+CME ERROR: <n>", code via atClientLastErrorCode(). */
    AT_ERR_CMS,          /**< "+CMS ERROR: <n>", code via atClientLastErrorCode(). */
    AT_ERR_TIMEOUT,      /**< No final result within the timeout. */
    AT_ERR_IO,           /**< The port write/read callback failed. */
    AT_ERR_PARAM,        /**< Invalid argument. */
    AT_ERR_OVERFLOW,     /**< Command finished OK but the response buffer was
                              too small; the response is truncated. */
    AT_ERR_BUSY,         /**< A command is already in progress on this client. */
    AT_PENDING           /**< atClientPollCmd(): the command started with
                              atClientSendCmdAsync() has not finished yet. */
} atStatus_t;

/**
 * Platform port. Implement these callbacks for your target.
 * The struct must stay valid for the lifetime of the client.
 *
 * Time comes from MILLIS() in util.h rather than from the port, so the
 * client and the driver share one clock.
 */
typedef struct atPort
{
    /** Opaque pointer passed back to every callback. */
    void *userData;

    /**
     * Write @p len bytes to the modem. Required.
     * @return number of bytes written, or a negative value on error.
     * May block until all bytes are accepted.
     */
    int (*write)(void *userData, const void *data, size_t len);

    /**
     * Non-blocking read of up to @p len bytes from the modem. Required.
     * @return number of bytes read (0 if none available), negative on error.
     */
    int (*read)(void *userData, void *buf, size_t len);

    /**
     * Optional. Called while waiting for data (e.g. sleep 1 ms, taskYIELD(),
     * or nothing). NULL means busy-wait.
     */
    void (*yield)(void *userData);

    /** Optional recursive-safe mutex hooks for multi-threaded use. */
    void (*lock)(void *userData);
    void (*unlock)(void *userData);
} atPort_t;

/** URC / line handler. @p line is NUL-terminated and valid only during the call. */
typedef void (*atLineHandler_t)(const char *line, void *userData);

/** Direction of traffic reported to the debug handler. */
typedef enum
{
    AT_DEBUG_TX,        /**< Data written to the modem. */
    AT_DEBUG_RX         /**< Data received from the modem. */
} atDebugDir_t;

/**
 * Debug / trace handler. @p data is valid only during the call. For command
 * lines (TX) and received lines (RX) it is NUL-terminated; raw payload data
 * (atClientWriteRaw() / atClientReadRaw()) may be binary, so always honour
 * @p len when logging.
 */
typedef void (*atDebugHandler_t)(atDebugDir_t dir, const char *data,
                                 size_t len, void *userData);

/** Internal URC table entry. Do not use directly. */
typedef struct
{
    const char *prefix;
    atLineHandler_t handler;
    void *userData;
} atUrcEntry_t;

/**
 * Client instance. Treat all fields as private; allocate one (statically is
 * fine) and initialise it with atClientInit().
 */
typedef struct atClient
{
    const atPort_t *port;

    /* Receive line assembly. */
    char *lineBuf;
    size_t lineSize;
    size_t lineLen;
    bool lineTruncated;

    /* URC dispatch. */
    atUrcEntry_t urc[AT_CLIENT_MAX_URC_HANDLERS];
    atLineHandler_t onLine; /* catch-all for unmatched lines when idle */
    void *onLineUserData;

    /* Debug trace. */
    atDebugHandler_t onDebug;
    void *onDebugUserData;

    /* In-flight command state. */
    bool inCmd;
    const char *pendingCmd; /* for echo suppression, may be NULL */
    char *respBuf;
    size_t respSize;
    size_t respLen;
    bool respOverflow;
    bool gotFinal;
    atStatus_t finalStatus;
    int lastErrorCode; /* numeric part of +CME/+CMS ERROR */

    /* Command started with atClientSendCmdAsync(), finished by
     * atClientPollCmd(). */
    bool asyncCmd;
    uint32_t asyncStartMs;
    uint32_t asyncTimeoutMs;
} atClient_t;

/**
 * Initialise a client.
 *
 * @param client   Client instance to initialise.
 * @param port     Platform callbacks; must outlive the client.
 * @param lineBuf Caller-owned buffer used to assemble received lines. Its
 *                 size bounds the longest receivable line (URCs included).
 * @param lineBufSize Size of @p lineBuf in bytes (>= 16 recommended >= 128).
 * @return AT_OK or AT_ERR_PARAM.
 */
atStatus_t atClientInit(atClient_t *client, const atPort_t *port,
                        char *lineBuf, size_t lineBufSize);

/**
 * Send a command and wait for the final result code.
 *
 * Intermediate information lines (e.g. "+CSQ: 17,99") are copied into
 * @p resp separated by '\n'. Echoed commands, "OK" and error finals are not
 * copied. Registered URCs arriving mid-command are dispatched to their
 * handlers instead of the response buffer.
 *
 * @param client     Initialised client.
 * @param cmd        Command without line terminator, e.g. "AT+CSQ".
 * @param resp       Buffer for information lines, or NULL to discard. Always
 *                   NUL-terminated on return when non-NULL.
 * @param respSize  Size of @p resp in bytes.
 * @param timeoutMs Time to wait for the final result code.
 */
atStatus_t atClientSendCmd(atClient_t *client, const char *cmd,
                           char *resp, size_t respSize,
                           uint32_t timeoutMs);

/** printf-style variant of atClientSendCmd(). */
atStatus_t atClientSendCmdFmt(atClient_t *client, char *resp,
                              size_t respSize, uint32_t timeoutMs,
                              const char *fmt, ...)
#if defined(__GNUC__)
__attribute__((format(printf, 5, 6)))
#endif
;

/**
 * Send a command without waiting for the final result code.
 *
 * Use for long-running commands (e.g. AT+SBDIX) when the caller can't
 * block. Then call atClientPollCmd() regularly until it returns something
 * other than ::AT_PENDING. Until then the client is busy: other sends
 * return ::AT_ERR_BUSY and atClientProcess() does nothing, but URCs are
 * still dispatched by atClientPollCmd().
 *
 * @param client     Initialised client.
 * @param cmd        Command without line terminator. Must stay valid until
 *                   the command finishes (it's used for echo suppression).
 * @param resp       Buffer for information lines, or NULL to discard. Must
 *                   stay valid until the command finishes.
 * @param respSize   Size of @p resp in bytes.
 * @param timeoutMs  Time to wait for the final result code, measured from
 *                   this call.
 * @return AT_OK if the command was sent (the result comes from
 *         atClientPollCmd()), AT_ERR_PARAM, AT_ERR_BUSY or AT_ERR_IO. On
 *         any error the client is left idle.
 */
atStatus_t atClientSendCmdAsync(atClient_t *client, const char *cmd,
                                char *resp, size_t respSize,
                                uint32_t timeoutMs);

/**
 * Check on a command started with atClientSendCmdAsync(). Reads whatever
 * the modem has sent and dispatches URCs, without waiting.
 *
 * @return ::AT_PENDING while the command is still running. Otherwise the
 *         command's result, as atClientSendCmd() would have returned it,
 *         and the client is idle again. AT_ERR_PARAM if no asynchronous
 *         command is running.
 */
atStatus_t atClientPollCmd(atClient_t *client);

/**
 * Wait for a final result code without sending anything first. Use after
 * writing raw payload data (e.g. SMS body, socket data) with
 * atClientWriteRaw().
 */
atStatus_t atClientWaitFinal(atClient_t *client, char *resp,
                             size_t respSize, uint32_t timeoutMs);

/**
 * Wait until @p pattern appears in the raw receive stream, e.g. the "> "
 * prompt of AT+CMGS / AT+USOWR. Bytes consumed while matching are NOT fed to
 * the line parser, so call this only when a prompt is genuinely expected.
 *
 * @return AT_OK when matched, AT_ERR_TIMEOUT or AT_ERR_IO otherwise.
 */
atStatus_t atClientWaitFor(const atClient_t *client, const char *pattern,
                           uint32_t timeoutMs);

/**
 * Write raw bytes to the modem (payload data, escape sequences, ...).
 * @return number of bytes written, or negative on error.
 */
int atClientWriteRaw(const atClient_t *client, const void *data, size_t len);

/**
 * Read raw bytes, waiting up to @p timeoutMs for the first byte. Returns as
 * soon as some data has been read; it does not wait for @p len bytes.
 * Bytes returned here bypass the line parser (use for binary socket reads).
 * @return number of bytes read (0 on timeout), or negative on error.
 */
int atClientReadRaw(const atClient_t *client, void *buf, size_t len,
                    uint32_t timeoutMs);

/**
 * Pump the receive path: read whatever is available, assemble lines and
 * dispatch URCs. Call regularly when no command is in flight. Does nothing
 * while a command is in flight, including one started with
 * atClientSendCmdAsync() (use atClientPollCmd() for that).
 */
void atClientProcess(atClient_t *client);

/**
 * Register a handler for unsolicited result codes whose line starts with
 * @p prefix (e.g. "+UUSORD:", "+CREG:"). Matching happens before response
 * collection, including while a command is in flight, so do not register a
 * prefix that is also a direct response to commands you send.
 *
 * @return AT_OK, AT_ERR_PARAM, or AT_ERR_OVERFLOW if the table is full.
 */
atStatus_t atClientRegisterUrc(atClient_t *client, const char *prefix,
                               atLineHandler_t handler, void *userData);

/** Remove a previously registered URC handler. */
void atClientUnregisterUrc(atClient_t *client, const char *prefix);

/**
 * Optional catch-all invoked for received lines that match no URC prefix
 * while no command is in flight. Pass NULL to disable.
 */
void atClientSetLineHandler(atClient_t *client, atLineHandler_t handler,
                            void *userData);

/**
 * Optional debug / trace handler for viewing AT traffic. Pass NULL to
 * disable. When set, the handler is invoked with:
 *
 *  - AT_DEBUG_TX for every command sent (without the line terminator) and
 *    every atClientWriteRaw() payload;
 *  - AT_DEBUG_RX for every complete received line (echoes, information
 *    lines, finals and URCs alike) and every atClientReadRaw() chunk.
 *
 * Bytes consumed by atClientWaitFor() prompt matching are not traced. The
 * handler runs in the calling context; keep it fast (e.g. printf).
 */
void atClientSetDebugHandler(atClient_t *client, atDebugHandler_t handler,
                             void *userData);

/** Numeric code of the last "+CME ERROR"/"+CMS ERROR" (-1 if none). */
int atClientLastErrorCode(const atClient_t *client);

/** Human-readable name of a status code. */
const char *atStatusStr(atStatus_t status);

#ifdef __cplusplus
}
#endif

#endif /* AT_CLIENT_H */
