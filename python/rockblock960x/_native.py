"""Load the rockblock960x_python shared library and declare its C types.

Everything ctypes-specific lives here so __init__.py can stay readable.
"""

import ctypes
import ctypes.util
import os
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Finding the library
# ---------------------------------------------------------------------------

_LIB_NAME = "rockblock960x_python"


def _candidate_names():
    if sys.platform == "win32":
        return [f"{_LIB_NAME}.dll", f"lib{_LIB_NAME}.dll"]
    if sys.platform == "darwin":
        return [f"lib{_LIB_NAME}.dylib"]
    return [f"lib{_LIB_NAME}.so"]


def _find_library():
    """Search order:
    1. ROCKBLOCK960X_LIB: an explicit path (best while developing).
    2. Next to this package (for when it's bundled into a wheel).
    3. The repo's build/ directory (python/ is one level below the root),
       and its Release/ and Debug/ folders, where Visual Studio puts it.
    4. The system library path.
    """
    explicit = os.environ.get("ROCKBLOCK960X_LIB")
    if explicit:
        return explicit

    here = Path(__file__).resolve().parent
    build = here.parent.parent / "build"
    for directory in (here, build, build / "Release", build / "Debug"):
        for name in _candidate_names():
            path = directory / name
            if path.is_file():
                return str(path)

    return ctypes.util.find_library(_LIB_NAME)


def _load():
    path = _find_library()
    if path is None:
        raise ImportError(
            "Couldn't find the rockblock960x_python shared library. Build it "
            "from the repo root with:\n"
            "    cmake -B build && cmake --build build\n"
            "or point ROCKBLOCK960X_LIB at the built file."
        )
    return ctypes.CDLL(path)


lib = _load()

# ---------------------------------------------------------------------------
# C types (must match at_client.h / rockblock960x.h exactly)
# ---------------------------------------------------------------------------

WRITE_FN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t)
READ_FN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t)
VOID_FN = ctypes.CFUNCTYPE(None, ctypes.c_void_p)
DEBUG_FN = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p)


class AtPort(ctypes.Structure):
    """atPort_t. Field order matters."""

    _fields_ = [
        ("userData", ctypes.c_void_p),
        ("write", WRITE_FN),
        ("read", READ_FN),
        ("yield_", VOID_FN),  # "yield" is a Python keyword
        ("lock", VOID_FN),
        ("unlock", VOID_FN),
    ]


class SessionResultC(ctypes.Structure):
    """rb960xSessionResult_t. ctypes adds the same padding the C compiler does."""

    _fields_ = [
        ("moStatus", ctypes.c_uint8),
        ("moResult", ctypes.c_int),  # rb960xMoStatus_t
        ("momsn", ctypes.c_uint16),
        ("mtStatus", ctypes.c_uint8),
        ("mtmsn", ctypes.c_uint16),
        ("mtLength", ctypes.c_uint16),
        ("mtQueued", ctypes.c_uint16),
    ]


# rb960xSessionCallback_t(rb, status, const rb960xSessionResult_t *result, userData)
SESSION_FN = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(SessionResultC), ctypes.c_void_p)
# rb960xSignalCallback_t(rb, status, uint8_t bars, userData)
SIGNAL_FN = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_int, ctypes.c_uint8, ctypes.c_void_p)
# rb960xRingCallback_t(rb, userData)
RING_FN = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p)


# ---------------------------------------------------------------------------
# Prototypes. Without argtypes/restype ctypes guesses int for everything,
# which truncates pointers on 64-bit.
# ---------------------------------------------------------------------------

_p = ctypes.c_void_p  # rb960x_t * (opaque)
_status = ctypes.c_int


def _proto(name, restype, *argtypes):
    fn = getattr(lib, name)
    fn.restype = restype
    fn.argtypes = list(argtypes)
    return fn


