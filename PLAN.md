# AIO Feeder — Engineering Plan

**Project:** All-in-One (AIO) Feeder — a multi-node automated feeding system
**Hub:** Waveshare ESP32-S3-Touch-LCD-4.3B
**Nodes:** ESP8266 **or** ESP32-C3 SuperMini + DS3231 RTC + 5 V 1-ch relay + 12 V push-pull solenoid (one per feeder). Both MCUs are supported from one codebase via a thin HAL (see §4.8 and §7.1).
**Document status:** v0.7 — 2026-11-01 (hub display prototype changes synced: timed gate calibration, custom storage capacity, setup/pairing capacity entry; hub facts verified against Waveshare docs; QEMU emulation and LVGL docs MCP added to the toolchain)
**Owner:** Mark

> Items marked **[VERIFY]** are facts about parts/SDKs that could not be confirmed. Check them against the datasheet / schematic / SDK docs before committing the design.

---

## 1. Goals and non-goals

### 1.1 Goals
1. Feed multiple, *different* kinds of things (dry pellets/kibble, grain, possibly liquids) from one central controller.
2. One touchscreen hub that owns schedules, calibration, logging and alerts.
3. Cheap, identical, replaceable nodes. Adding a feeder = flash node, power it, pair it.
4. **Fail-safe:** no failure mode (crash, brownout, radio loss, bad config) may cause continuous or repeated dispensing.
5. Works with no internet and no router for the core job (scheduled feeding).
6. Remote visibility/control (web dashboard + MQTT / Home Assistant) as an add-on, not a dependency.

### 1.2 Non-goals (v1)
- Cloud backend / accounts.
- Camera / animal recognition.
- Per-animal identification (RFID) — noted as a future extension.
- Precise gram-accurate dosing without a load cell (v1 is timed/pulse-count with calibration).

---

## 2. System architecture

```
                        ┌───────────────────────────────┐
   Phone / Browser ───► │  HUB  ESP32-S3 (4.3" touch)   │ ◄─── NTP (when Wi-Fi up)
   MQTT / Home Asst.    │  LVGL UI · scheduler · log    │
                        │  web server · MQTT client     │
                        └──────────────┬────────────────┘
                                       │  ESP-NOW (2.4 GHz, same channel as hub Wi-Fi)
              ┌────────────────┬───────┴────────┬────────────────┐
              ▼                ▼                ▼                ▼
        ┌──────────┐     ┌──────────┐     ┌──────────┐     ┌──────────┐
        │ NODE 1   │     │ NODE 2   │     │ NODE 3   │ ... │ NODE N   │
        │ 8266/C3  │     │ 8266/C3  │     │ 8266/C3  │     │ 8266/C3  │
        │ + RTC    │     │ + RTC    │     │ + RTC    │     │ + RTC    │
        │ relay →  │     │ relay →  │     │ relay →  │     │ relay →  │
        │ solenoid │     │ solenoid │     │ pump?    │     │ ...      │
        └──────────┘     └──────────┘     └──────────┘     └──────────┘
```

### 2.1 Responsibility split

| Concern | Hub | Node |
|---|---|---|
| Wall-clock time | NTP + onboard PCF85063 RTC (CR927 backup cell) | Synced from hub; DS3231 RTC keeps time through power loss |
| Schedules | Source of truth, UI editing | Holds a *copy* for offline fallback |
| Profiles / calibration | Source of truth | Holds a *copy* incl. safety limits |
| Actuation | Requests only | Executes; **enforces limits locally** |
| Logging | Persistent history | Last-N result ring buffer |
| Safety limits | Distributes | **Final authority** |

Design rule: **the hub proposes, the node disposes.** A node must be safe even if the hub is compromised, crashed, or sending garbage.

---

## 3. Requirements

### 3.1 Functional
- **FR1:** Create/edit/delete per-node schedules (time, days of week, amount).
- **FR2:** Manual "feed now" from touchscreen and web/MQTT.
- **FR3:** Per-node profile (mechanism, unit, calibration, limits, storage capacity).
- **FR4:** Calibration wizard (dispense → weigh → enter → compute rate).
- **FR5:** Pairing UI; no hard-coded MAC addresses.
- **FR6:** Feed history with per-event status; CSV export.
- **FR7:** Alerts: node offline, jam/stuck gate, hopper low, missed feed, supply voltage low.
- **FR8:** OTA firmware update for hub and nodes.
- **FR9:** Per-node missed-feed policy (skip, or catch up once within a window), selectable on the hub (§9.4).
- **FR10:** Optional PIN lock with a stated scope: settings only, or settings and manual feeds (§8).
- **FR11:** Configurable history retention, and a visible message when the SD card is full or missing (§7.5).
- **FR12:** Stock-level **estimate** per node, shown as capacity left in g or ml out of the node's storage capacity, calculated from the amounts actually dispensed. Includes a refill log and a "lasts about N days" prediction. Always labelled as an estimate (§9.7).
- **FR13:** Feed preview before a manual feed: amount, cooldown, remaining hourly and daily cap, or the reason it would be refused (§9.8).
- **FR14:** "Skip next" on a single schedule entry, recorded in history, with undo (§9.9).
- **FR15:** Hold mode: skip all scheduled feeds until a chosen time. Manual feeds still work (§9.9).
- **FR16:** Double-feed check: a manual feed within 60 min of a successful feed asks for confirmation (§9.10).
- **FR17:** History summary: totals per node for today and 7 days, and a 7-day bar chart with a text alternative.
- **FR18:** Free-text note per node (up to 160 characters).
- **FR19:** Settings backup to SD, restore, and download of the backup and of the history as CSV (§9.12).
- **FR20:** Short notification messages over MQTT for offline, fault, missed feed and low estimate (§9.11).
- **FR21:** First-run setup wizard: time zone, Wi-Fi, first node, first schedule (§9.13).
- **FR22:** Every node carries a DS3231 RTC so it keeps wall-clock time through its own power loss and can run its fallback schedule right after a reboot. The hub shows each node's time source and RTC health (§7.4).
- **FR23:** **[NEW in v0.7]** Storage capacity is user-entered: "Set size and level" on the node page, and the pairing dialog and setup wizard ask for it. The user enters the full size and the amount in it now (§9.7).
- **FR24:** **[NEW in v0.7]** A solenoid-gate node can be calibrated by open time (g/s or ml/s) as well as by pulse count; the wizard guards against test runs that are too small to weigh (§9.3).

### 3.2 Non-functional
- **NFR1:** Feed command → relay actuation latency < 500 ms typical (excluding retries).
- **NFR2:** Node heartbeat every 30 s; offline declared after 3 missed (≈ 90 s).
- **NFR3:** Survive power cycling at any moment without corrupting config/log.
- **NFR4:** Unattended operation ≥ 30 days without reboot (soak target).
- **NFR5:** Support ≥ 8 nodes in v1; architecture does not preclude 20 **[VERIFY ESP-NOW peer limits]**.

### 3.3 Safety requirements (highest priority)
- **SR1:** Relay output is **OFF** at power-up, reset, watchdog reset, and during OTA.
- **SR2:** Every actuation has a hard maximum on-time enforced by a node-local timer, independent of command content.
- **SR3:** Per-hour and per-day dispensed-quantity caps, enforced on the node, persisted across reboots.
- **SR4:** Minimum cooldown between actuations (solenoid thermal protection).
- **SR5:** Mechanical design is **normally closed**: de-energised solenoid = gate shut (spring return).
- **SR6:** A command that is not acknowledged is **never silently retried into a double feed** (idempotency via feed IDs). The node persists the `feed_id` *before* the relay turns on, so a power loss mid-feed cannot lead to a second feed when the hub retries after reboot.
- **SR7:** No schedule executes on a node until it has valid time, and missed feeds are never caught up on reconnect without explicit policy (§9.4). Time is valid when:
  - the DS3231 is healthy (oscillator-stop flag clear, value range-checked) and the last hub sync is no older than `rtc_trust_days` (default 30, confirm in M1); or
  - without a healthy RTC, a `TIME_SYNC` was received within the last 24 h (confirm in M1).

  Otherwise the node sets `time_invalid` and refuses scheduled feeds; manual feeds stay allowed. A node must never accept the RTC as valid right after a backup-cell loss.
- **SR8:** A replaced, reset or newly paired node starts in a "limits unconfirmed" state: it refuses scheduled feeds until the hub has pushed the profile and the persisted hour/day counters (or confirmed zero). Zeroed counters on a swapped node must never silently reset the caps.
- **SR9:** Every node has a physical way to stop actuation without the hub (a stop button or a power switch on the actuator supply). The hub learns about it through the next heartbeat flag (`manual_stop`) and shows an alert.
- **SR10:** Hub-side cancellations (skip next, hold mode) reach the nodes as part of the pushed schedule (`skip_until`, `hold_until`), so a node running its fallback schedule does not feed something the user cancelled. Until a node has confirmed the push, the hub shows that an offline node may still run its saved schedule. Skipped feeds are always logged, never silent.

---

## 4. Hardware design

### 4.1 Hub — Waveshare ESP32-S3-Touch-LCD-4.3B

Facts confirmed from the Waveshare product docs (docs.waveshare.com/ESP32-S3-Touch-LCD-4.3B):

