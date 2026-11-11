/**
 * @file rockblock960x.h
 * @brief RockBLOCK 9602 / 9603 / 9604 driver: public API.
 *
 * This is the only header applications include. Everything declared here
 * must be plain C99 and free of platform headers: no <windows.h>,
 * <unistd.h> or Arduino.h.
 *
 * Design rules (these are what keep the library portable):
 *   - No heap. The caller owns the rb960x_t context, which holds the AT
 *     line buffer; the library never calls malloc().
 *   - No direct OS or I/O calls. All UART and clock access goes through
 *     atPort_t callbacks: the platform's port, opened by rb960xBegin(), or
 *     ones the caller passes to rb960xInit().
 *   - Fixed-width <stdint.h> types where size matters, since @c int is only
 *     16 bits on AVR.
 *
 * Model support: some functions only work on certain models, or (on the
 * 9604) only in a certain radio access technology (RAT) mode. These return
 * RB960X_ERR_INVALID_MODEL or RB960X_ERR_WRONG_RAT rather than sending a
 * command the modem would reject.
 */
#ifndef ROCKBLOCK960X_H
#define ROCKBLOCK960X_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "at_client.h"
#include "at_port.h"
#include "rockblock960x_config.h"

/** @name Library version */
/** @{ */
#define RB960X_VERSION_MAJOR 0
#define RB960X_VERSION_MINOR 1
#define RB960X_VERSION_PATCH 0
#define RB960X_VERSION_STRING "0.1.0"
/** @} */

/** @name Highest valid option for rb960xAtDn() (AT&Dn) */
/** @{ */
#define RB960X_DTR_MAX 3        /**< 9602/9603: AT&D0 to AT&D3. */
#define RB960X_DTR_MAX_9604 2   /**< 9604: AT&D0 to AT&D2. */
/** @} */

/** @name Flow control options for rb960xAtKn() (AT&Kn)
 * XON/XOFF isn't offered: the AT command guides say no model supports it. */
/** @{ */
#define RB960X_FLOW_DISABLED 0  /**< AT&K0: no flow control. */
#define RB960X_FLOW_RTS_CTS 3   /**< AT&K3: hardware RTS/CTS flow control. */
/** @} */


