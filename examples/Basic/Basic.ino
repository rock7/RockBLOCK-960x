/*
 * RockBLOCK-960x basic example: identify the modem and print its details.
 *
 * Wiring assumption: RockBLOCK on Serial1 at 115200 baud. The 9602 and
 * 9603 need 19200: change RB960X_BAUD_RATE below.
 * Boards with only one UART (Uno) can use SoftwareSerial instead: any
 * Stream works.
 */
#include <rockblock960x.h>

/* The context lives in static memory: no heap on embedded. */
static rb960x_t rb;

/* 115200 for the 9604. The 9602 and 9603 need 19200. */
#define RB960X_BAUD_RATE 115200

static void debugHandler(atDebugDir_t dir, const char *data, size_t len, void *userData)
{
    (void)userData;
    Serial.print((dir == AT_DEBUG_TX) ? "TX > " : "RX < ");
    Serial.write(reinterpret_cast<const uint8_t *>(data), len);
    Serial.println();
}

void setup()
{
    Serial.begin(115200);
    Serial1.begin(RB960X_BAUD_RATE);

    /* Pass NULL instead of debugHandler to turn the AT trace off. */
    rb960xStatus_t status = rb960xBegin(&rb, Serial1, debugHandler);

    if (status == RB960X_OK)
    {
        Serial.print("Model: ");
        Serial.println(static_cast<int>(rb.model));
        Serial.print("IMEI:  ");
        Serial.println(rb.imei);
    }
    else
    {
        Serial.print("rb960xBegin failed: ");
        Serial.println(static_cast<int>(status));
    }
}

void loop()
{
}