| Item | Detail |
|---|---|
| Module | ESP32-S3-WROOM-1-**N16R8**: dual-core LX7 up to 240 MHz, 512 KB SRAM, **16 MB flash, 8 MB octal PSRAM** (room for LVGL double buffers in PSRAM) |
| Radio | 2.4 GHz Wi-Fi b/g/n + BLE 5, onboard antenna |
| Display | 4.3" IPS, 800×480, 65K colours, **RGB parallel** |
| Touch | 5-point capacitive, I²C on **GPIO8 (SDA) / GPIO9 (SCL)**, IRQ on **GPIO4**, reset via expander. Touch IC part not stated **[VERIFY]** (GT911 is likely) |
| IO expander | **CH422G** on the same I²C bus: EXIO1 = touch reset, EXIO2 = backlight enable, EXIO4 = SD CS (active low), EXIO0/EXIO5 = isolated DI0/DI1, OD0/OD1 = isolated DO0/DO1 |
| microSD | SPI: MOSI GPIO11, SCK GPIO12, MISO GPIO13, CS via CH422G EXIO4 |
| RTC | **Onboard PCF85063** on I²C GPIO8/9, **CR927** backup-cell holder |
| RS485 | RX GPIO43, TX GPIO44, auto direction, switchable termination (off by default) |
| CAN | TX GPIO15, RX GPIO16, CAN 2.0, switchable termination (off by default) |
| I²C header | GPIO8/9, VOUT selectable 5 V or 3.3 V by resistor |
| USB | Native USB-C, D− GPIO19 / D+ GPIO20 |
| Isolated I/O | 2 × DI (5–36 V, NPN or PNP, dry or wet contact); 2 × DO (5–36 V open-drain, up to 450 mA each) |
| Power | USB-C 5 V **or DC 7–36 V** terminal; about 5 V / 450 mA. MX1.25 connector for a 3.7 V Li-ion/LiPo cell with onboard charger (580 mA) and on/off switch |

**GPIO ownership.** The RGB bus uses GPIO0, 1, 2, 3, 5, 7, 10, 14, 17, 18, 21, 38–42 and 45–48. Add touch/I²C (4, 8, 9), SD (11–13), CAN (15, 16), USB (19, 20) and RS485 (43, 44); GPIO26–37 belong to flash and octal PSRAM. That leaves **GPIO6** as the only pin not listed. The page has no header pinout, so check the schematic before relying on it **[VERIFY]**. In practice, extra hub I/O goes on the I²C header or through the isolated DI/DO.

**Shared I²C bus.** Touch, the CH422G and the RTC all sit on GPIO8/9. A stuck bus loses touch, backlight control, SD chip select and the clock together, so the hub needs bus-recovery code (clock out 9 pulses, re-init) and a watchdog on I²C transactions.

**RS485 and the console.** GPIO43/44 are the S3's default UART0 console pins. If RS485 is ever used, move the log console to the USB-Serial-JTAG port.

Hub plan:
- Power from the same 12 V rail as the nodes through the 7–36 V terminal (confirmed in range).
- Optional: fit a 3.7 V LiPo so the hub rides through short mains cuts and keeps logging and showing alerts. Nodes still need their own supply to feed.
- Use the SD card for feed log + config backup, with LittleFS/NVS on internal flash for live config.
- Use the onboard PCF85063 as the hub RTC. No external DS3231 is needed on the hub.
- RS485/CAN are **not used in v1** but reserved for a future wired-node option (§16).
- The isolated DOs can optionally drive a local buzzer or alarm lamp for FAULT and offline alerts. The DIs can optionally read a mains-fail or door/lid contact. Neither is required for v1.

### 4.2 Feeder node

**Core parts**

| Part | Notes |
|---|---|
| ESP8266 board (Wemos D1 mini or similar) **or** ESP32-C3 SuperMini | Both ESP-NOW capable; see §4.8 for the trade-offs |
| DS3231 RTC module (I²C) with backup cell | Keeps time across node power loss. Many boards charge a CR2032, which is unsafe — use the matching rechargeable cell or remove the charge path **[VERIFY]**. Typical drift ±2 ppm, a few seconds per month **[VERIFY]** |
| 5 V 1-ch relay module | Check it triggers reliably from 3.3 V logic; note active-high vs active-low |
| 12 V push-pull (pull-type) solenoid | Get its coil resistance/current and rated duty cycle |
| Flyback diode (1N4007 or 1N5819/SS14) | **Across the solenoid coil** (cathode to +12 V) |
| 12 V DC supply | Size for solenoid inrush + margin (§4.3) |
| 12 V → 5 V buck (≥ 1 A) | Powers the MCU + relay coil |
| Bulk caps | 470–1000 µF on the 5 V rail near the MCU; 100 nF local decoupling |
| Fuse (polyfuse or blade) | On the 12 V input per node |
| TVS diode (SMBJ15A-class) across 12 V input | Absorbs transients |
| Stop switch | Series switch in the solenoid/relay supply, or a momentary button read by the MCU that latches `manual_stop` (SR9) |

**Optional (v1.x)**
- Reed switch / hall sensor on gate → "gate actually moved" confirmation.
- IR / capacitive hopper-low sensor.
- Input-voltage monitor: divider into ADC → detect sagging supply.
- Pairing button (momentary, to GND with pull-up).

### 4.3 Power budget method
1. Measure solenoid DC resistance → I = 12 V / R. Also measure inrush with a scope or current probe if possible.
2. Node steady load: MCU ≈ 70–170 mA (Wi-Fi/ESP-NOW peaks), relay coil ≈ 70–90 mA at 5 V.
3. Supply capacity ≥ 1.5 × (solenoid current + 5 V rail load reflected to 12 V).
4. Only one solenoid should be energised per supply at a time unless the supply is sized for all of them (enforced via hub-side feed staggering; §9.5).

### 4.4 ESP8266 pin map (proposal)

| Function | Pin | GPIO | Why |
|---|---|---|---|
| Relay IN | D5 | GPIO14 | Boot-safe |
| I²C SDA / SCL (DS3231) | D2 / D1 | GPIO4 / GPIO5 | Default I²C pins, boot-safe; add 4.7 kΩ pull-ups if the module has none |
| Gate reed (opt.) | D7 | GPIO13 | Input with pull-up |
| Hopper-low (opt.) | D0 | GPIO16 | Polled input with an external pull-up (GPIO16 has no internal pull-up) **[VERIFY]** |
| Pairing button | D6 | GPIO12 | Boot-safe, pull-up |
| Status LED | D4 | GPIO2 | Onboard LED, active-low; boot pin is fine for an LED |
| Supply monitor | A0 | ADC | Bare ESP8266 ADC is 0–1 V; D1 mini has an on-board divider **[VERIFY]** |

Avoid driving the relay from GPIO0/2/15 (boot-strap pins) — a floating/low/high state during boot can click or latch the relay.

### 4.5 ESP32-C3 SuperMini pin map (proposal) **[VERIFY against your board's silkscreen]**

The SuperMini exposes roughly GPIO0–10 plus GPIO20/21 (UART0). Strapping pins are GPIO2, GPIO8 and GPIO9; GPIO9 is the BOOT button and GPIO8 usually drives the on-board LED. USB (GPIO18/19) is native but not broken out as free I/O.

