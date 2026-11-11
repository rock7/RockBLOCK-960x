#!/usr/bin/env python3
"""Try out the Python bindings on a RockBLOCK.

    python3 python/examples/rockblock.py -d /dev/ttyUSB0 info
    python3 python/examples/rockblock.py -d /dev/ttyUSB0 signal --fresh
    python3 python/examples/rockblock.py -d /dev/ttyUSB0 send "Hello" --attempts 3
    python3 python/examples/rockblock.py -d /dev/ttyUSB0 at "AT+CSQF"
    python3 python/examples/rockblock.py -d /dev/ttyUSB0 rat lte
    python3 python/examples/rockblock.py -d /dev/ttyUSB0 -D info

"send" uses Iridium credits on a real modem.
"""

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rockblock960x import Model, RatMode, RockBlock960x, RockBlock960xError  # noqa: E402


def debug(direction, data):
    # Binary payloads (AT+SBDWB) go through here too, so escape anything
    # that isn't printable ASCII.
    text = "".join(chr(b) if 0x20 <= b < 0x7F else f"\\x{b:02x}" for b in data)
    print(f"{direction} {'>' if direction == 'TX' else '<'} {text}")


def cmd_info(rb, _args):
    print(f"Modem:              RockBLOCK {rb.model.name[2:] if rb.model != Model.UNKNOWN else 'unknown'}")
    print(f"Satellite IMEI:     {rb.imei}")
    if rb.model == Model.RB9604:
        print(f"Serial:             {rb.serial}")
        print(f"Cellular IMEI:      {rb.cellular_imei}")
        print(f"RAT:                {rb.rat_mode.name}")
    try:
        print(f"Satellite firmware: {rb.satellite_version()}")
    except RockBlock960xError as e:
        print(f"Satellite firmware: ({e})")
    if rb.model == Model.RB9604:
        try:
            print(f"Cellular firmware:  {rb.cellular_version()}")
        except RockBlock960xError as e:
            print(f"Cellular firmware:  ({e})")


def cmd_signal(rb, args):
    bars = rb.get_signal(fresh=args.fresh)
    print(f"Signal: {bars}/5 ({'ready' if rb.session_ready() else 'not ready'})")


def cmd_send(rb, args):
    if rb.model == Model.RB9604 and rb.rat_mode != RatMode.SBD:
        print("Switching the 9604 to SBD mode...")
        rb.switch_rat(RatMode.SBD)

    # The MO buffer keeps the message until it is replaced or cleared, so
    # it only needs writing once. Retries are the application's job.
    rb.write_mo_buffer(args.message.encode())
    for attempt in range(1, args.attempts + 1):
        result = rb.initiate_session()
        print(f"Attempt {attempt}: {result}")
        if result.sent:
            print(f"Sent, MOMSN {result.momsn}")
            return
        print(f"Not sent: {result.mo_result.name} ({result.mo_status_text})")
        if attempt == args.attempts:
            sys.exit(1)
        time.sleep(args.pause)


RAT_CHOICES = {"lte": RatMode.LTE_CAT_M1, "sbd": RatMode.SBD}


def cmd_rat(rb, args):
    if rb.model != Model.RB9604:
        print("Only the 9604 can switch RAT", file=sys.stderr)
        sys.exit(1)

    if args.mode is not None:
        mode = RAT_CHOICES[args.mode]
        if rb.rat_mode != mode:
            print(f"Switching to {mode.name} (the module resets)...")
        rb.switch_rat(mode)

    print(f"RAT: {rb.rat_mode.name}")


def cmd_at(rb, args):
    print(rb.command(args.command, timeout_ms=args.timeout))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-d", "--device", required=True, help="serial device, e.g. /dev/ttyUSB0")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="default 115200; the 9602 and 9603 need 19200")
    parser.add_argument("-D", "--debug", action="store_true", help="print AT traffic")
    sub = parser.add_subparsers(dest="action", required=True)

    sub.add_parser("info", help="print the modem's identity and firmware").set_defaults(fn=cmd_info)

    p = sub.add_parser("signal", help="check signal / session readiness")
    p.add_argument("--fresh", action="store_true", help="AT+CSQ (new reading) instead of AT+CSQF")
    p.set_defaults(fn=cmd_signal)

    p = sub.add_parser("send", help="send an MO message (uses credits)")
    p.add_argument("message")
    p.add_argument("--attempts", type=int, default=3)
    p.add_argument("--pause", type=float, default=10.0, help="seconds between attempts")
    p.set_defaults(fn=cmd_send)

    p = sub.add_parser("rat", help="show or switch the 9604's RAT mode")
    p.add_argument("mode", nargs="?", choices=sorted(RAT_CHOICES), help="omit to just show the current mode")
    p.set_defaults(fn=cmd_rat)

    p = sub.add_parser("at", help="send a raw AT command")
    p.add_argument("command")
    p.add_argument("--timeout", type=int, default=5000, help="milliseconds")
    p.set_defaults(fn=cmd_at)

    args = parser.parse_args()
    dbg = debug if args.debug else None

    try:
        with RockBlock960x() as rb:
            rb.begin(args.device, args.baud, debug=dbg)
            args.fn(rb, args)
    except (RockBlock960xError, RuntimeError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
