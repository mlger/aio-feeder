# hal_esp8266

ESP8266 implementation of the node HAL (PLAN.md §4.9): `gpio, hwtimer, wdt,
storage, radio, rtc, ota, adc`. Implements the same interface as
`hal_esp32c3/` so `node_core/` stays portable. Pin map: PLAN.md §4.4 (D23).
