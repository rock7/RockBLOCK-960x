"""Python bindings for the RockBLOCK-960x C library (via ctypes).

The C driver talks to the modem through a few port callbacks (write, read,
yield). This wrapper implements them in Python, on a pyserial port
that begin() opens, or on any object you pass it with non-blocking
``read(n) -> bytes`` and ``write(data)`` methods.

    import rockblock960x

    with rockblock960x.RockBlock960x() as rb:
        rb.begin("/dev/ttyUSB0")
        if rb.session_ready():
            rb.write_mo_buffer("Hello World!")
            result = rb.initiate_session()
            if result.mt_status == 1:
                print(rb.read_mt_buffer())

The driver never starts an SBD session (which can cost credits) by
itself: only initiate_session() or initiate_session_async() does, and
retrying is up to you. For the async session, set_ring_callback() and
set_signal_callback(), call process() regularly from your main loop; the
callbacks run inside it.

Build the shared library first, from the repo root:

    cmake -B build && cmake --build build
"""

import ctypes
import enum
import time
from dataclasses import dataclass
from typing import Callable, Optional

from . import _native as _n

__all__ = [
    "RockBlock960x",
    "RockBlock960xError",
    "SessionResult",
    "Status",
    "Buffer",
    "Model",
    "RatMode",
    "MoStatus",
    "mo_status_text",
    "MO_MAX",
    "MT_MAX",
    "SBDIX_TIMEOUT_MS",
    "SIGNAL_POLL_MS",
]
__version__ = "0.1.0"  # managed by scripts/version.py — do not edit by hand

# Limits from the C build that was loaded (rockblock960x_config.h).
MO_MAX = int(_n.rb960xPyMoMax())
MT_MAX = int(_n.rb960xPyMtMax())
SBDIX_TIMEOUT_MS = int(_n.rb960xPySbdixTimeoutMs())
# How often process() polls the signal for set_signal_callback(); 0 = never.
SIGNAL_POLL_MS = int(_n.rb960xPySignalPollMs())
_ICCID_LEN = int(_n.rb960xPyIccidLen())
_VERSION_LEN = int(_n.rb960xPyVersionLen())

# at_client's atStatus_t, for RockBlock960x.command().
_AT_STATUS = ["OK", "ERROR", "CME ERROR", "CMS ERROR", "TIMEOUT", "IO", "PARAM", "OVERFLOW", "BUSY", "PENDING"]


class Status(enum.IntEnum):
    """rb960xStatus_t."""

    OK = 0
    ERR_ARG = -1
    ERR_HAL = -2
    ERR_TIMEOUT = -3
    ERR_PROTOCOL = -4
    ERR_SESSION = -5
    ERR_NO_MESSAGE = -6
    ERR_TOO_LARGE = -7
    ERR_INVALID_MODEL = -8
    ERR_WRONG_RAT = -9
    ERR_BUSY = -10
    ERR_PORT = -11
    ERR_MT_UNREAD = -12
    ERR_NO_SIGNAL = -13


class Buffer(enum.IntEnum):
    """rb960xBuffer_t: which SBD buffer(s) clear_buffers() clears."""

    MO = 0
    MT = 1
    BOTH = 2


class Model(enum.IntEnum):
    """rb960xModel_t."""

    UNKNOWN = 0
    RB9602 = 1
    RB9603 = 2
    RB9604 = 3


class RatMode(enum.IntEnum):
    """rb960xRatMode_t (only the 9604 has more than one)."""

    UNKNOWN = 0
    LTE_CAT_M1 = 1
    SBD = 2


