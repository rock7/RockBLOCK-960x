#!/usr/bin/env python3
"""Send one message without blocking, using the driver's callbacks.

    python3 python/examples/send_receive_async.py -d /dev/ttyUSB0 -m "Hello"
    python3 python/examples/send_receive_async.py -d /dev/ttyUSB0 -b 19200 -D

The main loop only calls rb.process(); everything else happens in three
callbacks that run from inside it:

- on_signal(): process() polls the last known signal (AT+CSQF) every
  SIGNAL_POLL_MS and reports it. Once there's enough signal, it loads the
  MO buffer and starts the session with initiate_session_async().
- on_ring(): process() calls it after an SBDRING ring alert (the gateway
  has an MT message). If the session hasn't started yet, it starts it now,
  answering the ring, instead of waiting for the next signal poll.
- on_session(): process() calls it when the session finishes. It prints
  the result and reads the MT message if the session brought one down.

One attempt, no retry. Uses Iridium credits on a real modem.
"""

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rockblock960x import (  # noqa: E402
    SIGNAL_POLL_MS,
    Model,
    RatMode,
    RockBlock960x,
    RockBlock960xError,
    Status,
)


def debug(direction, data):
    text = "".join(chr(b) if 0x20 <= b < 0x7F else f"\\x{b:02x}" for b in data)
    print(f"{direction} {'>' if direction == 'TX' else '<'} {text}")


class Sender:
    """The state the callbacks share with the main loop."""

    def __init__(self, message: bytes):
        self.message = message
        self.bars = None
        self.started = False
        self.done = False
        self.ok = False

    def on_signal(self, rb, status, bars):
        if status != Status.OK:
            print(f"Signal poll failed: {status!r}")
            return
        if bars != self.bars:
            print(f"Signal: {bars}/5")
            self.bars = bars

        # Polling carries on while we wait, so this runs again on the next
        # poll if there isn't enough signal yet.
        self.start(rb)

    def on_ring(self, rb):
        print("Ring alert: an MT message is waiting at the gateway")
        if not self.started:
            self.start(rb)
        elif self.done:
            # This run's only session has finished; answering this ring
            # would cost another one, so just say so.
            print("(not answered: the session has already run)")
        # Otherwise the running session checks the gateway anyway.

    def start(self, rb):
        """Load the MO buffer and start the session, if there's signal.

        It's safe to call into the driver from a callback.
        """
        if self.started or not rb.session_ready():
            return
        rb.write_mo_buffer(self.message)
        # Answer a ring alert if one arrived (AT+SBDIXA); every session
        # also checks for MT messages either way.
        rb.initiate_session_async(self.on_session, answer_ring=rb.ring_alert)
        self.started = True
        print(f"Session started ({len(self.message)} byte(s))...")

    def on_session(self, rb, status, result):
        self.done = True
        if result is None:
            # The session didn't finish, e.g. ERR_TIMEOUT.
            print(f"Session failed: {status!r}")
            return

        if result.sent:
            self.ok = True
            print(f"Sent, MOMSN {result.momsn}")
        else:
            print(f"Not sent: MO status {result.mo_status}, {result.mo_result.name} ({result.mo_status_text})")

        if result.mt_status == 1:
            print(f"Received MT SBD: {rb.read_mt_buffer()!r}")
        if result.mt_queued > 0:
            print(f"{result.mt_queued} more MT message(s) waiting at the gateway")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-d", "--device", required=True, help="serial device, e.g. /dev/ttyUSB0")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="default 115200; the 9602 and 9603 need 19200")
    parser.add_argument("-m", "--message", default="Hello World!", help="payload to send")
    parser.add_argument("-w", "--wait", type=float, default=120.0, help="seconds to wait for signal before giving up")
    parser.add_argument("-D", "--debug", action="store_true", help="print AT traffic")
    args = parser.parse_args()

    if SIGNAL_POLL_MS == 0:
        print("error: signal polling is off in this build (RB960X_SIGNAL_POLL_MS is 0)", file=sys.stderr)
        sys.exit(1)

    sender = Sender(args.message.encode())

    try:
        with RockBlock960x() as rb:
            rb.begin(args.device, args.baud, debug=debug if args.debug else None)
            print(f"Connected to a RockBLOCK {rb.model.name[2:] if rb.model != Model.UNKNOWN else '(unknown model)'}")

            if rb.model == Model.RB9604 and rb.rat_mode != RatMode.SBD:
                print("Switching the 9604 to SBD mode (the module reboots)...")
                rb.switch_rat(RatMode.SBD)

            # The first poll happens on the next process().
            rb.set_signal_callback(sender.on_signal)
            rb.set_ring_callback(sender.on_ring)
            print(f"Waiting for signal (polled every {SIGNAL_POLL_MS / 1000:.0f} s)...")

            give_up = time.monotonic() + args.wait
            last_tick = time.monotonic()
            while not sender.done:
                rb.process()

                if not sender.started and time.monotonic() > give_up:
                    print("Gave up waiting for signal")
                    break

                # The loop is free for other work between process() calls.
                if time.monotonic() - last_tick >= 5.0:
                    last_tick = time.monotonic()
                    print("  (main loop still running)")
                time.sleep(0.05)

            rb.set_signal_callback(None)
            rb.set_ring_callback(None)
            print(f"Sessions this run: {rb.session_count}")
    except (RockBlock960xError, RuntimeError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)

    sys.exit(0 if sender.ok else 1)


if __name__ == "__main__":
    main()
