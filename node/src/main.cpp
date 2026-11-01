/* AIO Feeder node — M0 build stub (PLAN.md M0).
 * Real node structure per §7.3: cooperative loop + actuator FSM + relay-off
 * hardware timer. Safety requirements (§3.3) apply before any real actuation. */
#include <Arduino.h>
#include "proto.h"

#ifndef AIO_FIRMWARE_NAME
#define AIO_FIRMWARE_NAME "aio-feeder-node"
#endif

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("%s boot stub — proto v%d\n", AIO_FIRMWARE_NAME, PROTO_VER);
}

void loop() { delay(1000); }
