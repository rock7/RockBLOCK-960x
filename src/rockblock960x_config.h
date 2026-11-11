/*
 * Compile-time configuration.
 *
 * Every value is wrapped in #ifndef, so it can be overridden from the build
 * system without editing this file, e.g. -DRB960X_SBD_SESSION_TIMEOUT_S=120
 * or build_flags in platformio.ini.
 */
#ifndef ROCKBLOCK960X_CONFIG_H
#define ROCKBLOCK960X_CONFIG_H

/* --------------------------------------------------------------------------
 * Buffer sizes
 * ------------------------------------------------------------------------ */

/* AT response line buffer, in bytes. rb960x_t holds two of these (the AT
 * client's line buffer and the stored session reply), so it sets the size
 * of the context; keep it small on AVR targets. The longest line expected
 * is the +SBDIX reply, under 50 characters. MT messages are read raw, not
 * through this buffer. */
#ifndef RB960X_LINE_BUF_SIZE
#define RB960X_LINE_BUF_SIZE 96
#endif

/* IMEI buffers in rb960x_t, including the '\0'. An IMEI (AT+CGSN) is
 * always 15 digits. */
#ifndef RB960X_IMEI_LEN
#define RB960X_IMEI_LEN 16
#endif

/* Serial number buffer in rb960x_t, including the '\0'. The AT command
 * guides don't fix the format of AT+GSN, so it gets more room. */
#ifndef RB960X_SERIAL_LEN
#define RB960X_SERIAL_LEN 32
#endif

/* Suggested size for the buffer passed to rb960xGetCellularIccid(),
 * including the '\0'. An ICCID is usually 19 digits. */
#ifndef RB960X_ICCID_LEN
#define RB960X_ICCID_LEN 20
#endif

/* Suggested size for the buffer passed to rb960xGetCellularVersion() and
 * rb960xGetSatelliteVersion(), including the '\0'. */
#ifndef RB960X_MODEM_VERSION_LEN
#define RB960X_MODEM_VERSION_LEN 20
#endif

/* --------------------------------------------------------------------------
 * SBD message limits, in bytes
 * ------------------------------------------------------------------------ */

/* Largest MO message. 340 bytes on every model (9602, 9603 and 9604).
 * Must be 65535 or less. */
#ifndef RB960X_MO_MAX
#define RB960X_MO_MAX 340
#endif

/* Largest MT message. 270 bytes on the 9604. The 9602/9603 can receive up
 * to 1960 bytes, but the Iridium network normally limits MT messages to
 * 135 or 270 bytes. */
#ifndef RB960X_MT_MAX
#define RB960X_MT_MAX 270
#endif

/* --------------------------------------------------------------------------
 * Initialisation
 * ------------------------------------------------------------------------ */

/* rb960xInit() sends "AT" up to RB960X_INIT_ATTEMPTS times, waiting
 * RB960X_AT_PROBE_TIMEOUT_MS (in ms) for each reply, because the modem may
 * still be booting or charging its super capacitor. Several short tries
 * respond sooner than one long timeout. */
#ifndef RB960X_AT_PROBE_TIMEOUT_MS
#define RB960X_AT_PROBE_TIMEOUT_MS 2000
#endif

#ifndef RB960X_INIT_ATTEMPTS
#define RB960X_INIT_ATTEMPTS 3
#endif

/* DTR option sent with AT&Dn during rb960xInit(). 0 (the default here)
 * ignores DTR, which the guides require in 3-wire mode (RX, TX and GND
 * only).
 *   0: Ignore DTR transitions.
 *   1: Return to command mode without dropping the connection after an
 *      ON-to-OFF DTR transition.
 *   2: Hang up, disable auto-answer and return to command mode after an
 *      ON-to-OFF DTR transition (the modem's own default).
 *   3: Reset and return to command mode after an ON-to-OFF DTR transition.
 *      9602/9603 only; the 9604 accepts 0 to 2.
 * Checked at compile time, and against the model during rb960xInit(). */
#ifndef RB960X_DTR_OPTION
#define RB960X_DTR_OPTION 0
#endif

