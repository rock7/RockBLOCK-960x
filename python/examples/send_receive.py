"""Send one message; print a reply if the session brought one down.

One attempt, no retry. Requires the rockblock960x package to be installed.
"""

from rockblock960x import RatMode, Model, RockBlock960x, RockBlock960xError

rb = RockBlock960x()

try:
    # Defaults to 115200 baud
    rb.begin("/dev/ttyUSB0")
    # Uncomment for 9603 and 9602 and comment out the line above
    #rb.begin("/dev/ttyUSB0", 19200)
    print("Connected")

    if rb.model == Model.RB9604 and rb.rat_mode != RatMode.SBD:
        rb.switch_rat(RatMode.SBD)

    if not rb.session_ready():
        print("Not enough signal to send")
    else:
        rb.write_mo_buffer("Hello World!")
        print("Sending MO SBD...")
        result = rb.initiate_session()
        print("Sent MO SBD" if result.sent else f"Not sent: {result}")

        # Every session also checks the gateway for MT messages.
        if result.mt_status == 1:
            print("Received MT SBD:", rb.read_mt_buffer())
except RockBlock960xError as e:
    print("Failed:", e)
finally:
    rb.end()
