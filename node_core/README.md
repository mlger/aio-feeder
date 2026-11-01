# node_core

Portable node logic, host-tested (PLAN.md §13): limits, actuator FSM, profile,
time validity, protocol glue. No MCU headers — everything hardware-specific
goes behind the HAL in `node/hal_esp8266/` and `node/hal_esp32c3/`.