/* Flow control option sent with AT&Kn during rb960xInit(). 0 (the default
 * here) suits a 3-wire connection without RTS and CTS.
 *   0: No flow control.
 *   3: RTS/CTS hardware flow control (the modem's own default).
 * The guides say that XON/XOFF (4) isn't supported by the 9602/9603 or the
 * 9604. Checked at compile time. */
#ifndef RB960X_FLOW_CONTROL
#define RB960X_FLOW_CONTROL 0
#endif

/* Radio access technology (RAT) the 9604 is put in by rb960xInit(). The
 * values match rb960xRatMode_t. Ignored on the 9602/9603, which only have
 * SBD.
 *   RB960X_INIT_RAT_NONE:     Leave the module in whatever mode it is in.
 *   RB960X_INIT_RAT_CELLULAR: Switch to cellular LTE Cat M1 (AT+URAT=7).
 *   RB960X_INIT_RAT_SBD:      Switch to Iridium SBD (AT+URAT=10).
 * Switching resets the module (see rb960xSwitchRat()), which makes
 * rb960xInit() take longer; there's no reset if it's already in the mode.
 * Checked at compile time. */
#define RB960X_INIT_RAT_NONE 0
#define RB960X_INIT_RAT_CELLULAR 1
#define RB960X_INIT_RAT_SBD 2

#ifndef RB960X_INIT_RAT
#define RB960X_INIT_RAT RB960X_INIT_RAT_SBD
#endif

/* --------------------------------------------------------------------------
 * Timeouts
 * ------------------------------------------------------------------------ */

/* How long to wait for the reply to an ordinary AT command, in ms. Used for
 * every command without its own timeout below, and for each chunk of an MO
 * write or MT read. */
#ifndef RB960X_DEFAULT_TIMEOUT_MS
#define RB960X_DEFAULT_TIMEOUT_MS 5000
#endif

/* How long to wait for AT+CSQ (a fresh signal reading), in ms. It usually
 * takes about 2 s, but can take up to 10 s while the modem acquires the
 * system or hands over between satellites. */
#ifndef RB960X_CSQ_TIMEOUT_MS
#define RB960X_CSQ_TIMEOUT_MS 15000
#endif

/* The modem's SBD session timeout, in seconds (1 to 65535), sent with
 * AT+SBDST before every AT+SBDIX. The modem abandons a session that runs
 * longer and reports MO status 17 (gateway not responding). The modem
 * forgets the setting at power-up and defaults to 0 (no limit), which
 * could leave it busy with a session after the host has stopped waiting,
 * so the driver always sets it. */
#ifndef RB960X_SBD_SESSION_TIMEOUT_S
#define RB960X_SBD_SESSION_TIMEOUT_S 60
#endif

/* Suggested timeout for rb960xInitiateSession(), in ms: how long the host
 * waits for AT+SBDIX to finish. The session talks to the satellite, so it
 * takes much longer than an ordinary command. Defaults to the session
 * timeout plus 30 s, so the modem's own timeout normally ends a stalled
 * session before the host gives up on it. */
#ifndef RB960X_SBDIX_TIMEOUT_MS
#define RB960X_SBDIX_TIMEOUT_MS ((RB960X_SBD_SESSION_TIMEOUT_S + 30UL) * 1000UL)
#endif

/* --------------------------------------------------------------------------
 * Signal
 * ------------------------------------------------------------------------ */

/* Minimum signal strength, in bars (0 to 5), for rb960xSessionReady() to
 * report a session as worth initiating. The AT command guides don't
 * recommend a minimum; 1 only rules out 0 bars (no usable signal). The
 * reading is a snapshot, so it doesn't guarantee the session will work.
 * rb960xInitiateSession() doesn't check it. */
#ifndef RB960X_MIN_SIGNAL_BARS
#define RB960X_MIN_SIGNAL_BARS 1
#endif

/* How often rb960xProcess() polls the signal (AT+CSQF) while a signal
 * callback is set with rb960xSetSignalCallback(), in ms. 0 turns polling
 * off. */
#ifndef RB960X_SIGNAL_POLL_MS
#define RB960X_SIGNAL_POLL_MS 30000UL
#endif

#endif /* ROCKBLOCK960X_CONFIG_H */
