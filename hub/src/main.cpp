/* AIO Feeder hub — M0 build stub (PLAN.md M0).
 * Real firmware structure per §7.2 (ui/radio/sched/net/storage/feed_manager tasks). */
#include <Arduino.h>

#ifndef AIO_FIRMWARE_NAME
#define AIO_FIRMWARE_NAME "aio-feeder-hub"
#endif

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("%s boot stub — proto v%d\n", AIO_FIRMWARE_NAME, PROTO_VER);
}

void loop() { delay(1000); }