| Function | GPIO | Why |
|---|---|---|
| Relay IN | GPIO3 | Not a strapping pin; used as plain output; high-Z at reset |
| Gate reed (opt.) | GPIO4 | Input with pull-up |
| Hopper-low (opt.) | GPIO5 | Input with pull-up (ADC2 pin — don't use as analog while Wi-Fi is on) |
| Supply monitor | GPIO1 | ADC1 channel; needs a divider (12 V → ≤ ~2.5 V; 1 % resistors + 100 nF) |
| I²C SDA / SCL (DS3231) | GPIO6 / GPIO7 | Free, no boot constraints |
| Status LED | GPIO8 (on-board) | Strapping pin, but an LED load is fine; keep external circuitry from pulling it low at boot |
| Pairing button | GPIO9 (on-board BOOT) | Fine as an input **after** boot; holding it during reset enters download mode — document this |
| Debug UART | GPIO20/21 | Optional; native USB serial/JTAG also available |

**Default-OFF warning (both MCUs):** during reset and early boot the GPIOs can be high-impedance or toggle. An **active-low** relay module can then energise. Add an external resistor so the relay is OFF whenever the MCU doesn't drive it: a pull-up to the relay's VCC for active-low modules, a pull-down to GND for active-high modules (~10 kΩ; check the module's own input network first so you don't fight it). Verify on a scope across power-on, reset and watchdog reset (SR1).

### 4.6 Relay and solenoid wiring notes
- Isolate: relay module opto-isolation uses a separate JD-VCC where available; if jumpered to share 5 V, coil noise couples into the MCU supply → rely on the buck's capacity and bulk caps.
- Solenoid on the **NO** contact → de-energised = off. Never use NC for the actuator.
- Add an RC snubber across the relay contacts if you still see resets/arcing on the bench scope.
- Keep solenoid wiring physically away from the antenna/MCU and twist the pair.
- Consider later swapping to a **logic-level MOSFET** (IRLZ44N class or an AO3400 module) + flyback diode: silent, no contact wear, allows PWM "pull-in then hold" to cut heat. Relay is acceptable for v1.

### 4.7 Mechanical notes (solenoid gate)
- Spring-return, normally-closed.
- Prefer a **metering chamber** (fixed-volume pocket released per pulse) over timed open. Portion = N pulses; far less sensitive to hopper level, food bridging and flow-rate drift.
- Add a deflector/anti-bridging feature for pellets and grain; test each food type for jamming.
- Use food-safe, washable materials for food-contact parts; keep electronics out of the splash zone.

### 4.8 Hub BOM (indicative)
- Waveshare ESP32-S3-Touch-LCD-4.3B
- 12 V supply (7–36 V input) with margin, or USB-C 5 V
- CR927 cell for the onboard PCF85063 RTC
- microSD card (industrial/endurance rated preferred)
- Optional 3.7 V single-cell LiPo (MX1.25) for hub ride-through
- Enclosure

### 4.9 Node MCU choice: ESP8266 vs ESP32-C3 SuperMini

| Aspect | ESP8266 (D1 mini class) | ESP32-C3 SuperMini |
|---|---|---|
| CPU / RAM | 80 MHz Xtensa, ~80 KB usable RAM | 160 MHz RISC-V, ~400 KB SRAM |
| Flash | 1–4 MB depending on board | Typically 4 MB (dual-slot OTA fits comfortably) |
| Wi-Fi / BLE | 802.11 b/g/n, no BLE | 802.11 b/g/n + BLE 5 (BLE can do provisioning) |
| ESP-NOW | Yes (older API) | Yes (current ESP-IDF API) |
| Free GPIO | ~9 usable, several boot-strapped | ~11 usable, 3 strapping pins |
| USB | External USB-UART chip | Native USB serial/JTAG (flash/debug without a bridge) |
| Crypto / security | Software only | HW AES/SHA/HMAC, secure boot/flash encryption available |
| Watchdogs | Software + hardware WDT | Task WDT, interrupt WDT, RTC WDT, plus HW timers (gptimer) |
| SDK status | Legacy; slow-moving | Actively developed (ESP-IDF / Arduino-ESP32) |
| Cost | Lowest | Similar, sometimes cheaper |
| Known caveats | Boot-strap pins glitch relays; low RAM for TLS | Some SuperMini clones have weak RF/antenna layout; ADC2 unusable with Wi-Fi |

**Recommendation:**
- **New builds → ESP32-C3 SuperMini.** More RAM, hardware timers and crypto, native USB, and a supported toolchain. It also matches the hub's ESP32 family, so ESP-NOW behaviour and the Arduino/IDF APIs are closer.
- **Existing ESP8266 boards → keep using them.** Nothing in the design requires the C3. Mixed fleets are supported.

**SuperMini RF caveat [VERIFY on your boards]:** several clone batches have poor Wi-Fi/ESP-NOW range or unstable association because of antenna layout. Commonly reported mitigations: lower TX power (e.g. ~8.5 dBm via `WiFi.setTxPower`), keep metal and relay/solenoid wiring away from the antenna corner, and don't power from a noisy rail. Run the range/packet-loss test (§11.2) on real SuperMinis in M1, before standardising on them.

**ESP-NOW interoperability:** ESP8266 and ESP32-family boards can talk ESP-NOW to each other. Differences to handle in the HAL:
- Callback signatures and init calls differ per SDK/Arduino core version.
- Peer-count and **encrypted-peer** limits differ per chip/SDK **[VERIFY]**.
- Both must sit on the same Wi-Fi channel as the hub (§6.2). The channel-scan logic is the same on both.

**Hardware abstraction (HAL) so one node codebase serves both:**

```
hal_gpio_*        // relay out, inputs with pull-ups
hal_hwtimer_*     // one-shot "relay-off deadline" timer (ESP8266 timer1/os_timer; C3 gptimer/esp_timer in IRAM-safe ISR)
hal_wdt_*         // enable/feed/reset-reason
hal_storage_*     // flash KV + A/B config slots + counters (LittleFS/EEPROM on 8266, NVS on C3)
hal_radio_*       // init, set channel, add peer, send, rx callback
hal_rtc_*         // DS3231 read/write, oscillator-stop flag
hal_ota_*         // update flow, hash verify, rollback where supported
hal_adc_*         // vin monitor (A0 on 8266, ADC1 on C3)
```

Business logic (limits, actuator state machine, profile handling, protocol, time validity) stays in portable C/C++ with no SDK includes and is host-tested.

**C3-specific safety implementation notes:**
- Relay-off deadline: arm a **gptimer** one-shot alarm whose ISR (in IRAM) drives the pin OFF directly, so it still fires if a task blocks. `esp_timer` callbacks run in task context and are acceptable only as a second layer.
- Enable the task WDT and interrupt WDT; check `esp_reset_reason()` at boot and report it in the heartbeat. Treat WDT/brown-out resets as "possible fault" (set a flag, require hub acknowledgement before auto-resuming schedules).
- Keep the brown-out detector enabled.
- OTA uses the standard dual-slot partition table with rollback (`esp_ota_mark_app_valid_cancel_rollback` only after a successful hub handshake).

---

## 5. Node types and profiles

A node runs one firmware image. Behaviour differences come from a **profile** stored on the hub and mirrored to the node.

```c
typedef enum { MECH_SOLENOID_GATE, MECH_MOTOR_ONEWAY, MECH_PUMP } mech_t;
typedef enum { UNIT_PULSES, UNIT_GRAMS, UNIT_ML } unit_t;

typedef struct {
  uint8_t   version;
  char      name[24];
  mech_t    mech;
  unit_t    unit;
  uint16_t  cal_ms_per_unit_x100;  // open-time per unit (fixed-point), or per pulse
  uint16_t  pulse_on_ms;           // for pulse-count mode
  uint16_t  pulse_off_ms;
  uint16_t  max_on_ms_single;      // hard cap per actuation
  uint16_t  cooldown_ms;           // min gap between actuations
  uint32_t  max_units_per_hour;
  uint32_t  max_units_per_day;
  uint8_t   flags;                 // e.g. has_gate_sensor, has_hopper_sensor
  uint8_t   missed_policy;         // 0 = skip, 1 = catch up once within window
  uint8_t   catchup_window_min;    // default 30
  uint32_t  storage_capacity;      // full hopper/reservoir, in g or ml (matches `unit`)
  uint16_t  crc16;
} node_profile_t;
```

**[NEW in v0.7] Gate calibration mode.** The hub prototype calibrates a solenoid-gate node by **open time** (grams per second), the same way as an auger or pump, and shows amounts in grams. In that mode `cal_ms_per_unit_x100` is the open time per gram, `max_on_ms_single` is raised from the 2.5–3 s of the pulse design to 10 s (**[VERIFY]** against the real solenoid duty rating and SR2/SR4), and the node still enforces it. Pulse-count mode with a metering chamber remains the preferred mechanical design (§4.7, D7) because flow rate drifts with hopper level.

Supported mechanisms (same hardware, different profile):
- **Solenoid gate** — dry pellets/kibble (current hardware).
- **One-way motor/auger/vibrator via relay** — grain/powders; no reverse. (Anti-jam reverse needs an H-bridge variant → separate "motor node" design, out of v1 scope.)
- **Small pump via relay** — liquids; flow rate is steady, so timed dosing is accurate.

---

## 6. Communication design

### 6.1 Why ESP-NOW
- Works without a router, low latency, tiny frames, supported on both ESP32 and ESP8266.
- The hub still joins Wi-Fi (STA) for NTP/web/MQTT, which **forces the hub's radio onto the router's channel**. ESP-NOW peers must be on that same channel.

### 6.2 Channel strategy
- Hub and nodes all operate on the *router's* channel.
- Nodes that lose the hub for > T seconds run a **channel scan**: hop channels 1–13 sending `HELLO`, listen for `HELLO_ACK`, lock the channel that answers, persist it.
- When Wi-Fi is down, the hub sticks to the last channel (does not hop).
- If channel instability becomes a problem in the field: pin the router to a fixed channel, or move nodes to Wi-Fi + MQTT (§6.8).

### 6.3 Pairing
1. Hub UI → "Add node" opens a 60 s pairing window.
2. Node (button held at boot, or unpaired state) broadcasts `PAIR_REQ` (MAC, fw version, capabilities).
3. Hub shows the node; the user taps to accept, names it, assigns a profile and confirms its storage capacity.
4. Before accepting, the hub shows a short confirmation code (e.g. 4 digits derived from a nonce both sides contribute). The node shows the same code via its status-LED pattern or serial output, and the user confirms they match. This prevents pairing the wrong node and limits what a passive listener can learn.
5. Hub replies `PAIR_ACCEPT` with a node ID, channel, and a per-node key; the node stores them in flash. The key is wrapped using a secret derived from the confirmation exchange, so it is never sent in the clear. If this proves impractical on ESP8266, document the residual risk in the decision log.
6. Unpair = hub deletes + node factory-reset command. If the node is offline, the hub keeps it in a "pending unpair" state, retries on every heartbeat window, and shows it in the node list. Until the node confirms, the hub treats it as paired but disabled (no feeds, no schedule pushes).

### 6.4 Security
- ESP-NOW supports a PMK/LMK-based encryption mode; peer-count limits for *encrypted* peers are lower than for unencrypted ones **[VERIFY on both SDKs]**.
- **v1 default:** unicast to known MACs only, a truncated HMAC in every frame, and sequence numbers to defeat replay.
- Native encryption is used only if the verified encrypted-peer limit on both SDKs covers the target node count (8, with room for 20). Otherwise the fleet stays on HMAC and the limit is recorded in the decision log.
- The node-side safety caps (SR2–SR4) remain the real protection against a hostile or buggy command.

### 6.5 Frame format

```c
#define PROTO_VER 1

typedef enum {
  MSG_HELLO, MSG_HELLO_ACK, MSG_PAIR_REQ, MSG_PAIR_ACCEPT,
  MSG_FEED, MSG_FEED_ACK, MSG_FEED_RESULT,
  MSG_CONFIG, MSG_CONFIG_ACK,
  MSG_SCHEDULE_SET, MSG_SCHEDULE_ACK,
  MSG_TIME_SYNC, MSG_STATUS_REQ, MSG_HEARTBEAT,
  MSG_ABORT, MSG_TARE_OR_CAL,
} msg_type_t;

typedef struct __attribute__((packed)) {
  uint8_t  ver;
  uint8_t  type;
  uint8_t  node_id;
  uint8_t  flags;
  uint16_t seq;          // per-sender, monotonic
  uint16_t len;          // payload bytes
  uint8_t  payload[ESPNOW_MAX_PAYLOAD - 8 - 8];
  uint8_t  mac[8];       // truncated HMAC over header+payload
} frame_t;
```

Key payloads:
- `FEED { feed_id (u32), amount (u16), unit (u8), max_ms_override_forbidden }`
- `FEED_ACK { feed_id, accepted (bool), reason }`
- `FEED_RESULT { feed_id, status, actual_on_ms, pulses, gate_ok (opt), t_unix }`
- `SCHEDULE_SET { version, entries[], skip_until, hold_until }`
- `HEARTBEAT { uptime, rssi, vin_mv, hopper_low, last_feed_id, day_units, flags, rtc_drift_ms }`
  - `flags`: `manual_stop`, `time_invalid`, `limits_unconfirmed`, `rtc_ok`, `rtc_lost` (oscillator-stop flag set)
  - `rtc_drift_ms`: last measured RTC offset against `TIME_SYNC`
- `TIME_SYNC { unix_time, tz_rule }` (time zone and daylight-saving rule)

### 6.6 Reliability and idempotency
- Each `FEED` carries a unique `feed_id` (hub-generated, persisted counter).
- The node keeps the last K completed `feed_id`s in flash/RAM; a duplicate `FEED` returns the stored `RESULT` and **does not actuate again**.
- The `feed_id` is written to flash *before* the relay turns on (SR6), and the record is marked complete afterwards. On boot, an "in progress" record means the feed is treated as already attempted and is never repeated automatically.
- Hub retry policy: resend `FEED` up to 3× at 300 ms / 1 s / 3 s if no `FEED_ACK`. After that, mark `UNKNOWN` and **alert**; never blindly re-issue as a new feed.
- The node always answers `FEED_RESULT` and may repeat it until it gets the hub's ack.

### 6.7 Remote interfaces
- **Web dashboard** (hub-served): node status, manual feed, history, backup and CSV download.
- **MQTT topics**

```
aio/hub/status                  (LWT: online/offline)
aio/hub/notify                  {"text":"…"}  notification messages (§9.11)
aio/node/<id>/state             (retained)  JSON: online, hopper, vin, last_feed...
aio/node/<id>/cmd/feed          {"amount":2,"unit":"pulses"}
aio/node/<id>/event/feed_result
aio/node/<id>/config            (retained)  profile summary
```

- Home Assistant MQTT discovery for each node (button, sensors, binary sensors).
- Authentication: MQTT user/password; TLS if the broker supports it and flash/RAM allow.
- The hub's **Remote access** screen (Settings) covers the dashboard switch, port, login requirement, connected browsers, and MQTT broker, credentials, TLS, discovery, status, topic list and live log.

### 6.8 Fallback if ESP-NOW proves flaky
Move nodes to normal Wi-Fi STA + MQTT (nodes subscribe to command topics; the hub also acts as a client). Same message semantics, different transport. Keep the protocol layer behind a `transport` interface so this swap is cheap.

---

## 7. Firmware architecture

### 7.1 Toolchain
- **PlatformIO** monorepo with three environments — `hub_esp32s3`, `node_esp8266`, `node_esp32c3` — plus a shared `lib/proto`.
- Hub: ESP-IDF via Arduino-compat or pure ESP-IDF **[decide at M0]**; LVGL 8.x or 9.x with Waveshare's RGB panel driver. Waveshare provides demos for both Arduino and ESP-IDF.
- **Display HAL on the hub:** the UI talks to `esp_lcd` through a thin display layer. On hardware it uses the RGB panel driver plus the CH422G backlight. Under emulation it uses `espressif/esp_lcd_qemu_rgb` (§7.7).
- **Emulation:** Espressif's QEMU fork runs both the hub (ESP32-S3) and the C3 node through `idf.py qemu` (§7.7). The ESP8266 is not emulated.
- **LVGL docs MCP:** the official LVGL MCP server (`https://lvgl.mcp.kapa.ai/`, run by kapa.ai) is part of the dev setup. It is read-only: it answers from LVGL's docs, forum and source with links, so AI-assisted UI work uses current APIs rather than guessing. Configure it in `.claude/.mcp.json` (§13).
- Node: `node_esp8266` (Arduino core for ESP8266) and `node_esp32c3` (Arduino-ESP32 first; move to ESP-IDF for tighter timer/WDT/OTA control). Both link the same portable `node_core` library through the HAL (§4.9).
- Shared C headers for `msg_*`, profile and status codes — one source of truth, unit-tested on host.
- CI: build all targets, run host unit tests.

### 7.2 Hub software structure (FreeRTOS)

| Task | Priority | Responsibility |
|---|---|---|
| `ui_task` | mid | LVGL tick/handler, input |
| `radio_task` | high | ESP-NOW rx/tx, retry state machines |
| `sched_task` | mid | Evaluate schedules each second, issue FEED requests |
| `net_task` | low | Wi-Fi, NTP, web server, MQTT |
| `storage_task` | low | Log writes, config persistence (debounced) |
| `watchdog` | — | Task WDT + health supervisor |

Modules: `node_registry`, `profile_store`, `schedule_engine`, `feed_manager` (the single owner of in-flight feeds), `stock_estimator`, `event_log`, `alerts`, `notify`, `backup`, `web_api`, `mqtt_bridge`, `ui_*`.

`feed_manager` is a state machine per in-flight feed:
`CREATED → SENT → ACKED → RESULT_OK | RESULT_FAIL | TIMEOUT_UNKNOWN`
and is the only module permitted to originate `MSG_FEED`.

### 7.3 Node software structure
Cooperative loop (no RTOS needed), driven by a non-blocking scheduler and a **hardware timer / ticker** dedicated to the relay-off deadline.

Modules: `radio`, `actuator`, `limits`, `profile`, `sched_local`, `timekeeper`, `rtc`, `sensors`, `storage`, `ota`, `diag`.

**Actuator state machine**
```
IDLE ──FEED accepted──► PRE_CHECK ──ok──► ON(pulse k) ──on_ms──► OFF_GAP ──more pulses──► ON...
  ▲                         │fail                                      │done
  └──── COOLDOWN ◄──────────┴──────────── RESULT reported ◄────────────┘
FAULT (latched) ◄── any watchdog/limit/sensor violation → relay OFF, alert, require hub ack/clear
```

**PRE_CHECK** (all must pass, else reject with reason):
1. Not in FAULT, cooldown or `manual_stop`.
2. `amount` ≤ profile per-feed cap; computed `on_ms` ≤ `max_on_ms_single`.
3. Hourly/daily counters would not be exceeded.
4. Supply voltage above threshold (if monitored).
5. Time is valid (scheduled feeds only; manual feeds are allowed without valid time).
6. Limits are confirmed (SR8, scheduled feeds only).
7. The `feed_id` has been persisted (SR6).

**Relay-off guarantee:** when the relay turns on, arm a timer for `min(on_ms, max_on_ms_single) + margin`. The timer ISR forces the pin OFF independent of the main loop. Additionally the watchdog resets the chip if the loop stalls; the pin defaults OFF on reset.

### 7.4 Time handling
- **Hub:** NTP over Wi-Fi when available; otherwise the onboard PCF85063 RTC (CR927 backup). The hub checks the RTC's oscillator/voltage-low flag at boot, just as the nodes do. The UI shows clock source and staleness.
- **Node:** receives `TIME_SYNC` from the hub at least every heartbeat and tracks with `millis()` between syncs.
- **Node RTC:** the DS3231 stores UTC; `TIME_SYNC` carries the time-zone and daylight-saving rule, so local time is computed on the node. At boot the node reads the RTC, checks the oscillator-stop flag and range-checks the value, and only then marks time valid.
- **Drift correction:** on every `TIME_SYNC` the node compares the RTC with hub time. If the offset is above 2 s it rewrites the RTC and counts a re-sync; the measured offset is reported as `rtc_drift_ms`. Persistent drift or a set oscillator-stop flag raises `rtc_lost` and an alert on the hub. Reads that jump backwards or far from `millis()` tracking are rejected.
- **Trust window (SR7):** with a healthy RTC, time is trusted while the last sync is within `rtc_trust_days` (default 30). Without one, 24 h without a fresh sync sets `time_invalid` and scheduled feeds stop.
- **Power loss:** a node with a healthy RTC resumes its fallback schedule immediately after a reboot, without waiting for the hub (§9.6). Only a node whose RTC is missing or failed loses time on its own reset.
- **Daylight saving:** a local time inside the skipped hour runs just after the gap; a local time inside the repeated hour runs once (first occurrence). Covered by host tests (§11.1).

### 7.5 Persistence
- **Hub:** NVS/LittleFS for config, profiles, schedules, stock levels and refill log; SD for the append-only feed log. Write with atomic rename / double-buffered slots + CRC. Limit write frequency (flash wear).
- **Node:** small config sector with A/B slots + CRC for profile, node ID, channel, key; a separate wear-levelled counter region for `day_units`, `hour_units`, `last_feed_id`.
- **Log retention:** feed history is kept for a configurable period (7, 30 or 90 days; default 30); older events are pruned. If the SD card is full or missing, the hub keeps recent events in RAM only, shows a banner in History and an alert, and keeps feeding normally. **Feeding never depends on the SD card.**

### 7.6 OTA
- **Hub:** ESP-IDF OTA to a second app partition with rollback on failed boot.
- **Node:** the hub sends `ENTER_OTA`; the node forces the relay OFF and locks out actuation, joins the hub's AP or Wi-Fi, pulls the image and verifies its hash. The C3 uses dual-slot rollback (§4.9); the ESP8266 uses its own HTTP-based flow. Keep a serial recovery path in case of a bad image.

### 7.7 Emulation (QEMU)
Espressif maintains a QEMU fork that emulates the ESP32, **ESP32-S3** and **ESP32-C3**. It is installed with `idf_tools.py install qemu-xtensa qemu-riscv32`. ESP-IDF v5.3.1 or later is needed for `idf.py qemu` on the S3.

**Emulated:** CPU (dual-core on S3), UART, interrupt matrix, GPIO strap, SPI NOR flash and flash encryption, octal PSRAM (S3), eFuse, RNG, GDMA, AES/SHA/RSA, secure boot v2. There are also two virtual devices that real chips do not have: an **OpenCores Ethernet** controller for networking and an **RGB framebuffer** for GUI testing.

**Not emulated:** Wi-Fi, Bluetooth, USB and general-purpose SPI. The feature table does not list I²C, the RGB LCD peripheral, timers or watchdogs either, so treat them as missing until checked. That means no ESP-NOW, no SD card, no touch controller, no CH422G and no RTC.

**What this gives the project:**
- **Hub UI without hardware:** run the real LVGL hub firmware with `idf.py qemu --graphics monitor` and the framebuffer display driver. Feed touch input through a debug UART or injected events.
- **Hub networking:** the web dashboard, MQTT and NTP code run over the emulated Ethernet (`openeth`) against a broker on the host.
- **Hub ↔ node logic:** because ESP-NOW is not emulated, add a `udp` transport behind the existing `transport` interface (§6.8). Emulated hubs then talk to the PC fake-node simulator (§13) or to C3 node images also running in QEMU.
- **Security and storage:** test flash encryption, secure boot and NVS/A-B config slots on emulated eFuses and flash without burning real chips.
- **CI:** boot smoke tests and scripted UI screenshots for both S3 and C3 images on every commit (`idf.py qemu` runs headless).

**Limits:** host unit tests stay the main safety test for `node_core`, and the relay-off timer, watchdogs, I²C, ESP-NOW and real timing still need the bench (§11.2). QEMU reduces bench time; it does not replace it.

---

## 8. User interface (LVGL)

**Screens**
1. **Home / dashboard** — a tile per node: name, icon, next feed (ignoring skipped occurrences), last result, hopper/supply status, and a hold-to-feed button showing the amount. The tile note shows what is left of the hourly cap; a stock-estimate bar with an "estimate" caption runs along the bottom edge. A hold-mode banner appears under the summary strip when active.
2. **Node detail** — profile, recent events, test button, calibrate, pause schedule, missed-feed policy, and fallback-schedule state (when pushed; valid or expired). Feed preview under the feed button, a stock-estimate panel with storage capacity, a "Refilled" action and **[NEW in v0.7]** a "Set size and level" action (custom total capacity and amount now), time source and RTC health, and a Notes panel.
3. **Schedule editor** — time picker, weekday toggles, amount, enable, and the daylight-saving rule. "Skip next" / "Undo skip" per entry and "Hold all feeds" in the header.
4. **Calibration wizard** — choose test run time (seconds) or amount → dispense → enter weight/volume → compute and review → save. **[NEW in v0.7]** Run time is capped at the node's max on-time; a measured value under 5 g (or ml) is rejected as too small to weigh; results show as g/s.
5. **Pairing** — discover list, confirmation-code step, accept, name, choose profile template, enter hopper size and the amount in it now **[NEW in v0.7]** (template default 1500 g gate / 5000 g auger / 1000 ml pump, editable).
6. **History** — 7-day chart and per-node totals above a filterable list; retention and SD status; CSV export.
7. **Alerts** — active and acknowledged.
8. **Settings** — Wi-Fi, time zone/NTP, brightness/screen-off, PIN lock and scope, OTA, factory reset, plus:
   - **Remote access** — web dashboard, MQTT, and the notification messages panel.
   - **Storage** — history retention, SD status.
   - **Backup and export.**
   - **Run setup wizard.**
9. **Setup wizard** — **[NEW in v0.7]** six steps: time zone and NTP, Wi-Fi, first node (existing or newly paired), hopper size and level (skipped for a node just paired, because pairing asked), first schedule, done. Opened from the empty Home screen or from Settings.

**UX rules**
- "Feed now" requires hold-to-confirm to avoid accidental taps.
- The screen auto-dims; the first touch on a dimmed screen only wakes it.
- Always show hub time source and last-sync age.
- Clear banner when a node is in FAULT or offline.
- PIN lock scope is explicit: "Settings only" or "Settings and manual feeds". A correct PIN unlocks for 60 s. Schedules and automatic feeds are never blocked by the PIN.
- Clearing a node fault asks the user to confirm the gate is physically closed.
- **[NEW in v0.7]** Home tiles: the header (status, name, next feed) opens the node page; a faulted tile offers "Clear fault" and an offline tile "Check connection" directly, while the hold-to-feed button stays the only feed action. Names wrap to two lines before being cut off.
- **[NEW in v0.7]** Number fields (capacity, amounts) use the dark field style with the unit inside the field and a live summary ("About 7 % full · 2799 g to fill") with a level bar that turns amber at 20 % or less; an "It is full" shortcut copies the total into the amount.
- **[NEW in v0.7]** Settings has a sticky jump bar (Restart, Display, Time, Network, Remote, Security, Storage, Firmware, Nodes, Danger). Restart hub is first and asks for confirmation. "Remove node" and factory reset sit in a Danger zone; factory reset needs the typed word `RESET`.
- **[NEW in v0.7]** Dark and light themes (Settings > Display), each contrast-checked separately; a branded boot screen plays on power-up and restart (about 4 s, tap to skip, relay outputs shown OFF first); an empty hub shows a first-run "Add your first feeder" screen.

---

## 9. Behavioural specifications

### 9.1 Feed request flow
```
Scheduler/UI/MQTT ──► feed_manager.create(feed_id,node,amount)
   hub ── FEED ─────────────► node
   hub ◄── FEED_ACK ───────── node   (accepted / rejected + reason)
          node: PRE_CHECK → persist feed_id → actuate → record
   hub ◄── FEED_RESULT ────── node   (status, actual_on_ms, pulses, gate_ok)
   hub: log + UI + MQTT event; update daily totals and stock estimate
```

### 9.2 Result codes
**Node results:** `OK`, `REJ_COOLDOWN`, `REJ_LIMIT_HOUR`, `REJ_LIMIT_DAY`, `REJ_TOO_LONG`, `REJ_NO_TIME`, `REJ_FAULT`, `REJ_LOW_VOLT`, `REJ_LIMITS_UNCONFIRMED`, `REJ_MANUAL_STOP`, `ABORTED`, `GATE_NOT_OPENED`, `GATE_STUCK_OPEN`.

**Hub-side results:** `TIMEOUT_NO_RESULT`; `SKIPPED_USER` and `SKIPPED_HOLD` record a scheduled feed the hub did not send because of "skip next" or hold mode.

### 9.3 Calibration
1. Select node, enter test amount (e.g. 10 pulses or 5 s). **[NEW in v0.7]** For a gate calibrated by time the test run is 2 s by default and cannot exceed the node's max on-time.
2. Node runs it under normal limits.
3. User weighs dispensed food (or measures volume) and enters the value. **[NEW in v0.7]** Values under 5 g (or ml) are refused as too small to weigh reliably; the user runs a longer test.
4. Hub computes units-per-pulse/second, applies a sanity range, shows before/after.
5. Repeat ≥ 3 times, use the median; store and push `CONFIG`.
6. Re-run when food type or hopper geometry changes. Show a "calibration age" field.

### 9.4 Missed-feed policy (per node)
- Set on the node detail screen and stored in the profile (`missed_policy`, `catchup_window_min`).
- **Default — skip:** missed feeds are skipped after reconnect; an alert is raised with a one-tap "feed now".
- **Optional — catch up once:** one feed is caught up if within the window (e.g. ≤ 30 min late) and the daily cap allows. If the window has passed when the node returns, the feed stays skipped and the alert says so.
- Never stack multiple missed feeds.

### 9.5 Concurrency
- The hub staggers feeds so no more than `N` solenoids are energised at once (default 1 per shared supply).
- Node-level mutex: one actuation at a time per node.

### 9.6 Offline (hub down) fallback
- The hub pushes the next 7 days of each node's schedule (`SCHEDULE_SET`) with a version number, including `skip_until` / `hold_until`.
- The node executes only entries within a **freshness window** (schedule pushed ≤ 7 days ago) and only with valid time (SR7).
- With a healthy RTC, a node that reboots or loses power while the hub is down still runs its valid fallback schedule.
- On hub return, the node reports what it ran; the hub reconciles it into the log and the stock estimate.
- Per node, the hub shows when the schedule was last pushed, whether the fallback copy is valid or expired, and the time source (e.g. "RTC, synced 3 d ago"). An offline node whose fallback has expired is flagged because it will not feed on its own; `rtc_lost` is flagged too.

### 9.7 Stock estimate, capacity left and refill log
There is no level sensor in v1, so stock is an **estimate** computed on the hub from the node's **storage capacity** and the amounts it has actually dispensed.

- **Storage capacity** (`storage_capacity`, g or ml, same unit as feeds) is the quantity in a full hopper or reservoir. Entered or confirmed at pairing; editable on the node page.
- **Set size and level** **[NEW in v0.7]** lets the user enter any full size (1–100000, in the node's unit) and the amount in it now (0 up to the size). The hub rejects a missing, non-numeric, out-of-range, or larger-than-total value and keeps what was typed. Saving sets `storage_capacity` and `stock_left`, writes a refill-log entry marked "set", flags the capacity as custom and offers undo. The pairing dialog and the setup wizard ask the same two questions (defaults 1500 g gate, 5000 g auger, 1000 ml pump), and the hub pushes `storage_capacity` to the node with its profile.
- **Capacity left:** `stock_left = stock_left_previous − dispensed_actual`, clamped at 0. The shown level is `stock_left / storage_capacity`. The UI shows both, e.g. "about 930 g left of 1500 g (62 %) — estimate".
- **Dispensed amount** is what the node reports in `FEED_RESULT` (pulses × calibrated amount per pulse, or `actual_on_ms` × calibrated rate), **not** the commanded amount. This applies to scheduled, manual, MQTT and catch-up feeds, and to feeds run from the fallback schedule while the hub was down (reconciled on return). `ABORTED` counts the partial amount; `REJ_*` and `GATE_NOT_OPENED` count 0.
- **No double counting:** each feed is subtracted once, keyed by `feed_id` (SR6), so a retried or replayed result cannot subtract twice.
- **Refill** resets the stock: Full (`stock_left = storage_capacity`), Add an amount (`min(storage_capacity, stock_left + added)`), or About 75 % / About 50 %. Each refill is logged with time and level before/after, and can be undone.
- **Low level** triggers an alert (and, if enabled, a notification) when `stock_left` falls below the larger of 20 % of capacity and one day of planned use. Both limits are configurable.
- **Lasts about N days** = `stock_left` ÷ average daily use. Planned use comes from enabled schedules; once there are 7 days of history the hub uses the actual 7-day average. Hidden when neither is available.
- **Why it drifts:** food settles or bridges, calibration is off, food is spilled or hand-fed without logging. Every level is therefore labelled "estimate", a note appears when calibration is older than 30 days, and refills correct the number. An optional hopper-low sensor (§4.2) would replace the estimate for the low alarm only.
- **Accuracy goal:** the error of `stock_left` over one refill cycle is documented per food type from weighed refills (§11.2, §11.6); expected to be no better than the calibration tolerance (±15 %).

### 9.8 Feed preview
Before a manual feed the node page shows what will happen: amount, cooldown, remaining hourly and daily cap afterwards, and roughly how long the actuator runs (seconds or pulses). If the node would refuse the feed (fault, cooldown, cap, supply voltage), the preview shows the reason instead. The node still decides; the preview is advisory.

### 9.9 Skip next and hold mode
- **Skip next** marks one occurrence of one schedule entry. At that time the hub records `SKIPPED_USER` and sends nothing; the entry then runs normally. "Undo skip" removes the mark.
- **Hold mode** has a single end time (1 day, 3 days, 1 week or 2 weeks). While active, the hub records `SKIPPED_HOLD` for every scheduled feed and shows a banner on Home and Schedule. It ends by itself with a notice and never catches up skipped feeds. Manual feeds are not blocked.
- Both are pushed to nodes with the schedule (SR10). An offline node may still run its saved fallback schedule until it reconnects, and the UI says so.

### 9.10 Double-feed check
A manual feed within 60 minutes of the node's last `OK` feed first asks "Feed again?", showing when and how much was fed and how much of the hourly cap is left. Scheduled and MQTT feeds are not asked; node caps still apply to all feeds.

### 9.11 Notification messages
- Short text messages are published on `aio/hub/notify` as `{"text":"…"}`; Home Assistant or another app turns them into phone notifications.
- Triggers, each switchable: node offline for 30 min, fault result, missed feed while offline, stock estimated low. A master switch turns all off. "Send test message" is available.
- The hub keeps the last 6 messages and shows whether each was sent ("Not sent" if MQTT is disconnected). The display remains the primary alert channel; notifications are an add-on (goal 6).

### 9.12 Backup, restore and export
- **Back up now** writes nodes, schedules and settings to the SD card with a timestamp. The PIN and MQTT password are **not** included.
- **Restore** asks for confirmation, replaces nodes, schedules and settings, and leaves history, the current PIN and the MQTT password unchanged. Intended for a replaced hub or a bad change.
- The web dashboard can download the backup as JSON and the history as CSV (columns: time, node, amount, unit, result, source).

### 9.13 Setup wizard
Steps: (1) time zone and NTP, (2) Wi-Fi name and a connection test, (3) choose a paired node or pair a new one, (4) **[NEW in v0.7]** hopper size and amount in it now (validated as in §9.7; skipped when the node was just paired), (5) first schedule (time, days preset, amount within the node's per-feed cap), (6) done, with reminders to calibrate and to tap Refilled when topping up. Back is available at every step. Nothing is written to a node until the schedule step is saved.

---

## 10. Failure-mode analysis (FMEA-lite)

| # | Failure | Effect | Mitigation | Detection |
|---|---|---|---|---|
| 1 | Node crashes with relay ON | Continuous feeding | HW timer forces OFF; WDT reset; pin defaults OFF; spring-return gate; solenoid duty cap | Missing heartbeat, FEED_RESULT absent |
| 2 | Relay contacts weld | Gate stuck open | Flyback diode, snubber, derated relay; optional gate reed sensor; optional second series switch for critical use | Gate sensor / current sensing |
| 3 | Brownout on solenoid pull-in | Node reset mid-feed | Separate rails, caps, adequate supply; staggering | `vin_mv` dips, reset reason in heartbeat |
| 4 | Radio loss | Feeds not delivered | Fallback schedule on node; alerts | Heartbeat timeout |
| 5 | Duplicate/late command | Double feed | `feed_id` idempotency, node caps | Log audit |
| 6 | Bad config (huge on_ms) | Overfeed | Node-side clamps independent of hub | REJ_* codes |
| 7 | Time wrong (NTP failure, RTC drift) | Wrong feeding times | Show time source/age; refuse scheduled actions on invalid time | UI banner |
| 8 | Hopper empty/bridged | Silent underfeeding | Hopper-low sensor; weight check (future); jam alerts | Alerts |
| 9 | SD/flash corruption | Lost log/config | CRC + A/B slots, atomic writes, config backup | Boot self-check |
| 10 | Replay/rogue ESP-NOW frame | Unauthorised feed | Sequence numbers, HMAC, MAC allow-list, node caps | Rejected-frame counter |
| 11 | OTA failure | Bricked node | Relay lockout, image hash, recovery via serial | OTA status |
| 12 | Mains power cut/restore storm | Repeated reboots | No auto-catch-up, boot-time relay OFF, debounce restart | Reset counter |
| 13 | Power loss after relay on, before result stored | Double feed on hub retry | Persist `feed_id` before relay on; never auto-repeat an in-progress record | Boot-time in-progress record |
| 14 | Swapped/reset node has zeroed counters | Caps silently reset | `limits_unconfirmed` state until hub pushes counters | Heartbeat flag, `REJ_LIMITS_UNCONFIRMED` |
| 15 | Unpaired node stays paired while offline | Node still obeys commands | Pending-unpair state, retry on heartbeat | Node list shows pending |
| 16 | Hub unreachable or SD full | Lost log, no visibility | Feeding independent of SD; RAM buffer, banner, alert | History banner, alert |
| 17 | Need to stop a feeder immediately | Overfeed while hub is unreachable | Physical stop switch (SR9) | `manual_stop` heartbeat flag |
| 18 | Stock estimate drifts from the real level | Silent underfeeding or false low alerts | Labelled "estimate" everywhere; refill log with correction; optional hopper-low sensor | Refill log, jam or low alerts |
| 19 | Skip or hold not honoured by an offline node's fallback schedule | Feed happens that the user cancelled | Push `skip_until` / `hold_until` with the schedule (SR10); UI warns for offline nodes | Result reconciled after reconnect |
| 20 | Backup restore overwrites newer settings or leaks secrets | Lost changes, exposed PIN or MQTT password | Confirmation dialog; secrets excluded; timestamp shown | Restore toast, settings log |
| 21 | Notification message not delivered | Fault goes unnoticed away from the hub | Sent / not-sent status per message; display and MQTT state remain primary | "Not sent" status |
| 22 | DS3231 backup cell flat or removed | Node boots with wrong time and feeds at the wrong hour | Oscillator-stop flag and range check at boot; time invalid until `TIME_SYNC`; falls back to the 24 h rule (SR7) | `rtc_lost` flag, hub alert |
| 23 | RTC drifts or I²C read returns garbage | Wrong feeding times | Compare with `TIME_SYNC`, rewrite above 2 s; reject reads that jump backwards or far from `millis()` tracking | `rtc_drift_ms` in heartbeat |
| 24 | Stock estimate counts the commanded amount, or counts a feed twice | Wrong capacity left | Use node-reported amount; subtract once per `feed_id` | Refill log shows the correction |
| 25 | Hub I²C bus (GPIO8/9) locks up | Touch, backlight, SD chip select and hub RTC lost together; screen may go dark | Bus recovery (9 clock pulses + re-init), transaction timeouts, task watchdog; feeding continues because nodes are independent | I²C error counter, alert on web/MQTT |
| 26 | Gate calibrated by open time drifts as hopper level or bridging changes the flow rate **[NEW in v0.7]** | Doses too large or small; stock estimate drifts | Prefer metering chamber (D7); re-calibrate when food or geometry changes; 30-day calibration-age notice; node on-time and caps still enforced | Calibration age on node page; weighed refill check (§11.2) |

---

## 11. Test plan

### 11.1 Host unit tests
- Frame encode/decode, CRC/HMAC, version handling.
- Limit logic (`limits.c`): hour/day rollover, cooldown, overflow cases.
- Schedule evaluation incl. DST/time-zone edge cases: a local time in the skipped hour, a local time in the repeated hour, and a schedule exactly at the transition.
- Feed state machine transitions, retry/idempotency, including a reset between persist and result.
- Missed-feed policy: catch-up inside and outside the window, never stacked, daily cap respected.
- Time validity: RTC trust window, oscillator-stop flag, 24 h fallback rule.
- Stock countdown and "lasts about" with and without a schedule; no double subtraction on retried results.
- `nextRun` with skip and hold across a daylight-saving change.
- Double-feed window edges (59 and 61 min).
- Backup round-trip without PIN or MQTT password; CSV quoting of node names containing commas or quotes.

### 11.2 Bench / hardware-in-the-loop
- Scope on relay pin: boot, reset, WDT reset, brown-out — relay never energises unintentionally.
- Scope on 5 V rail during solenoid pull-in: record sag/ripple; verify no MCU resets.
- Measure real solenoid temperature rise at max duty.
- Radio range and packet-loss test in the real installation environment, including a router channel change.
- Pull power on a node mid-feed ×100; verify counters and logs stay coherent.
- Kill the hub mid-feed; verify the node completes/aborts safely.
- Swap in a freshly flashed node and confirm it refuses scheduled feeds until limits are confirmed.
- Press the physical stop during a feed and confirm the relay drops and the hub is told.
- **Node RTC:** power-cycle test (time survives, schedule resumes without `TIME_SYNC`); pull the backup cell and confirm `rtc_lost` and refused scheduled feeds; 30-day drift measurement against NTP; I²C fault injection (no response, stuck bus, garbage time).
- **Stock estimate:** weigh a full hopper, run a set of feeds, compare `stock_left` with the weighed remainder per food type; verify fallback-schedule feeds are counted once after the hub returns.

### 11.3 System tests
- 48 h soak with fake loads, then 7 days with real food.
- 8-node stress: simultaneous schedule triggers, verify staggering.
- Wi-Fi off/on cycles, NTP failure, MQTT broker down.
- Fuzzing: random/malformed frames to the node — expect no actuation and no crash.
- The soak includes at least 100 forced power cuts per node type (at random points, including during a feed), at least 20 hub resets during active feeds, and at least 10 router channel changes.

### 11.4 UI tests
- Setup wizard with an existing node, a newly paired node, and no node; the Hopper step is skipped after pairing and Back from Schedule returns to it. **[NEW in v0.7]**
- "Set size and level", pairing and the wizard Hopper step with: empty, 0, 1, 100000, 100001, non-numeric, amount above total, amount equal to total. **[NEW in v0.7]**
- Gate calibration by time: test time above max on-time is blocked; a measured value under 5 g is refused. **[NEW in v0.7]**
- Number fields stay readable in dark and light themes. **[NEW in v0.7]**
- Notification status with MQTT connected and disconnected.
- History chart with no data, one day of data, and a mix of g and ml nodes.

### 11.5 Emulation tests (QEMU, in CI)
- Hub S3 image boots under QEMU, mounts NVS, and survives a forced reset with config intact.
- Every LVGL screen renders on the virtual framebuffer; scripted walk-through of the setup wizard, feed-now hold, skip/hold and history; screenshots kept as CI artefacts.
- Hub web dashboard and MQTT over emulated Ethernet against a broker on the CI host: feed command, retained state, LWT, notification messages.
- Hub ↔ fake node over the `udp` transport: 1000-feed loop with no duplicates, retries, `TIMEOUT_UNKNOWN`, offline/online transitions.
- C3 node image boots under QEMU; NVS A/B slots and `feed_id` persistence survive a reset injected between persist and result.
- Flash encryption and secure boot flows exercised on emulated eFuses.

### 11.6 Acceptance criteria for v1
- 0 unintended actuations across the soak.
- All safety requirements SR1–SR10 demonstrated by test.
- Feed accuracy within ±15 % (after calibration) for each supported food type, or documented tolerance.
- 30-day no-intervention run.
- Skip next, hold mode and the double-feed check demonstrated on the bench, including a node that goes offline while a skip or hold is active (SR10).
- Stock-estimate error measured against weighed refills for each food type and documented.

---

## 12. Milestones

| M | Name | Deliverable | Exit criteria |
|---|---|---|---|
| M0 | Setup | Repo, PlatformIO envs, CI, shared proto lib, decision log, QEMU job, LVGL docs MCP config | All targets build; host tests run; hub and C3 images boot in QEMU in CI |
| M1 | Bench node | ESP8266 **and** ESP32-C3 SuperMini + DS3231 + relay + solenoid + diode; hard timer; serial CLI; HAL v1 | SR1/SR2 verified on scope on both MCUs; calibration numbers collected; first range, packet-loss and channel test on real boards; verified ESP-NOW peer limits recorded; RTC trust defaults confirmed |
| M2 | Link | ESP-NOW hub↔node: HELLO, FEED/ACK/RESULT, heartbeat, channel scan, `skip_until` / `hold_until` fields | 1000-feed loopback with no duplicates |
| M3 | Hub core | Scheduler, time (NTP/RTC), feed_manager, storage, log (no UI) | Schedules fire correctly across reboots |
| M4a | Hub UI — operate | Dashboard, node detail, schedule editor, history, alerts; skip next, hold mode, feed preview, double-feed check, stock estimate, notes, history chart | Daily operation via touchscreen; all screens pass the QEMU screenshot run |
| M4b | Hub UI — set up | Pairing (with confirmation code), calibration wizard, settings, PIN; backup, export, setup wizard | Full setup workflow via touchscreen |
| M5 | Profiles & types | Profile system; motor/pump variants | Mixed-type nodes working |
| M6 | Remote | Web dashboard, MQTT, HA discovery, notification messages | Control + telemetry from phone |
| M7 | Resilience | Offline schedule (incl. skip/hold on nodes), missed-feed policy, alerts, sensors | FMEA items 1–8, 13–17 and 22–23 verified |
| M8 | OTA & hardening | OTA both sides, security pass, fuzzing, 30-day soak | Acceptance criteria met |
| M9 | Packaging | Enclosures, wiring harness, labels, user guide | Install on real site |

Rough effort (solo, part-time): M0–M2 ≈ 2–3 weeks, M3 ≈ 1–2 weeks, M4a–M4b ≈ 3–4 weeks, M5–M7 ≈ 3–4 weeks, M8–M9 ≈ 3–4 weeks. Re-estimate after M2 (the radio link is the biggest unknown). M4 is split because the calibration wizard, pairing and schedule editor are each substantial in LVGL.

---

## 13. Repository layout (proposed)

```
aio-feeder/
├─ PLAN.md
├─ .claude/
│  └─ .mcp.json        # LVGL docs MCP server (https://lvgl.mcp.kapa.ai/)
├─ docs/               # wiring diagrams, calibration logs, decision records
├─ lib/
│  └─ proto/           # shared frame/profile/status definitions (C, host-testable)
├─ hub/
│  ├─ platformio.ini
│  └─ src/             # ui/, radio/, sched/, net/, storage/, feed_manager/
├─ node_core/          # portable node logic: limits, actuator FSM, profile, time validity, protocol glue (host-tested)
├─ node/
│  ├─ platformio.ini   # envs: node_esp8266, node_esp32c3
│  ├─ hal_esp8266/     # gpio, hwtimer, wdt, storage, radio, rtc, ota, adc
│  ├─ hal_esp32c3/
│  └─ src/             # main loop / glue only
├─ test/               # host unit tests (Unity/Catch2)
├─ tools/              # provisioning, log viewer, fake-node simulator (UDP transport)
│  └─ qemu/            # QEMU launch scripts, flash/eFuse images, UI screenshot runner
└─ hardware/           # schematics, BOM, enclosure CAD
```

Build a **fake node simulator** (on an ESP32 or a PC) early so hub development isn't blocked on hardware. Together with the QEMU hub image (§7.7) it allows most hub work before any node hardware exists.

`.claude/.mcp.json`:
```json
{
  "mcpServers": {
    "lvgl": { "type": "http", "url": "https://lvgl.mcp.kapa.ai/" }
  }
}
```
or `claude mcp add --transport http lvgl https://lvgl.mcp.kapa.ai/`.

---

## 14. Risks and open questions

### 14.1 Risks

| Risk | Likelihood | Impact | Plan |
|---|---|---|---|
| ESP-NOW channel/coexistence issues with hub Wi-Fi | Med | High | Channel scan (§6.2); transport abstraction for MQTT fallback; test in M1 |
| Solenoid thermal / relay wear | Med | Med | Duty caps, snubber, MOSFET upgrade path |
| Timed dosing inaccurate across food types | High | Med | Metering chamber; per-type calibration; load cell later |
| GPIO scarcity on hub board | High | Low | Confirmed: only GPIO6 appears free (§4.1). Extra I/O goes on the I²C header or the isolated DI/DO |
| Shared hub I²C bus (touch, CH422G, RTC) locks up | Med | High | Bus-recovery routine, transaction timeouts, watchdog; FMEA #25 |
| QEMU gaps hide hardware bugs (no Wi-Fi, ESP-NOW, I²C, timers) | Med | Med | QEMU for UI, networking and storage only; safety and radio tests stay on the bench |
| ESP8266 legacy SDK / limited resources | Low | Med | HAL + `node_core` keep logic portable; C3 is the target for new nodes |
| SuperMini RF/antenna quality varies by clone | Med | Med | Range test on each batch, TX-power tuning, antenna placement rules (§4.9) |
| HAL divergence between the two node targets | Med | Low | Shared host tests + on-target conformance test on both boards |
| Flash wear from frequent writes | Low | Med | Debounce, wear-leveling, SD for logs |
| Pairing key exposure over the air | Low | Med | Confirmation code and wrapped key (§6.3); document residual risk for ESP8266 |
| Encrypted-peer limit below target node count | Med | Med | HMAC default (§6.4); record verified limits in M1 |
| Unsafe CR2032 charging on cheap DS3231 modules | Med | Med | Choose module/cell carefully or remove the charge path (§4.2) |

### 14.2 Open questions
1. Exact food/animal types for v1 and their particle sizes/viscosity?
2. How many nodes at launch, and how far (and through what obstacles) from the hub?
3. Is the installation indoors or outdoors (temperature, moisture, dust)?
4. Is a weight-feedback load cell wanted in v1, or deferred?
5. Remote access: local network only, or exposed to the internet (affects auth/TLS)?
6. Single shared 12 V supply or one per node?
7. Fit a LiPo to the hub for ride-through during mains cuts, or rely on the onboard RTC only?
8. Should the PIN also protect pairing and factory reset when the scope is "settings only"? (Currently yes, because they live in Settings.)
9. Should hold mode also stop a node's fallback schedule while it is offline, or only the hub's? (SR10 says push it; confirm the node-side field in M2.)
10. Should "Skip next" and hold mode be allowed over MQTT and the web dashboard, or only on the touchscreen?
11. Is a 60 min double-feed window right for fish and poultry, or should it be set per node?
12. Which DS3231 module and backup cell? **[VERIFY]**
13. How is the real storage capacity per node obtained — weigh a full hopper once at setup, or enter a volume and a bulk density?
14. Should hand-fed amounts be loggable so the estimate stays close, or is the refill correction enough?
15. **[NEW in v0.7]** Is calibrating a solenoid gate by open time acceptable for v1, or must it use a metering chamber and pulse counts (D7)? What on-time limit does the real solenoid allow? **[VERIFY]**

---

## 15. Decision log

| ID | Decision | Status |
|---|---|---|
| D1 | Hub = Waveshare ESP32-S3-Touch-LCD-4.3B; nodes = ESP8266 **or** ESP32-C3 SuperMini (see D8) | Decided |
| D2 | Node actuator = 5 V relay + 12 V push-pull solenoid | Decided (v1) |
| D3 | Transport = ESP-NOW (Wi-Fi/MQTT fallback) | Proposed |
| D4 | Node enforces safety caps; hub is advisory | Proposed |
| D5 | Single node firmware + hub-managed profiles | Proposed |
| D6 | Hub = **pure ESP-IDF** (v5.4, CMake project in `hub/`, esp_console REPL bench CLI, native ESP-NOW/NVS/SNTP); nodes stay Arduino/PlatformIO via HAL | **Decided** (v0.3): unlocked by the user's ESP-IDF+QEMU direction; `idf.py` path is also what M4a LVGL and the QEMU boot smoke (ci.yml `qemu-hub`) build on |
| D7 | Metering chamber vs timed gate | Open — prototype both |
| D8 | Node MCU = ESP8266 **or** ESP32-C3 SuperMini via HAL; C3 preferred for new builds | Proposed |
| D9 | Relay default-OFF enforced by external pull resistor, not firmware alone | Proposed |
| D10 | Persist `feed_id` before relay on | Proposed |
| D11 | Replaced/reset nodes start `limits_unconfirmed` | Proposed |
| D12 | Physical stop on every node | Proposed |
| D13 | HMAC on unencrypted peers is the v1 default; native encryption only if verified peer limits allow | Proposed |
| D14 | Pairing uses a confirmation code and a wrapped key | Proposed |
| D15 | Time trust window: 24 h without a healthy RTC; `rtc_trust_days` (default 30) with one (see D21). Confirm both in M1 | Proposed |
| D16 | Stock level is a counted estimate, always labelled as such; a sensor can replace it for alarms later | Proposed |
| D17 | Skip and hold are pushed to nodes with the schedule (SR10) | Proposed |
| D18 | Backups exclude the PIN and the MQTT password | Proposed |
| D19 | Notifications are MQTT messages on `aio/hub/notify`, not a separate cloud service | Proposed |
| D20 | Double-feed window = 60 min, manual feeds only | Proposed |
| D21 | DS3231 RTC is a core part of every node; RTC time is trusted only while the oscillator-stop flag is clear and the last sync is within `rtc_trust_days` | Proposed |
| D22 | Stock left = storage capacity minus amounts actually dispensed, computed on the hub and keyed by `feed_id` | Proposed |
| D23 | ESP8266 pin map: relay on GPIO14, optional inputs on GPIO13 / GPIO16, GPIO4 / GPIO5 for I²C | Proposed |
| D24 | Hub time uses the onboard PCF85063 RTC; no external DS3231 on the hub | Decided |
| D25 | Hub UI, networking and storage tested in Espressif QEMU in CI, with a `udp` transport to a fake node; ESP-NOW and safety stay on the bench | Partially done (v0.3): QEMU boot smoke in CI (serial markers, no-panic); hub↔fake-node pipeline e2e runs on the host over loopback UDP (`test/test_e2e_sim`, `tools/sim`) since QEMU has no Wi-Fi/ESP-NOW; in-QEMU serial/chardev transport to the fake node deferred to M4a |
| D26 | LVGL docs MCP server (`lvgl.mcp.kapa.ai`) is part of the dev setup, checked into `.claude/.mcp.json` | Proposed |
| D27 | The hub prototype calibrates a gate node by open time (g/s) with a 10 s max on-time; the pulse-count metering-chamber design stays preferred pending D7 | Proposed — **[VERIFY]** |
| D28 | Storage capacity is user-entered (size and amount now) at pairing, in the wizard and on the node page, not fixed by template | Proposed |
| D29 | M2 pairing is serial-provisioned: node `pair <hub_mac>` generates a random 256-bit key, the operator pastes it into the hub CLI. OTA PAIR_REQ/PAIR_ACCEPT with confirmation code + wrapped key (D14) lands in M3 with the hub UI; wire formats and crypto are already in `lib/proto/pairing.h` and host-tested | Proposed |
| D30 | On an otherwise quiet link the hub acks every non-reply HEARTBEAT with STATUS_REQ; the node answers with a heartbeat whose frame-flags bit0 is set, which the hub does not re-ack. Keeps the node's §6.2 freshness window fed without a ping-pong | Proposed |

---

## 16. Future extensions
- Load cell (HX711) per node for closed-loop dosing and food-level monitoring.
- MOSFET driver with pull-in/hold PWM; reverse-capable motor node (H-bridge).
- RFID/animal ID for per-animal portions.
- Wired nodes over RS485 (GPIO43/44) or CAN (GPIO15/16) using the hub's onboard transceivers.
- Camera snapshot at feed time.
- Battery/solar node variant (requires a deep-sleep redesign — not compatible with v1 listen-always nodes).
- Companion Android app (BLE provisioning + remote control).

---

## Revision history
| Version | Summary |
|---|---|
| v0.2 | Initial engineering plan |
| v0.3 | Plan-review fixes: feed_id persistence, limits-unconfirmed state, physical stop, pairing confirmation code, HMAC default, time trust window, missed-feed policy, log retention, M1 radio test, M4 split |
| v0.4 | Features from the hub display prototype: stock estimate, feed preview, skip next, hold mode, double-feed check, history chart, node notes, backup/export, notifications, setup wizard |
| v0.5 | DS3231 RTC on every node; stock estimate reworked around storage capacity and actually dispensed amounts; ESP8266 pin map updated for I²C |
| v0.6 | Hub specs verified against Waveshare docs (pins, CH422G map, onboard PCF85063 RTC, 7–36 V input, LiPo option); QEMU emulation plan and CI tests; LVGL docs MCP in the dev setup; shared-I²C risk and FMEA #25 |
| v0.7 | Synced with the hub display prototype: timed gate calibration (g/s) with a too-small-to-weigh guard, custom storage capacity ("Set size and level"), capacity entry in pairing and a Hopper wizard step, Home tile actions, Settings jump bar and Danger zone, themes, boot screen, first-run screen; FR23–FR24, FMEA row 26, D27–D28, question 15 |