# Helpers from python/rb960x_python.c
rb960xPySizeof = _proto("rb960xPySizeof", ctypes.c_size_t)
rb960xPyMoMax = _proto("rb960xPyMoMax", ctypes.c_size_t)
rb960xPyMtMax = _proto("rb960xPyMtMax", ctypes.c_size_t)
rb960xPyIccidLen = _proto("rb960xPyIccidLen", ctypes.c_size_t)
rb960xPyVersionLen = _proto("rb960xPyVersionLen", ctypes.c_size_t)
rb960xPySbdixTimeoutMs = _proto("rb960xPySbdixTimeoutMs", ctypes.c_uint32)
rb960xPySignalPollMs = _proto("rb960xPySignalPollMs", ctypes.c_uint32)
rb960xPyVersion = _proto("rb960xPyVersion", ctypes.c_char_p)
rb960xPyModel = _proto("rb960xPyModel", ctypes.c_int, _p)
rb960xPyRatMode = _proto("rb960xPyRatMode", ctypes.c_int, _p)
rb960xPyImei = _proto("rb960xPyImei", ctypes.c_char_p, _p)
rb960xPyCellularImei = _proto("rb960xPyCellularImei", ctypes.c_char_p, _p)
rb960xPySerial = _proto("rb960xPySerial", ctypes.c_char_p, _p)
rb960xPyRingAlert = _proto("rb960xPyRingAlert", ctypes.c_bool, _p)
rb960xPyMtWaiting = _proto("rb960xPyMtWaiting", ctypes.c_bool, _p)
rb960xPyMtQueued = _proto("rb960xPyMtQueued", ctypes.c_uint, _p)
rb960xPySessionPending = _proto("rb960xPySessionPending", ctypes.c_bool, _p)
rb960xPySessionCount = _proto("rb960xPySessionCount", ctypes.c_uint32, _p)
rb960xPyCommand = _proto(
    "rb960xPyCommand", ctypes.c_int, _p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint32
)

# Driver API (rockblock960x.h)
rb960xInit = _proto(
    "rb960xInit", _status, _p, ctypes.POINTER(AtPort), DEBUG_FN, ctypes.c_void_p
)
rb960xSwitchRat = _proto("rb960xSwitchRat", _status, _p, ctypes.c_int)
rb960xGetCellularIccid = _proto("rb960xGetCellularIccid", _status, _p, ctypes.c_char_p, ctypes.c_size_t)
rb960xGetCellularVersion = _proto("rb960xGetCellularVersion", _status, _p, ctypes.c_char_p, ctypes.c_size_t)
rb960xGetSatelliteVersion = _proto("rb960xGetSatelliteVersion", _status, _p, ctypes.c_char_p, ctypes.c_size_t)
rb960xGetSignal = _proto("rb960xGetSignal", _status, _p, ctypes.c_bool, ctypes.POINTER(ctypes.c_uint8))
rb960xSessionReady = _proto("rb960xSessionReady", _status, _p)
rb960xWriteMoBuffer = _proto("rb960xWriteMoBuffer", _status, _p, ctypes.c_char_p, ctypes.c_size_t)
rb960xClearBuffers = _proto("rb960xClearBuffers", _status, _p, ctypes.c_int)
rb960xInitiateSession = _proto(
    "rb960xInitiateSession", _status, _p, ctypes.c_bool, ctypes.c_uint32, ctypes.POINTER(SessionResultC)
)
rb960xInitiateSessionAsync = _proto(
    "rb960xInitiateSessionAsync", _status, _p, ctypes.c_bool, ctypes.c_uint32, SESSION_FN, ctypes.c_void_p
)
rb960xSetSignalCallback = _proto("rb960xSetSignalCallback", None, _p, SIGNAL_FN, ctypes.c_void_p)
rb960xSetRingCallback = _proto("rb960xSetRingCallback", None, _p, RING_FN, ctypes.c_void_p)
rb960xProcess = _proto("rb960xProcess", None, _p)
rb960xReadMtBuffer = _proto(
    "rb960xReadMtBuffer", _status, _p, ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)
)
rb960xMoStatusText = _proto("rb960xMoStatusText", ctypes.c_char_p, ctypes.c_int)
rb960xAtProbe = _proto("rb960xAtProbe", _status, _p, ctypes.c_uint32)