class MoStatus(enum.IntEnum):
    """rb960xMoStatus_t: the MO status, decoded for the modem's model.

    Named values equal the raw code. A code only gets its name on models
    whose AT command guide defines it; otherwise it is one of the RESERVED_*
    values (above 255).
    """

    OK = 0
    OK_MT_TOO_BIG = 1  # 9602/9603 only
    OK_LOCATION_UPDATE_REJECTED = 2
    GSS_CALL_TIMEOUT = 10  # 10 to 14: 9602/9603 only
    GSS_QUEUE_FULL = 11
    TOO_MANY_SEGMENTS = 12
    GSS_SESSION_INCOMPLETE = 13
    INVALID_SEGMENT_SIZE = 14
    ACCESS_DENIED = 15
    LOCKED = 16
    GATEWAY_NOT_RESPONDING = 17
    RF_DROP = 18
    LINK_FAILURE = 19  # 9602/9603 only
    NO_SERVICE = 32
    ANTENNA_FAULT = 33
    RADIO_DISABLED = 34
    BUSY = 35
    WAIT_REGISTRATION = 36
    SERVICE_DISABLED = 37
    TRAFFIC_MANAGEMENT = 38
    BAND_VIOLATION = 64  # 9602/9603 only
    HARDWARE_ERROR = 65
    RESERVED_SUCCESS = 256
    RESERVED_LOCATION_UPDATE_FAILURE = 257  # 9604 only
    RESERVED_FAILURE = 258


class RockBlock960xError(Exception):
    """A driver call failed. ``status`` is the Status code."""

    def __init__(self, status: int, what: str):
        try:
            self.status = Status(status)
            name = self.status.name
        except ValueError:
            self.status = status
            name = str(status)
        super().__init__(f"{what} failed: {name}")


def _check(status: int, what: str) -> None:
    if status != Status.OK:
        raise RockBlock960xError(status, what)


def mo_status_text(mo_result: int) -> str:
    """rb960xMoStatusText(): the AT command guide's description of an MO status."""
    return _n.rb960xMoStatusText(int(mo_result)).decode()


@dataclass(frozen=True)
class SessionResult:
    """rb960xSessionResult_t, plus whether the message went."""

    mo_status: int
    mo_result: MoStatus
    momsn: int
    mt_status: int
    mtmsn: int
    mt_length: int
    mt_queued: int

    @property
    def sent(self) -> bool:
        """MO status 0 to 4 means the session worked."""
        return self.mo_status <= 4

    @property
    def mo_status_text(self) -> str:
        """The AT command guide's description of mo_result."""
        return mo_status_text(self.mo_result)

    @classmethod
    def _from_c(cls, c: _n.SessionResultC) -> "SessionResult":
        return cls(c.moStatus, MoStatus(c.moResult), c.momsn, c.mtStatus, c.mtmsn, c.mtLength, c.mtQueued)


DebugHandler = Callable[[str, bytes], None]
"""Called with ("TX" or "RX", data) for every line to or from the modem."""

SessionHandler = Callable[["RockBlock960x", "Status", Optional[SessionResult]], None]
"""Called from process() when initiate_session_async() finishes, with
(rb, status, result). ``result`` is set when status is OK or ERR_SESSION,
None otherwise (e.g. ERR_TIMEOUT)."""

SignalHandler = Callable[["RockBlock960x", "Status", int], None]
"""Called from process() with (rb, status, bars) for each signal poll."""

RingHandler = Callable[["RockBlock960x"], None]
"""Called from process() with (rb) after an SBDRING ring alert."""


def _status_of(value: int):
    try:
        return Status(value)
    except ValueError:
        return value