#ifdef __cplusplus
extern "C" { /* Arduino sketches are C++, so keep C linkage. */
#endif

/** Result code returned by every rb960x function that can fail. */
typedef enum
{
    RB960X_OK = 0,                /**< Success. */
    RB960X_ERR_ARG = -1,          /**< Bad argument (NULL pointer, zero-length or too-small buffer, out-of-range option). */
    RB960X_ERR_HAL = -2,          /**< atClientInit() rejected the port callbacks. */
    RB960X_ERR_TIMEOUT = -3,      /**< The modem didn't respond in time. */
    RB960X_ERR_PROTOCOL = -4,     /**< The modem responded, but not with "OK" (e.g. "ERROR"). */
    RB960X_ERR_SESSION = -5,      /**< AT+SBDIX ran, but the MO status reported a failure (5 or higher). */
    RB960X_ERR_NO_MESSAGE = -6,   /**< AT+SBDRB ran, but the MT buffer was empty. */
    RB960X_ERR_TOO_LARGE = -7,    /**< The payload or message doesn't fit the given buffer. */
    RB960X_ERR_INVALID_MODEL = -8,/**< This RockBLOCK model doesn't support the feature. */
    RB960X_ERR_WRONG_RAT = -9,    /**< The 9604 is in the wrong RAT mode for this feature (see rb960xSwitchRat()). */
    RB960X_ERR_BUSY = -10,        /**< An asynchronous rb960xInitiateSessionAsync() is still running. */
    RB960X_ERR_PORT = -11,        /**< rb960xBegin() couldn't open the serial port (on Linux and macOS, errno says why). */
    RB960X_ERR_MT_UNREAD = -12,   /**< An MT message is waiting in the modem's MT buffer. Read it with rb960xReadMtBuffer() (or discard it with rb960xClearBuffers()) first: the next session would erase it. */
    RB960X_ERR_NO_SIGNAL = -13    /**< The signal is below RB960X_MIN_SIGNAL_BARS, so a session isn't worth attempting yet. */
} rb960xStatus_t;

/** RockBLOCK model, detected from the ATI7 reply during rb960xInit(). */
typedef enum
{
    RB960X_MODEL_UNKNOWN = 0,     /**< ATI7 didn't report a recognised model. */
    RB960X_MODEL_9602,            /**< RockBLOCK 9602 (SBD only). */
    RB960X_MODEL_9603,            /**< RockBLOCK 9603 (SBD only). */
    RB960X_MODEL_9604             /**< RockBLOCK 9604 (SBD and LTE Cat M1). */
} rb960xModel_t;

/**
 * Radio access technology (RAT) mode. Only the 9604 can switch between
 * modes; see rb960xSwitchRat().
 */
typedef enum
{
    RB960X_RAT_UNKNOWN = 0,       /**< Not read yet, or not a 9604. */
    RB960X_RAT_LTE_CAT_M1 = 1,    /**< Cellular LTE Cat M1 (AT+URAT=7). Same value as RB960X_INIT_RAT_CELLULAR. */
    RB960X_RAT_SBD = 2,           /**< Iridium Short Burst Data (AT+URAT=10). Same value as RB960X_INIT_RAT_SBD. */
} rb960xRatMode_t;

/** Module functionality level, set with rb960xAtCfun() (AT+CFUN=\<fun\>). */
typedef enum
{
    /**
     * Minimum functionality: transmit and receive RF circuits are turned
     * off (both CS and PS services deactivated).
     */
    RB960X_CFUN_0  = 0,
    /**
     * Full functionality (the factory default), e.g. to come out of airplane
     * mode or minimum functionality. Persists across power cycles triggered
     * by AT+CFUN=15, AT+CFUN=16 or AT+CPWROFF (where supported).
     */
    RB960X_CFUN_1  = 1,
    /**
     * Airplane mode: transmit and receive RF circuits are turned off (both
     * CS and PS services deactivated). Persists across power cycles
     * triggered by AT+CFUN=15, AT+CFUN=16 or AT+CPWROFF (where supported).
     */
    RB960X_CFUN_4  = 4,
    /**
     * Fast power-off: shuts down without sending a network detach request,
     * saving the current settings to non-volatile memory. "OK" means the
     * request was accepted; the +UUFASTSHUTDOWN URC then reports how the
     * power-off went.
     */
    RB960X_CFUN_10 = 10,
    /**
     * Silent reset of the module and the SIM card, with network detach and
     * NVM parameters saved first.
     */
    RB960X_CFUN_16 = 16,
} rb960xModuleFunc_t;


/**
 * The +SBDIX MO status, decoded for the modem's model; see
 * rb960xSessionResult_t::moResult and rb960xMoStatusText().
 *
 * Both AT command guides number the codes the same way, but the 9602/9603
 * guide defines more of them. A code is only given its own name on the
 * models whose guide defines it. On other models it becomes one of the
 * RB960X_MO_RESERVED_* values, as that model's guide describes it.
 *
 * Named values equal the raw code. The RB960X_MO_RESERVED_* values are
 * above 255, so they never match a raw code; the raw code is still in
 * rb960xSessionResult_t::moStatus.
 */
typedef enum
{
    /* Success: the MO message, if any, was sent. */
    RB960X_MO_OK = 0,                         /**< 0: MO message, if any, sent. All models. */
    RB960X_MO_OK_MT_TOO_BIG = 1,              /**< 1: MO message, if any, sent, but the queued MT message was too big to transfer. 9602/9603 only. */
    RB960X_MO_OK_LOCATION_UPDATE_REJECTED = 2, /**< 2: Session completed, but the requested location update was not accepted. All models. */

    /* Failure, reported by the gateway. 10 to 14 are 9602/9603 only. */
    RB960X_MO_GSS_CALL_TIMEOUT = 10,          /**< 10: The gateway reported that the call did not complete in the allowed time. */
    RB960X_MO_GSS_QUEUE_FULL = 11,            /**< 11: The MO message queue at the gateway is full. */
    RB960X_MO_TOO_MANY_SEGMENTS = 12,         /**< 12: The MO message has too many segments. */
    RB960X_MO_GSS_SESSION_INCOMPLETE = 13,    /**< 13: The gateway reported that the session did not complete. */
    RB960X_MO_INVALID_SEGMENT_SIZE = 14,      /**< 14: Invalid segment size. */
    RB960X_MO_ACCESS_DENIED = 15,             /**< 15: Access is denied. All models. */

    /* Failure, reported by the modem. */
    RB960X_MO_LOCKED = 16,                    /**< 16: The modem is locked and may not make SBD calls (see AT+CULK). All models. */
    RB960X_MO_GATEWAY_NOT_RESPONDING = 17,    /**< 17: Gateway not responding (local session timeout). All models. */
    RB960X_MO_RF_DROP = 18,                   /**< 18: Connection lost (RF drop). All models. */
    RB960X_MO_LINK_FAILURE = 19,              /**< 19: Link failure (a protocol error ended the call). 9602/9603 only. */
    RB960X_MO_NO_SERVICE = 32,                /**< 32: No network service, unable to initiate call. All models. */
    RB960X_MO_ANTENNA_FAULT = 33,             /**< 33: Antenna fault, unable to initiate call. All models. */
    RB960X_MO_RADIO_DISABLED = 34,            /**< 34: Radio is disabled, unable to initiate call (see AT*Rn). All models. */
    RB960X_MO_BUSY = 35,                      /**< 35: The modem is busy, unable to initiate call. All models. */
    RB960X_MO_WAIT_REGISTRATION = 36,         /**< 36: Try later, must wait 3 minutes since the last registration. All models. */
    RB960X_MO_SERVICE_DISABLED = 37,          /**< 37: SBD service is temporarily disabled. All models. */
    RB960X_MO_TRAFFIC_MANAGEMENT = 38,        /**< 38: Try later, traffic management period (see AT+SBDLOE). All models. */
    RB960X_MO_BAND_VIOLATION = 64,            /**< 64: Band violation (tried to transmit outside the permitted band). 9602/9603 only. */
    RB960X_MO_HARDWARE_ERROR = 65,            /**< 65: Hardware error during the transmit (the 9602/9603 guide says PLL lock failure). All models. */

    /* Codes the model's guide doesn't define by name. */
    RB960X_MO_RESERVED_SUCCESS = 256,         /**< 9602/9603: 3 or 4 ("reserved, but indicate MO session success"). 9604: 1, which its +SBDIX list leaves out. */
    RB960X_MO_RESERVED_LOCATION_UPDATE_FAILURE, /**< 9604 only: 3 to 14 ("reserved, but indicate location update failure"). */
    RB960X_MO_RESERVED_FAILURE                /**< Any other code the model's guide doesn't define ("reserved, but indicate failure"). */
} rb960xMoStatus_t;

/**
 * Result of an SBD session (AT+SBDIX), as reported by the modem in
 * "+SBDIX: <MO status>, <MOMSN>, <MT status>, <MTMSN>, <MT length>,
 * <MT queued>" ("+SBDI:" on the 9604).
 *
 * Every session also checks the gateway for mobile-terminated (MT)
 * messages, so check mtStatus and mtQueued even after a send.
 */
typedef struct
{
    uint8_t moStatus;   /**< Raw MO status code. 0 to 4: the session worked (and the MO message, if any, was sent). 5 or higher: it failed. */
    rb960xMoStatus_t moResult; /**< moStatus decoded for this model; rb960xMoStatusText() describes it. */
    uint16_t momsn;     /**< MO message sequence number (wraps at 65535). */
    uint8_t mtStatus;   /**< 0: no MT message waiting. 1: an MT message was received into the MT buffer. 2: error checking for or receiving an MT message. */
    uint16_t mtmsn;     /**< MT message sequence number. Only meaningful when mtStatus is 1. */
    uint16_t mtLength;  /**< Length of the received MT message in bytes; 0 if none. */
    uint16_t mtQueued;  /**< MT messages still waiting at the gateway. */
} rb960xSessionResult_t;

/**
 * State of the modem's SBD buffers and ring alert, as reported by
 * AT+SBDSX: "+SBDSX: <MO flag>, <MOMSN>, <MT flag>, <MTMSN>, <RA flag>,
 * <msg waiting>". Same on every model. See rb960xGetSbdStatus().
 */
typedef struct
{
    bool moWaiting;     /**< A message is in the MO buffer. */
    uint16_t momsn;     /**< MO message sequence number the next session will use. */
    bool mtWaiting;     /**< A message is in the MT buffer. */
    int32_t mtmsn;      /**< MT message sequence number of the last MT session; -1 if the MT buffer is empty. */
    bool ringAlert;     /**< A ring alert has arrived and hasn't been answered with a session yet. */
    uint16_t mtQueued;  /**< MT messages waiting at the gateway, as last reported. */
} rb960xSbdStatus_t;

/**
 * Which SBD buffer(s) rb960xClearBuffers() clears. The values match the
 * AT+SBDD parameter.
 */
typedef enum
{
    RB960X_BUFFER_MO = 0,         /**< The mobile-originated (outgoing) buffer. */
    RB960X_BUFFER_MT = 1,         /**< The mobile-terminated (incoming) buffer. */
    RB960X_BUFFER_BOTH = 2        /**< Both buffers. */
} rb960xBuffer_t;

struct rb960x;

/**
 * @brief Called when an asynchronous rb960xInitiateSessionAsync() finishes.
 *
 * Runs from inside rb960xProcess(). The session has already been cleared,
 * so the callback may start another one.
 *
 * @param rb        The context the session ran on.
 * @param status    The result rb960xInitiateSession() would have returned:
 *                  RB960X_OK if the session worked (MO status 0 to 4),
 *                  RB960X_ERR_SESSION if it ran but failed,
 *                  RB960X_ERR_TIMEOUT or RB960X_ERR_PROTOCOL otherwise.
 * @param result    The session result when @p status is RB960X_OK or
 *                  RB960X_ERR_SESSION, otherwise NULL. Only valid during
 *                  the callback; copy it to keep it.
 * @param userData  The pointer given to rb960xInitiateSessionAsync().
 */
typedef void (*rb960xSessionCallback_t)(struct rb960x *rb, rb960xStatus_t status, const rb960xSessionResult_t *result, void *userData);

/**
 * @brief Called by rb960xProcess() with each polled signal reading.
 *
 * See rb960xSetSignalCallback(). Runs from inside rb960xProcess().
 *
 * @param rb        The context that was polled.
 * @param status    RB960X_OK if the signal was read, otherwise the error
 *                  from rb960xGetSignal() (e.g. RB960X_ERR_WRONG_RAT on a
 *                  9604 in LTE Cat M1 mode).
 * @param bars      Signal strength, 0 to 5; 0 when @p status isn't
 *                  RB960X_OK.
 * @param userData  The pointer given to rb960xSetSignalCallback().
 */
typedef void (*rb960xSignalCallback_t)(struct rb960x *rb, rb960xStatus_t status, uint8_t bars, void *userData);

/**
 * @brief Called by rb960xProcess() after an SBDRING ring alert.
 *
 * See rb960xSetRingCallback(). Runs from inside rb960xProcess(), never
 * while a command is in progress, so it may call any driver function,
 * e.g. rb960xInitiateSession(rb, true, ...) to answer the ring.
 *
 * @param rb        The context the ring alert arrived on.
 * @param userData  The pointer given to rb960xSetRingCallback().
 */
typedef void (*rb960xRingCallback_t)(struct rb960x *rb, void *userData);

/**
 * Driver context. Allocate one per modem (statically or on the stack) and
 * pass it to rb960xBegin() or rb960xInit(). Treat the fields as read-only;
 * they are filled in by those functions and kept up to date by the driver.
 */
typedef struct rb960x
{
    atClient_t at;                       /**< Underlying AT client. */
    rb960xModel_t model;                 /**< Detected model (from ATI7). */
    char imei[RB960X_IMEI_LEN];          /**< Satellite (Iridium) IMEI. */
    char cellularImei[RB960X_IMEI_LEN];  /**< Cellular IMEI (9604 only; empty on other models). */
    char serial[RB960X_SERIAL_LEN];      /**< Satellite modem serial number. */
    bool initialised;                    /**< True once rb960xInit() has succeeded. */
    rb960xRatMode_t ratMode;             /**< Current RAT mode (9604 only; RB960X_RAT_UNKNOWN on other models). */
    rb960xSessionCallback_t sessionCb;   /**< Callback for the running rb960xInitiateSessionAsync(). */
    void *sessionCbUserData;             /**< Passed back to sessionCb. */
    bool sessionPending;                 /**< True while an asynchronous AT+SBDIX / AT+SBDIXA session is running. */
    char sessionResp[RB960X_LINE_BUF_SIZE]; /**< Holds the session reply while sessionPending is true. */
    bool mtWaiting;                      /**< A session put an MT message in the modem's MT buffer and it hasn't been read or cleared yet. */
    uint16_t mtQueued;                   /**< MT messages still waiting at the gateway, from the last session that reached it. */
    bool ringAlert;                      /**< An SBDRING ring alert arrived and no session has reached the gateway since. Answer it with rb960xInitiateSession(rb, true, ...). */
    bool ringNotify;                     /**< An SBDRING arrived that the ring callback hasn't been told about yet. */
    rb960xRingCallback_t ringCb;         /**< Set by rb960xSetRingCallback(); NULL for none. */
    void *ringCbUserData;                /**< Passed back to ringCb. */
    rb960xSignalCallback_t signalCb;     /**< Set by rb960xSetSignalCallback(); NULL turns signal polling off. */
    void *signalCbUserData;              /**< Passed back to signalCb. */
    bool signalPolled;                   /**< True once rb960xProcess() has polled the signal (lastSignalPollMs is valid). */
    uint32_t lastSignalPollMs;           /**< When rb960xProcess() last polled the signal (MILLIS()). */
    uint32_t sessionCount;               /**< SBD sessions (AT+SBDIX / AT+SBDIXA) started since rb960xInit(), to keep an eye on what they cost. */
    char lineBuf[RB960X_LINE_BUF_SIZE];  /**< Buffer the AT client assembles received lines in. */
    atPortHandle_t portHandle;           /**< Serial port opened by rb960xBegin(); unused after rb960xInit(). */
    bool ownsPort;                       /**< True if rb960xBegin() opened portHandle, so rb960xEnd() closes it. */
} rb960x_t;


/* ==========================================================================
 * High-level API
 * ========================================================================== */

/**
 * @brief Open the serial port and initialise the modem on it.
 *
 * The usual way to start: opens @p device with the platform's port (see
 * at_port.h) inside @p rb, then runs the same steps as rb960xInit(). Call
 * rb960xEnd() when finished. Use rb960xInit() instead to supply your own
 * port callbacks.
 *
 * On Arduino, call begin() on the serial port first and use the
 * rb960xBegin(rb, Serial1) overload below; @p baud is ignored there.
 *
 * Needs the platform's port linked in (atclient_port in CMake; built in
 * automatically on Arduino and PlatformIO).
 *
 * @param[out] rb            Context to initialise. Cleared before use.
 * @param[in]  device        What to open, e.g. "/dev/ttyUSB0" (see
 *                           atPortDevice_t).
 * @param[in]  baud          Baud rate (see atPortOpen()).
 * @param[in]  debugHandler  Optional AT traffic trace (NULL to disable). See
 *                           rb960xInit().
 * @param[in]  debugUserData Passed back to @p debugHandler.
 *
 * @return RB960X_OK on success,
 *         RB960X_ERR_ARG if @p rb is NULL,
 *         RB960X_ERR_PORT if the port couldn't be opened,
 *         or the error from rb960xInit(). On any failure the port is closed
 *         again.
 */
rb960xStatus_t rb960xBegin(rb960x_t *rb, atPortDevice_t device, uint32_t baud,
                             atDebugHandler_t debugHandler, void *debugUserData);

/**
 * @brief Close the serial port rb960xBegin() opened.
 *
 * Safe to call more than once, after a failed rb960xBegin(), or after
 * rb960xInit() (which owns no port, so there is nothing to close). The
 * context can't be used again until the next rb960xBegin() or rb960xInit().
 *
 * @param[in,out] rb  Context to close.
 *
 * @return RB960X_OK, or RB960X_ERR_ARG if @p rb is NULL.
 */
rb960xStatus_t rb960xEnd(rb960x_t *rb);

/**
 * @brief Initialise a driver context and identify the connected modem.
 *
 * Steps, stopping at the first failure:
 *  1. Set up the AT client and debug handler.
 *  2. Send "AT" until the modem responds, up to RB960X_INIT_ATTEMPTS times.
 *  3. Turn command echo off (ATE0).
 *  4. Read the model (ATI7).
 *  5. Read the IMEI (AT+CGSN). On the 9604 this is the cellular IMEI.
 *  6. Read the serial number: AT+GSN, or on the 9604, AT+USATSN, which also
 *     gives the satellite IMEI.
 *  7. 9604 only: read the current RAT mode (AT+URAT?), then switch to
 *     RB960X_INIT_RAT with rb960xSwitchRat() unless it is
 *     RB960X_INIT_RAT_NONE (the default is SBD). A switch resets the
 *     module, so it comes before the settings below.
 *  8. Set the DTR option (AT&D) and flow control (AT&K) from
 *     rockblock960x_config.h.
 *  9. Enable the SBDRING ring alert URC: AT+SBDMTA=1, or on the 9604
 *     AT+URING=1.
 *
 * Use this to supply your own port callbacks (an RTOS, a custom UART, a
 * test mock); otherwise rb960xBegin() is simpler.
 *
 * @param[out] rb            Context to initialise. Cleared before use.
 * @param[in]  port          Platform callbacks. Must stay valid for as long
 *                           as @p rb is used (see atPort_t).
 * @param[in]  debugHandler  Optional AT traffic trace (NULL to disable). It
 *                           is set up before the first "AT" so it also shows
 *                           rb960xInit()'s own commands. See
 *                           atClientSetDebugHandler().
 * @param[in]  debugUserData Passed back to @p debugHandler.
 *
 * @return RB960X_OK on success, otherwise the error from the step that
 *         failed. If ATI7 doesn't report a recognised model, rb->model is
 *         left as RB960X_MODEL_UNKNOWN.
 */
rb960xStatus_t rb960xInit(rb960x_t *rb, const atPort_t *port,
                            atDebugHandler_t debugHandler, void *debugUserData);

/**
 * @brief Switch the 9604 to a different radio access technology.
 *
 * Turns the radio off (AT+CFUN=0), selects the new RAT (AT+URAT), then
 * resets the module (AT+CFUN=16). It then waits for the module to respond
 * again, turns echo back off and reads the RAT back to confirm the switch.
 * If AT+URAT fails, the radio is turned back on (AT+CFUN=1). Does nothing
 * if the module is already in @p mode.
 *
 * The reset may undo the settings rb960xInit() made, so after a switch the
 * DTR option (AT&D), flow control (AT&K) and ring alert (AT+URING=1) are
 * set again. rb960xInit() also switches to RB960X_INIT_RAT.
 *
 * @note Blocks while the module resets (up to about 20 probes of 100 ms
 *       each).
 *
 * @param[in,out] rb    Initialised context. rb->ratMode is updated.
 * @param[in]     mode  RB960X_RAT_LTE_CAT_M1 or RB960X_RAT_SBD.
 *
 * @return RB960X_OK if the module is now in @p mode,
 *         RB960X_ERR_ARG if @p rb is NULL or @p mode isn't one of the above,
 *         RB960X_ERR_INVALID_MODEL if the model isn't a 9604,
 *         RB960X_ERR_PROTOCOL if every command worked but AT+URAT? still
 *         reports the old mode,
 *         or the error from the AT command that failed.
 */
rb960xStatus_t rb960xSwitchRat(rb960x_t *rb, rb960xRatMode_t mode);

/**
 * @brief Read the SIM card's ICCID (9604 in LTE Cat M1 mode only).
 *
 * Sends AT+CCID, trying up to 10 times 500 ms apart, because the ICCID can
 * take a moment to become available after a RAT switch.
 *
 * @param[in]  rb           Initialised context.
 * @param[out] iccid        Buffer for the ICCID. Always NULL terminated;
 *                          RB960X_ICCID_LEN is enough for a full ICCID.
 * @param[in]  iccidLength  Size of @p iccid in bytes.
 *
 * @return RB960X_OK on success,
 *         RB960X_ERR_ARG if a pointer is NULL or @p iccidLength is 0,
 *         RB960X_ERR_INVALID_MODEL if the model isn't a 9604,
 *         RB960X_ERR_WRONG_RAT if the 9604 isn't in LTE Cat M1 mode,
 *         or the AT+CCID error if every retry failed.
 */
rb960xStatus_t rb960xGetCellularIccid(rb960x_t *rb, char * const iccid, const size_t iccidLength);

/**
 * @brief Read the cellular module's firmware version (9604 only).
 *
 * Sends ATI9 and copies the reply as-is, e.g. "07.01.A00.01".
 *
 * @param[in]  rb             Initialised context.
 * @param[out] version        Buffer for the version string. Always NULL
 *                            terminated; cut short if it doesn't fit.
 * @param[in]  versionLength  Size of @p version in bytes.
 *
 * @return RB960X_OK on success,
 *         RB960X_ERR_ARG if a pointer is NULL or @p versionLength is 0,
 *         RB960X_ERR_INVALID_MODEL if the model isn't a 9604,
 *         or the ATI9 error.
 */
rb960xStatus_t rb960xGetCellularVersion(rb960x_t *rb, char * const version, const size_t versionLength);

/**
 * @brief Read the satellite (Iridium) modem's firmware version.
 *
 * - 9602 / 9603: sends ATI3 and copies the reply as-is, e.g. "TA21004".
 * - 9604: sends AT+USATINFO and copies the third field of the reply, e.g.
 *   "03.00.10" from "1,SBD,03.00.10". Only works in SBD mode.
 *
 * @param[in]  rb             Initialised context.
 * @param[out] version        Buffer for the version string. Always NULL
 *                            terminated; cut short if it doesn't fit.
 * @param[in]  versionLength  Size of @p version in bytes.
 *
 * @return RB960X_OK on success,
 *         RB960X_ERR_ARG if a pointer is NULL or @p versionLength is 0,
 *         RB960X_ERR_INVALID_MODEL if the model is unknown,
 *         RB960X_ERR_WRONG_RAT if a 9604 isn't in SBD mode,
 *         or the AT command's error.
 */
rb960xStatus_t rb960xGetSatelliteVersion(rb960x_t *rb, char * const version, const size_t versionLength);

/* --------------------------------------------------------------------------
 * SBD sessions
 *
 * These follow the SBD protocol one step at a time, and the driver never
 * starts a session by itself: every AT+SBDIX (which can cost credits) comes
 * from a call to rb960xInitiateSession() or rb960xInitiateSessionAsync().
 * Retrying is up to the application; the result's moResult says why a
 * session failed. A typical send:
 *
 *   1. rb960xSessionReady()     - cheap local checks and the last signal.
 *   2. rb960xWriteMoBuffer()    - load the message (local, free).
 *   3. rb960xInitiateSession()  - run the session (sends the MO message and
 *                                 checks the gateway for MT messages).
 *   4. If the result's mtStatus is 1, rb960xReadMtBuffer().
 *
 * To check for MT messages without sending anything, clear the MO buffer
 * with rb960xClearBuffers() and initiate a session: the MO buffer is not
 * emptied by a successful session, so otherwise it would be sent again.
 * ------------------------------------------------------------------------ */

/**
 * @brief Read the signal strength.
 *
 * - @p fresh false: AT+CSQF, the last strength the modem knows. Returns at
 *   once and doesn't wake the receiver.
 * - @p fresh true: AT+CSQ, a new reading. Usually takes about 2 s, up to
 *   10 s; waits at most RB960X_CSQ_TIMEOUT_MS.
 *
 * A local command: no session, no credits.
 *
 * @param[in,out] rb    Initialised context.
 * @param[in]     fresh true to take a new reading (AT+CSQ), false for the
 *                      last known one (AT+CSQF).
 * @param[out]    bars  Signal strength, 0 to 5. Set to 0 on any error.
 *
 * @return RB960X_OK if the signal was read,
 *         RB960X_ERR_ARG if @p rb or @p bars is NULL,
 *         RB960X_ERR_INVALID_MODEL if the model is unknown,
 *         RB960X_ERR_WRONG_RAT if a 9604 isn't in SBD mode,
 *         RB960X_ERR_BUSY if an asynchronous session is still running,
 *         RB960X_ERR_TIMEOUT if the modem didn't respond in time,
 *         or RB960X_ERR_PROTOCOL if the reply wasn't a valid reading.
 */
rb960xStatus_t rb960xGetSignal(rb960x_t *rb, bool fresh, uint8_t *bars);

/**
 * @brief Check whether a session is worth initiating now.
 *
 * Checks, in order, stopping at the first that fails:
 *  1. Local state: the model is known, a 9604 is in SBD mode, and no
 *     asynchronous session is running.
 *  2. No MT message is waiting unread (a session would erase it).
 *  3. The last known signal (AT+CSQF, see rb960xGetSignal()) is at least
 *     RB960X_MIN_SIGNAL_BARS.
 *
 * Only sends AT+CSQF, which is quick and free. RB960X_OK means a session
 * is worth attempting, not that it will succeed: the sky view can change,
 * and the gateway can still refuse it. For a fresh reading call
 * rb960xGetSignal(rb, true, ...) yourself.
 *
 * @param[in,out] rb  Initialised context.
 *
 * @return RB960X_OK if a session is worth initiating,
 *         RB960X_ERR_NO_SIGNAL if the last known signal is too weak,
 *         RB960X_ERR_MT_UNREAD if an MT message is waiting unread,
 *         RB960X_ERR_ARG if @p rb is NULL,
 *         or any other error from rb960xGetSignal().
 */
rb960xStatus_t rb960xSessionReady(rb960x_t *rb);

/**
 * @brief Write a message into the modem's mobile-originated (MO) buffer.
 *
 * A local command: no session, no credits. Replaces anything already in
 * the MO buffer. rb960xInitiateSession() sends it.
 * - 9602 / 9603: written as binary with AT+SBDWB.
 * - 9604: written hex encoded with AT+SBDW, because the 9604 doesn't
 *   support AT+SBDWB.
 *
 * @param[in] rb      Initialised context.
 * @param[in] data    Message bytes. Can be binary; it doesn't have to be
 *                    NULL terminated.
 * @param[in] length  Number of bytes in @p data, 1 to RB960X_MO_MAX.
 *
 * @return RB960X_OK if the message was written,
 *         RB960X_ERR_ARG if a pointer is NULL or @p length is 0,
 *         RB960X_ERR_TOO_LARGE if @p length is over RB960X_MO_MAX or the
 *         modem rejected the size,
 *         RB960X_ERR_INVALID_MODEL if the model is unknown,
 *         RB960X_ERR_WRONG_RAT if a 9604 isn't in SBD mode,
 *         RB960X_ERR_BUSY if an asynchronous session is still running,
 *         RB960X_ERR_TIMEOUT if the modem didn't respond in time,
 *         or RB960X_ERR_PROTOCOL if the modem replied with something
 *         unexpected (including a checksum mismatch).
 */
rb960xStatus_t rb960xWriteMoBuffer(rb960x_t *rb, const char * const data, const size_t length);

/**
 * @brief Clear the MO buffer, the MT buffer, or both (AT+SBDD).
 *
 * A local command: no session, no credits. Clear the MO buffer before a
 * session meant only to check for MT messages. Clearing the MT buffer
 * discards an unread MT message, which also clears RB960X_ERR_MT_UNREAD.
 *
 * @param[in,out] rb     Initialised context.
 * @param[in]     which  RB960X_BUFFER_MO, RB960X_BUFFER_MT or
 *                       RB960X_BUFFER_BOTH.
 *
 * @return RB960X_OK if the buffer(s) were cleared,
 *         RB960X_ERR_ARG if @p rb is NULL or @p which is out of range,
 *         RB960X_ERR_INVALID_MODEL if the model is unknown,
 *         RB960X_ERR_WRONG_RAT if a 9604 isn't in SBD mode,
 *         RB960X_ERR_BUSY if an asynchronous session is still running,
 *         RB960X_ERR_TIMEOUT, or RB960X_ERR_PROTOCOL if the modem reported
 *         an error.
 */
rb960xStatus_t rb960xClearBuffers(rb960x_t *rb, rb960xBuffer_t which);

/**
 * @brief Initiate an SBD session and wait for it to finish (AT+SBDIX).
 *
 * Sends whatever is in the MO buffer (nothing if it is empty) and checks
 * the gateway for MT messages, in one session. Every session can cost
 * credits. Makes one attempt; on RB960X_ERR_SESSION the result's
 * moResult says why, so the application can decide whether and when to
 * try again.
 *
 * Sets the modem's session timeout to RB960X_SBD_SESSION_TIMEOUT_S
 * (AT+SBDST) first, as the modem forgets it at power-up.
 *
 * A session empties the modem's MT buffer, so while an MT message is
 * waiting there unread this returns RB960X_ERR_MT_UNREAD without starting
 * one. Read it with rb960xReadMtBuffer() (or discard it with
 * rb960xClearBuffers()) first.
 *
 * Check the result's MT fields even after a failed MO: mtStatus 1 means a
 * message is now in the MT buffer, and mtQueued says how many more are
 * waiting at the gateway.
 *
 * @param[in,out] rb          Initialised context.
 * @param[in]     answerRing  true to answer an SBDRING ring alert
 *                            (AT+SBDIXA; see rb960x_t::ringAlert), false
 *                            for an ordinary session (AT+SBDIX). The 9604
 *                            has no AT+SBDIXA and always uses AT+SBDIX.
 * @param[in]     timeoutMs   How long to wait for the session, in
 *                            milliseconds. RB960X_SBDIX_TIMEOUT_MS is a
 *                            sensible default; keep it longer than
 *                            RB960X_SBD_SESSION_TIMEOUT_S so the modem's
 *                            own timeout ends a stalled session first.
 * @param[out]    result      Filled with the session result when the
 *                            return is RB960X_OK or RB960X_ERR_SESSION,
 *                            zeroed otherwise. May be NULL.
 *
 * @return RB960X_OK if the session worked (MO status 0 to 4),
 *         RB960X_ERR_SESSION if it ran but failed (see @p result),
 *         RB960X_ERR_MT_UNREAD if an MT message is waiting unread,
 *         RB960X_ERR_ARG if @p rb is NULL,
 *         RB960X_ERR_INVALID_MODEL if the model is unknown,
 *         RB960X_ERR_WRONG_RAT if a 9604 isn't in SBD mode,
 *         RB960X_ERR_BUSY if an asynchronous session is still running,
 *         RB960X_ERR_TIMEOUT if the modem didn't respond in time,
 *         or RB960X_ERR_PROTOCOL if the reply was malformed.
 */
rb960xStatus_t rb960xInitiateSession(rb960x_t *rb, bool answerRing, uint32_t timeoutMs, rb960xSessionResult_t *result);

/**
 * @brief Initiate an SBD session without waiting for it to finish.
 *
 * The same session as rb960xInitiateSession(), but returns as soon as the
 * command has been sent. Call rb960xProcess() regularly; it calls
 * @p callback with the result once the session finishes or @p timeoutMs
 * runs out. Until then, other functions that talk to the modem return
 * RB960X_ERR_BUSY.
 *
 * @param[in,out] rb          Initialised context.
 * @param[in]     answerRing  As for rb960xInitiateSession().
 * @param[in]     timeoutMs   As for rb960xInitiateSession().
 * @param[in]     callback    Called from rb960xProcess() when the session
 *                            finishes. Must not be NULL.
 * @param[in]     userData    Passed back to @p callback.
 *
 * @return RB960X_OK if the session has started (@p callback will be
 *         called),
 *         RB960X_ERR_ARG if @p rb or @p callback is NULL,
 *         or any error rb960xInitiateSession() returns before the session
 *         starts. On any error the session didn't start and @p callback
 *         won't be called.
 */
rb960xStatus_t rb960xInitiateSessionAsync(rb960x_t *rb, bool answerRing, uint32_t timeoutMs,
                                          rb960xSessionCallback_t callback, void *userData);

/**
 * @brief Read the modem's mobile-terminated (MT) buffer.
 *
 * A local command: no session, no credits. Reads whatever the last session
 * left in the MT buffer: binary with AT+SBDRB on the 9602 / 9603, hex
 * encoded with AT+SBDR on the 9604. Never starts a session; messages
 * arrive through rb960xInitiateSession().
 *
 * @param[in,out] rb       Initialised context.
 * @param[out]    buf      Buffer for the message. Not NULL terminated (it
 *                         can be binary); RB960X_MT_MAX bytes is always
 *                         enough.
 * @param[in]     bufSize  Size of @p buf in bytes.
 * @param[out]    length   Set to the message length, or 0 if there wasn't
 *                         one.
 *
 * @return RB960X_OK if a message was read into @p buf,
 *         RB960X_ERR_NO_MESSAGE if the MT buffer was empty,
 *         RB960X_ERR_ARG if a pointer is NULL,
 *         RB960X_ERR_INVALID_MODEL if the model is unknown,
 *         RB960X_ERR_WRONG_RAT if a 9604 isn't in SBD mode,
 *         RB960X_ERR_BUSY if an asynchronous session is still running,
 *         RB960X_ERR_TOO_LARGE if the message doesn't fit @p buf (it is
 *         discarded),
 *         RB960X_ERR_PROTOCOL if the message data was malformed (including
 *         an AT+SBDRB checksum mismatch), or RB960X_ERR_TIMEOUT.
 */
rb960xStatus_t rb960xReadMtBuffer(rb960x_t *rb, char *buf, size_t bufSize, size_t *length);

/**
 * @brief Describe an MO status, in the AT command guide's words.
 *
 * For logging and display. Use rb960xSessionResult_t::moResult, which the
 * session has already decoded for the modem's model.
 *
 * @param[in] moResult  rb960xSessionResult_t::moResult from the session.
 *
 * @return A constant string; never NULL. "Unknown MO status" for a value
 *         that isn't in rb960xMoStatus_t.
 */
const char *rb960xMoStatusText(rb960xMoStatus_t moResult);

/**
 * @brief Read the state of the SBD buffers and ring alert (AT+SBDSX).
 *
 * A local command: no session, no credits.
 *
 * @param[in]  rb         Initialised context.
 * @param[out] sbdStatus  Filled in on success; zeroed otherwise.
 *
 * @return RB960X_OK,
 *         RB960X_ERR_ARG if a pointer is NULL,
 *         RB960X_ERR_INVALID_MODEL if the model is unknown,
 *         RB960X_ERR_WRONG_RAT if a 9604 isn't in SBD mode,
 *         RB960X_ERR_BUSY if an asynchronous session is still running,
 *         RB960X_ERR_PROTOCOL if the reply was malformed, or
 *         RB960X_ERR_TIMEOUT.
 */
rb960xStatus_t rb960xGetSbdStatus(rb960x_t *rb, rb960xSbdStatus_t *sbdStatus);

/**
 * @brief Have rb960xProcess() poll the signal and report each reading.
 *
 * While a callback is set, rb960xProcess() reads the last known signal
 * (AT+CSQF, see rb960xGetSignal()) every RB960X_SIGNAL_POLL_MS and passes
 * the result to @p callback. The first poll happens on the next
 * rb960xProcess(). Polling is skipped while an asynchronous session is
 * running, and is off when RB960X_SIGNAL_POLL_MS is 0.
 *
 * AT+CSQF reports the strength the modem last knew, so it doesn't wake the
 * receiver. For a fresh reading call rb960xGetSignal(rb, true, ...).
 *
 * Call after rb960xInit(), which clears the callback.
 *
 * @param[in,out] rb        Initialised context. Does nothing if NULL.
 * @param[in]     callback  Called with each reading, or NULL to stop
 *                          polling.
 * @param[in]     userData  Passed back to @p callback.
 */
void rb960xSetSignalCallback(rb960x_t *rb, rb960xSignalCallback_t callback, void *userData);

/**
 * @brief Have rb960xProcess() report SBDRING ring alerts.
 *
 * A ring alert means the gateway has an MT message waiting. The driver
 * always records it in rb960x_t::ringAlert; this callback is an optional
 * way to hear about it instead of checking that field.
 *
 * The callback runs from rb960xProcess(), not when the SBDRING line
 * arrives: a ring alert can arrive in the middle of another command, so it
 * is held until the next rb960xProcess() that isn't finishing an
 * asynchronous session. It is skipped if a session has reached the
 * gateway in the meantime (which answers the ring). It is called once per
 * SBDRING; the gateway may repeat a ring alert until it is answered. A
 * ring alert that arrived before the callback was set is reported on the
 * next rb960xProcess().
 *
 * The callback may start a session, e.g.
 * rb960xInitiateSession(rb, true, ...) or rb960xInitiateSessionAsync(rb,
 * true, ...), which costs credits; the driver never answers a ring by
 * itself.
 *
 * Call after rb960xInit(), which clears the callback.
 *
 * @param[in,out] rb        Initialised context. Does nothing if NULL.
 * @param[in]     callback  Called after each ring alert, or NULL for none.
 * @param[in]     userData  Passed back to @p callback.
 */
void rb960xSetRingCallback(rb960x_t *rb, rb960xRingCallback_t callback, void *userData);

/**
 * @brief Service the modem: dispatch URCs, finish asynchronous sessions and
 *        poll the signal.
 *
 * Never blocks for long. Each call does one of:
 * - If an asynchronous session is running: checks whether it has finished
 *   (or timed out) and, if so, calls its callback.
 * - Otherwise: reads any pending modem output and dispatches URCs (notably
 *   SBDRING, which sets rb960x_t::ringAlert), calls the ring callback if a
 *   ring alert is waiting to be reported, then, if a signal callback is set,
 *   a poll is due and the ring callback didn't start an asynchronous
 *   session, sends AT+CSQF and calls the signal callback.
 *
 * Call this regularly from the application's main or idle loop.
 *
 * @param[in,out] rb  Initialised context. Does nothing if NULL.
 */
void rb960xProcess(rb960x_t *rb);


/* ==========================================================================
 * Low-level AT commands
 *
 * Each function sends one AT command and returns the modem's reply as-is,
 * without parsing it or updating the context. Prefer the high-level API
 * above; these are for features it doesn't cover yet.
 *
 * Common to all of them:
 *   - @p rb must be an initialised context. It is not checked for NULL.
 *   - Where there is a @p resp buffer, it receives the information line(s)
 *     from the reply, without the final "OK".
 *   - They return RB960X_OK, RB960X_ERR_TIMEOUT if the modem didn't respond
 *     in time, or RB960X_ERR_PROTOCOL if it replied with something other
 *     than "OK".
 * ========================================================================== */

/**
 * @brief AT: check the modem is present and responding.
 * @param[in,out] rb  Initialised context.
 * @param[in] timeoutMs  How long to wait for "OK", in milliseconds.
 */
rb960xStatus_t rb960xAtProbe(rb960x_t *rb, uint32_t timeoutMs);

/**
 * @brief ATE0 / ATE1: turn command echo off or on.
 * @param[in,out] rb  Initialised context.
 * @param[in] enable  true sends ATE1, false sends ATE0.
 */
rb960xStatus_t rb960xAtEcho(rb960x_t *rb, bool enable);

/**
 * @brief ATI3: satellite modem software revision, e.g. "TA21004"
 *        (9602 / 9603).
 */
rb960xStatus_t rb960xAtI3(rb960x_t *rb, char *resp, size_t respSize);

/**
 * @brief ATI7: hardware/model banner, e.g. "BOOT07d4/9603NGrevA/04/RAW0f".
 */
rb960xStatus_t rb960xAtI7(rb960x_t *rb, char *resp, size_t respSize);

/**
 * @brief ATI9: cellular module firmware version, e.g. "07.01.A00.01" (9604).
 */
rb960xStatus_t rb960xAtI9(rb960x_t *rb, char *resp, size_t respSize);

/** @brief ATI11. */
rb960xStatus_t rb960xAtI11(rb960x_t *rb, char *resp, size_t respSize);

/**
 * @brief AT+CGSN: IMEI, e.g. "300234010753370". On the 9604 this is the
 *        cellular IMEI.
 */
rb960xStatus_t rb960xAtCgsn(rb960x_t *rb, char *resp, size_t respSize);

/**
 * @brief AT+GSN: satellite modem serial number (9602 / 9603). Not supported
 *        on the 9604; use rb960xAtUsatsn() instead.
 */
rb960xStatus_t rb960xAtGsn(rb960x_t *rb, char *resp, size_t respSize);

/**
 * @brief AT+USATSN: satellite serial number and IMEI (9604). The reply
 *        looks like "+USATSN: <serial>[,<sbdImei>]".
 */
rb960xStatus_t rb960xAtUsatsn(rb960x_t *rb, char *resp, size_t respSize);

/**
 * @brief AT+USATINFO: satellite modem information (9604), e.g.
 *        "1,SBD,03.00.10", where the third field is the firmware version.
 */
rb960xStatus_t rb960xAtUsatinfo(rb960x_t *rb, char *resp, size_t respSize);

/**
 * @brief AT+CSQF: last known signal quality, returned immediately without
 *        taking a new measurement.
 */
rb960xStatus_t rb960xAtCsqf(rb960x_t *rb, char *resp, size_t respSize);

/**
 * @brief AT+CSQ: take a fresh signal quality reading, e.g. "+CSQ:4". Waits
 *        up to RB960X_CSQ_TIMEOUT_MS, as the modem may take up to 10 s.
 */
rb960xStatus_t rb960xAtCsq(rb960x_t *rb, char *resp, size_t respSize);

/**
 * @brief AT&Dn: set how the modem responds to the DTR line.
 * @param[in,out] rb  Initialised context.
 * @param[in] dtrOption  0 to RB960X_DTR_MAX (RB960X_DTR_MAX_9604 on the
 *                       9604).
 * @return As above, or RB960X_ERR_ARG if @p dtrOption is out of range for
 *         this model.
 */
rb960xStatus_t rb960xAtDn(rb960x_t *rb, const uint32_t dtrOption);

/**
 * @brief AT&Kn: set flow control.
 * @param[in,out] rb  Initialised context.
 * @param[in] flowOption  RB960X_FLOW_DISABLED or RB960X_FLOW_RTS_CTS.
 * @return As above, or RB960X_ERR_ARG if @p flowOption is anything else.
 */
rb960xStatus_t rb960xAtKn(rb960x_t *rb, const uint32_t flowOption);

/**
 * @brief AT+SBDMTA: turn SBD ring alerts (the SBDRING URC) on or off.
 *        Not supported on the 9604.
 * @param[in,out] rb  Initialised context.
 * @param[in] enable  true sends AT+SBDMTA=1, false sends AT+SBDMTA=0.
 */
rb960xStatus_t rb960xAtSbdMta(rb960x_t *rb, const bool enable);

/**
 * @brief AT+SBDST: set the SBD session timeout. Applies to AT+SBDI,
 *        AT+SBDIX and AT+SBDDET; a session that runs longer ends with MO
 *        status 17. Not saved: it returns to 0 (no limit) at power-up.
 * @param[in,out] rb  Initialised context.
 * @param[in] seconds  Timeout in seconds; 0 means no limit.
 */
rb960xStatus_t rb960xAtSbdst(rb960x_t *rb, const uint16_t seconds);

/**
 * @brief AT+CFUN: set the module functionality level (9604).
 * @param[in,out] rb  Initialised context.
 * @param[in] function  One of rb960xModuleFunc_t.
 */
rb960xStatus_t rb960xAtCfun(rb960x_t *rb, const uint8_t function);

/**
 * @brief AT+URAT: select the radio access technology (9604). Takes effect
 *        after a module reset; rb960xSwitchRat() handles the full sequence.
 * @param[in,out] rb  Initialised context.
 * @param[in] mode  7 for LTE Cat M1, 10 for SBD.
 */
rb960xStatus_t rb960xAtUrat(rb960x_t *rb, const uint8_t mode);

/**
 * @brief AT+CCID: SIM card ICCID (9604, LTE Cat M1 mode), e.g.
 *        "+CCID: 8944107253918064278".
 */
rb960xStatus_t rb960xAtCcid(rb960x_t *rb, char *resp, size_t respSize);

/**
 * @brief AT+URAT?: read the current radio access technology (9604), e.g.
 *        "+URAT: 10".
 */
rb960xStatus_t rb960xAtReadUrat(rb960x_t *rb, char *resp, size_t respSize);


#ifdef __cplusplus
}
#endif

#if defined(ARDUINO) && defined(__cplusplus)
/**
 * @brief rb960xBegin() for Arduino: initialise the modem on @p port.
 *
 * Call begin() on the port first, e.g. Serial1.begin(115200) (19200 for
 * the 9602 and 9603). Works with any
 * Stream (HardwareSerial, SoftwareSerial, ...).
 */
inline rb960xStatus_t rb960xBegin(rb960x_t *rb, Stream &port,
                                    atDebugHandler_t debugHandler = NULL,
                                    void *debugUserData = NULL)
{
    return rb960xBegin(rb, &port, 0, debugHandler, debugUserData);
}
#endif

#endif /* ROCKBLOCK960X_H */