class RockBlock960x:
    """One RockBLOCK modem.

    Create it, then call begin() to open the port and identify the modem;
    end() closes the port again. Also a context manager that calls end() on
    exit.

    Not thread safe: use one RockBlock960x from one thread.
    """

    def __init__(self):
        self._port = None
        self._owns_port = False
        self._callback_error: Optional[BaseException] = None
        self._debug: Optional[DebugHandler] = None
        self._session_handler: Optional[SessionHandler] = None
        self._signal_handler: Optional[SignalHandler] = None
        self._ring_handler: Optional[RingHandler] = None
        self._handler_error: Optional[BaseException] = None
        self._begun = False

        # rb960x_t is opaque here: allocate sizeof(rb960x_t) bytes. A
        # c_uint64 array guarantees 8-byte alignment, which a char buffer
        # doesn't. Zeroed, so the identity properties read as unknown/empty
        # until begin() succeeds.
        words = (int(_n.rb960xPySizeof()) + 7) // 8
        self._rb = (ctypes.c_uint64 * words)()
        self._rbp = ctypes.cast(self._rb, ctypes.c_void_p)

        # Keep every CFUNCTYPE object referenced from self: if Python
        # garbage-collects one, C ends up calling freed memory.
        self._write_cb = _n.WRITE_FN(self._on_write)
        self._read_cb = _n.READ_FN(self._on_read)
        self._yield_cb = _n.VOID_FN(self._on_yield)
        self._debug_cb = _n.DEBUG_FN(self._on_debug)
        self._session_cb = _n.SESSION_FN(self._on_session)
        self._signal_cb = _n.SIGNAL_FN(self._on_signal)
        self._ring_cb = _n.RING_FN(self._on_ring)

        self._at_port = _n.AtPort()
        self._at_port.userData = None
        self._at_port.write = self._write_cb
        self._at_port.read = self._read_cb
        self._at_port.yield_ = self._yield_cb
        self._at_port.lock = _n.VOID_FN()
        self._at_port.unlock = _n.VOID_FN()

    # ---- Begin / end ------------------------------------------------------

    def begin(self, port, baud: int = 115200, debug: Optional[DebugHandler] = None) -> None:
        """Open the port and run rb960xInit(), which identifies the modem.

        ``port`` is a serial device name ("/dev/ttyUSB0", "COM3"), opened
        here with pyserial at ``baud`` (115200 suits the 9604; the 9602 and
        9603 need ``baud=19200``), or an already open object with
        non-blocking ``read(n) -> bytes`` and ``write(data)`` (``baud`` is
        then ignored). ``debug`` is called with ("TX" or "RX", data) for
        every line to or from the modem.

        Raises RockBlock960xError if the modem doesn't answer, or the serial
        exception if the port can't be opened. Calling begin() again first
        calls end().
        """
        self.end()
        # rb960xInit() clears the driver's callbacks, so forget ours too.
        self._session_handler = None
        self._signal_handler = None
        self._ring_handler = None
        # Clear the context in case an earlier begin() partly filled it.
        ctypes.memset(self._rb, 0, ctypes.sizeof(self._rb))

        if isinstance(port, str):
            import serial

            port = serial.Serial(port, baud, timeout=0, write_timeout=5)
            self._owns_port = True
        self._port = port
        self._debug = debug

        try:
            self._call(
                _n.rb960xInit,
                "rb960xInit",
                self._rbp,
                ctypes.byref(self._at_port),
                self._debug_cb if debug else _n.DEBUG_FN(),
                None,
                check_begun=False,
            )
        except BaseException:
            self.end()
            raise
        self._begun = True

    def end(self) -> None:
        """Close the serial port if begin() opened it. Safe to call more than once."""
        if self._owns_port and self._port is not None:
            self._port.close()
        self._port = None
        self._owns_port = False
        self._begun = False

    def __enter__(self) -> "RockBlock960x":
        return self

    def __exit__(self, *exc) -> None:
        self.end()

    # ---- Port callbacks (called from C) -------------------------------------
    # An exception must never escape into C, so each one catches everything,
    # remembers it, and reports an I/O error. _call() re-raises it afterwards.

    def _on_write(self, _user, data, length):
        try:
            self._port.write(ctypes.string_at(data, length))
            return length
        except BaseException as e:  # noqa: BLE001
            self._callback_error = self._callback_error or e
            return -1

    def _on_read(self, _user, buf, length):
        try:
            chunk = self._port.read(length)
            if chunk:
                ctypes.memmove(buf, chunk, len(chunk))
            return len(chunk)
        except BaseException as e:  # noqa: BLE001
            self._callback_error = self._callback_error or e
            return -1

    def _on_yield(self, _user):
        time.sleep(0.001)

    def _on_debug(self, direction, data, length, _user):
        try:
            self._debug("TX" if direction == 0 else "RX", ctypes.string_at(data, length))
        except BaseException as e:  # noqa: BLE001
            self._callback_error = self._callback_error or e

    # Session and signal callbacks, called from C inside rb960xProcess().
    # They may call back into the driver (e.g. read_mt_buffer()), which
    # resets _callback_error, so their own exceptions go to _handler_error
    # and process() raises them.

    def _on_session(self, _rb, status, result, _user):
        handler, self._session_handler = self._session_handler, None
        try:
            if handler is not None:
                session = SessionResult._from_c(result.contents) if result else None
                handler(self, _status_of(status), session)
        except BaseException as e:  # noqa: BLE001
            self._handler_error = self._handler_error or e

    def _on_signal(self, _rb, status, bars, _user):
        try:
            if self._signal_handler is not None:
                self._signal_handler(self, _status_of(status), int(bars))
        except BaseException as e:  # noqa: BLE001
            self._handler_error = self._handler_error or e

    def _on_ring(self, _rb, _user):
        try:
            if self._ring_handler is not None:
                self._ring_handler(self)
        except BaseException as e:  # noqa: BLE001
            self._handler_error = self._handler_error or e

    def _require_begun(self) -> None:
        # The driver doesn't check for an initialised context, so calling
        # into it before begin() would dereference a NULL port in C.
        if not self._begun:
            raise RuntimeError("call begin() first")

    def _call(self, fn, what, *args, check_begun=True):
        """Call a driver function, re-raising anything a callback threw.

        ``check_begun=False`` skips the begin() check (for rb960xInit itself).
        """
        if check_begun:
            self._require_begun()
        self._callback_error = None
        status = fn(*args)
        if self._callback_error is not None:
            err, self._callback_error = self._callback_error, None
            raise err
        _check(status, what)
        return status

    # ---- Identity (read during init) ------------------------------------------

    @property
    def model(self) -> Model:
        return Model(_n.rb960xPyModel(self._rbp))

    @property
    def rat_mode(self) -> RatMode:
        return RatMode(_n.rb960xPyRatMode(self._rbp))

    @property
    def imei(self) -> str:
        """Satellite (Iridium) IMEI."""
        return _n.rb960xPyImei(self._rbp).decode(errors="replace")

    @property
    def cellular_imei(self) -> str:
        """Cellular IMEI (9604 only, else empty)."""
        return _n.rb960xPyCellularImei(self._rbp).decode(errors="replace")

    @property
    def serial(self) -> str:
        return _n.rb960xPySerial(self._rbp).decode(errors="replace")

    # ---- Driver API -------------------------------------------------------------

    def satellite_version(self) -> str:
        buf = ctypes.create_string_buffer(_VERSION_LEN)
        self._call(_n.rb960xGetSatelliteVersion, "rb960xGetSatelliteVersion", self._rbp, buf, len(buf))
        return buf.value.decode(errors="replace")

    def cellular_version(self) -> str:
        """9604 only."""
        buf = ctypes.create_string_buffer(_VERSION_LEN)
        self._call(_n.rb960xGetCellularVersion, "rb960xGetCellularVersion", self._rbp, buf, len(buf))
        return buf.value.decode(errors="replace")

    def iccid(self) -> str:
        """9604 in LTE Cat M1 mode only."""
        buf = ctypes.create_string_buffer(_ICCID_LEN)
        self._call(_n.rb960xGetCellularIccid, "rb960xGetCellularIccid", self._rbp, buf, len(buf))
        return buf.value.decode(errors="replace")

    def switch_rat(self, mode: RatMode) -> None:
        """9604 only. Blocks while the module resets."""
        self._call(_n.rb960xSwitchRat, "rb960xSwitchRat", self._rbp, int(mode))

    # ---- SBD session state (kept up to date by the driver) ------------------

    @property
    def ring_alert(self) -> bool:
        """An SBDRING ring alert arrived and no session has answered it yet."""
        return bool(_n.rb960xPyRingAlert(self._rbp))

    @property
    def mt_waiting(self) -> bool:
        """A session left an MT message in the modem that hasn't been read."""
        return bool(_n.rb960xPyMtWaiting(self._rbp))

    @property
    def mt_queued(self) -> int:
        """MT messages still waiting at the gateway, from the last session."""
        return int(_n.rb960xPyMtQueued(self._rbp))

    @property
    def session_pending(self) -> bool:
        """An initiate_session_async() session is running."""
        return bool(_n.rb960xPySessionPending(self._rbp))

    @property
    def session_count(self) -> int:
        """SBD sessions started since begin(), to keep an eye on cost."""
        return int(_n.rb960xPySessionCount(self._rbp))

    # ---- SBD sessions -------------------------------------------------------------

    def get_signal(self, fresh: bool = False) -> int:
        """rb960xGetSignal(): signal bars, 0-5.

        ``fresh=False`` reads the last known value (AT+CSQF, instant);
        ``fresh=True`` takes a new reading (AT+CSQ, a few seconds).
        """
        bars = ctypes.c_uint8(0)
        self._call(_n.rb960xGetSignal, "rb960xGetSignal", self._rbp, fresh, ctypes.byref(bars))
        return bars.value

    def session_ready(self) -> bool:
        """rb960xSessionReady(): is a session worth initiating now?

        Cheap local checks plus the last known signal (AT+CSQF). Returns
        False if the signal is too weak. Raises RockBlock960xError for
        anything else, e.g. ERR_MT_UNREAD if an MT message must be read
        first (a session would erase it).
        """
        self._require_begun()
        self._callback_error = None
        status = _n.rb960xSessionReady(self._rbp)
        if self._callback_error is not None:
            err, self._callback_error = self._callback_error, None
            raise err
        if status == Status.ERR_NO_SIGNAL:
            return False
        _check(status, "rb960xSessionReady")
        return True

    def write_mo_buffer(self, data) -> None:
        """rb960xWriteMoBuffer(): load an MO message (no session, no credits)."""
        if isinstance(data, str):
            data = data.encode()
        self._call(_n.rb960xWriteMoBuffer, "rb960xWriteMoBuffer", self._rbp, data, len(data))

    def clear_buffers(self, which: Buffer = Buffer.MO) -> None:
        """rb960xClearBuffers(): clear the MO buffer, the MT buffer, or both."""
        self._call(_n.rb960xClearBuffers, "rb960xClearBuffers", self._rbp, int(which))

    def initiate_session(self, answer_ring: bool = False, timeout_ms: int = SBDIX_TIMEOUT_MS) -> SessionResult:
        """rb960xInitiateSession(): run one SBD session (blocking).

        Sends whatever is in the MO buffer and checks for MT messages. Can
        cost credits. Returns the session result whether or not the MO part
        worked; check ``result.sent``, ``result.mo_result`` and
        ``result.mt_status``. Retrying is up to you. Pass
        ``answer_ring=rb.ring_alert`` to answer a ring alert. Raises
        RockBlock960xError for anything else (timeout, ERR_MT_UNREAD, ...).
        """
        self._require_begun()
        result = _n.SessionResultC()
        self._callback_error = None
        status = _n.rb960xInitiateSession(self._rbp, answer_ring, timeout_ms, ctypes.byref(result))
        if self._callback_error is not None:
            err, self._callback_error = self._callback_error, None
            raise err
        if status not in (Status.OK, Status.ERR_SESSION):
            raise RockBlock960xError(status, "rb960xInitiateSession")
        return SessionResult._from_c(result)

    def initiate_session_async(
        self,
        callback: SessionHandler,
        answer_ring: bool = False,
        timeout_ms: int = SBDIX_TIMEOUT_MS,
    ) -> None:
        """rb960xInitiateSessionAsync(): start one SBD session and return.

        The same session as initiate_session(), but it returns as soon as
        the command is sent. Keep calling process(); it calls
        ``callback(rb, status, result)`` once the session finishes or times
        out. Until then, other modem calls raise ERR_BUSY. Raises
        RockBlock960xError if the session couldn't start (the callback is
        then never called).
        """
        self._require_begun()
        self._callback_error = None
        status = _n.rb960xInitiateSessionAsync(self._rbp, answer_ring, timeout_ms, self._session_cb, None)
        if self._callback_error is not None:
            err, self._callback_error = self._callback_error, None
            raise err
        _check(status, "rb960xInitiateSessionAsync")
        self._session_handler = callback

    def set_signal_callback(self, callback: Optional[SignalHandler]) -> None:
        """rb960xSetSignalCallback(): have process() report the signal.

        While set, process() reads the last known signal (AT+CSQF) every
        SIGNAL_POLL_MS and calls ``callback(rb, status, bars)``; the first
        poll is on the next process(). Pass None to stop.
        """
        self._require_begun()
        self._signal_handler = callback
        _n.rb960xSetSignalCallback(self._rbp, self._signal_cb if callback else _n.SIGNAL_FN(), None)

    def set_ring_callback(self, callback: Optional[RingHandler]) -> None:
        """rb960xSetRingCallback(): have process() report ring alerts.

        An optional alternative to checking ``ring_alert``. Called as
        ``callback(rb)`` from process() after each SBDRING, unless a session
        has already answered it. The callback may start a session (e.g.
        ``initiate_session_async(..., answer_ring=True)``); the driver never
        answers a ring by itself. Pass None to stop.
        """
        self._require_begun()
        self._ring_handler = callback
        _n.rb960xSetRingCallback(self._rbp, self._ring_cb if callback else _n.RING_FN(), None)

    def process(self) -> None:
        """rb960xProcess(): call regularly from your main loop.

        Handles ring alerts, finishes initiate_session_async() and polls the
        signal; the session, ring and signal callbacks run from inside this
        call. Re-raises anything
        a callback threw.
        """
        self._require_begun()
        self._callback_error = None
        self._handler_error = None
        _n.rb960xProcess(self._rbp)
        err = self._handler_error or self._callback_error
        self._handler_error = None
        self._callback_error = None
        if err is not None:
            raise err

    def read_mt_buffer(self) -> Optional[bytes]:
        """rb960xReadMtBuffer(): the MT message in the modem, or None if empty.

        Never starts a session; messages arrive through initiate_session().
        """
        self._require_begun()
        buf = ctypes.create_string_buffer(MT_MAX)
        length = ctypes.c_size_t(0)
        self._callback_error = None
        status = _n.rb960xReadMtBuffer(self._rbp, buf, len(buf), ctypes.byref(length))
        if self._callback_error is not None:
            err, self._callback_error = self._callback_error, None
            raise err
        if status == Status.ERR_NO_MESSAGE:
            return None
        _check(status, "rb960xReadMtBuffer")
        return buf.raw[: length.value]

    def command(self, cmd: str, timeout_ms: int = 5000) -> str:
        """Send any AT command and return the response text (without "OK").

        For poking at the modem while testing. Raises RuntimeError if the
        modem doesn't reply OK (this goes straight to the AT client, so there
        is no rb960xStatus_t to put in a RockBlock960xError).
        """
        self._require_begun()
        resp = ctypes.create_string_buffer(256)
        self._callback_error = None
        status = _n.rb960xPyCommand(self._rbp, cmd.encode(), resp, len(resp), timeout_ms)
        if self._callback_error is not None:
            err, self._callback_error = self._callback_error, None
            raise err
        if status != 0:
            name = _AT_STATUS[status] if 0 <= status < len(_AT_STATUS) else str(status)
            raise RuntimeError(f"{cmd} failed: {name}")
        return resp.value.decode(errors="replace")


def library_version() -> str:
    """Version of the C library that was loaded."""
    return _n.rb960xPyVersion().decode()
