# EmreTag — BLE 6.0 Channel Sounding Distance Tracker

A battery-powered tracker that measures the **real distance** between two nRF54L15 devices using **Bluetooth 6.0 Channel Sounding**, and guides the user to the tag with distance-aware haptic and acoustic feedback.

> **Status:** In development. Channel Sounding ranging works between the tag and an nRF54L15 DK acting as locator — distance values are produced over the air. The tag runs the IMU's own motion, tap and free-fall engines off an interrupt, turns a tap gesture into a find-my-phone request and a burst of free-fall reports into a drop alarm, and exposes all of it over a custom GATT profile. The MCU-side sleep states in [Power architecture](#4-power-architecture) are still design intent.

Built on **Zephyr RTOS** with the nRF Connect SDK.

> This repository holds the **tag (reflector)** firmware. The locator/initiator side lives separately.

---

## 1. Why Channel Sounding

Most BLE trackers estimate distance from **RSSI** — signal strength. That works badly: RSSI drops with obstacles, body shadowing and orientation, so "weak signal" and "far away" are not the same thing. In practice RSSI-based trackers can only tell you *warmer* or *colder*, and they lie whenever something is in the way.

**Channel Sounding** (introduced in Bluetooth 6.0) measures distance directly. Two devices — an **initiator** and a **reflector** — exchange tones across many frequencies and derive range from phase and timing, rather than from how loud the signal is. The result is a distance in metres instead of a signal-strength guess.

This project exists to build that pipeline end to end on real hardware, on a battery budget, rather than on a bench with a debugger attached.

---

## 2. System overview

*Target architecture. See [Status](#7-status) for what is actually running today.*

```
┌─────────────────────────┐         Bluetooth LE          ┌─────────────────────────┐
│  Tag (Reflector)        │◄─────  Channel Sounding  ────►│  Locator (Initiator)    │
│                         │         + GATT/Notify         │                         │
│  nRF54L15               │                               │  nRF54L15               │
│  IMU wake-on-motion     │                               │  distance readout       │
│  buzzer + vibration     │                               │                         │
│  ~250 mAh LiPo          │                               │                         │
└─────────────────────────┘                               └─────────────────────────┘
```

The tag stays in deep sleep until it is moved or called. The locator initiates ranging and receives live distance values over a **custom GATT profile with notifications**. When "Find-Me" is triggered, the tag drives its buzzer and vibration motor so the user can locate it by ear and touch.

---

## 3. Hardware

| Role | Board | Notes |
|---|---|---|
| Tag / reflector | **Seeed XIAO nRF54L15 Sense** | on-board **LSM6DS3TR-C** IMU, intended for wake-on-motion |
| Locator / initiator | **nRF54L15 DK** | development-side counterpart |
| Prototype build | Perfboard | tag-side carrier for battery, buzzer and vibration motor |

**Tag peripherals**

| Function | Detail | State |
|---|---|---|
| Battery | ~250 mAh LiPo | — |
| Acoustic feedback | Buzzer | plain GPIO on/off with timed pulse and blink patterns; PWM tones planned |
| Haptic feedback | Vibration motor | same output layer as the buzzer |
| Motion sensing | LSM6DS3TR-C | interrupt-driven wake-up, double tap, free fall and inactivity on INT1, plus a 104 Hz polling loop for logging |

Both the buzzer and the vibration motor are switched from GATT and by the alarm logic today, but their pins are still placeholders in firmware and are not yet wired to the prototype.

> This is a **firmware and RF project**, not a PCB project — the hardware is a module-based prototype on perfboard. A custom PCB is a later stage, once the firmware and power behaviour are settled.

### RF path

The XIAO nRF54L15 routes the radio through an RF switch that selects between the on-board ceramic antenna and the external u.FL connector. Neither switch node is enabled at boot by the board devicetree, and the upstream Channel Sounding samples are unaware of it, so out of the box the RF path is left undefined and the link budget suffers. The tag firmware powers the switch and selects the ceramic antenna explicitly before starting the Bluetooth stack — see [Measurements](#6-measurements) for what that was worth.

<!-- TODO: buzzer part number + drive circuit (direct GPIO / MOSFET / driver IC) -->
<!-- TODO: vibration motor part + driver -->
<!-- TODO: photo of the assembled prototype -->

---

## 4. Power architecture

> **Partly implemented.** The sensor half of the scheme runs; the MCU half does not. The CPU is still awake and advertising still runs continuously at a fast interval.

On a 250 mAh cell, the interesting engineering is not the radio — it is **staying asleep**. The tag uses a tiered wake-up scheme instead of running continuously:

1. **Deep sleep** — radio and CPU down, IMU running autonomously — *MCU side not implemented*
2. **Motion trigger** — the LSM6DS3TR-C decides on its own that the tag has been moved and raises INT1; the MCU does no polling to notice motion — ✅ **running**. The same engine also drops the accelerometer to 12.5 Hz after ~25 s of stillness and raises it again by itself when motion returns.
3. **Connectable / ranging** — BLE active, Channel Sounding bursts on demand
4. **Find-Me / alarm** — buzzer and vibration motor active; the highest-current state by a wide margin — ✅ **running**, driven from the gesture and drop detection below

The buzzer and vibration motor draw far more peak current than the radio ever does, so the feedback pattern is part of the power budget, not an afterthought. Energy behaviour of each state will be profiled with a **Nordic Power Profiler Kit II (PPK2)**.

---

## 5. Firmware

**Zephyr RTOS** on the nRF Connect SDK.

Source is split by concern: `ble_cs` (connection, advertising, CS reflector), `app_gatt` (the custom profile), `imu`, `button`, `outputs`, and a `main` that owns only the start-up order and the wiring between them.

**Running today**

- **Channel Sounding ranging** against an nRF54L15 DK: the tag takes the reflector role, applies CS default settings and procedure parameters on connection, and serves ranging data through the **Ranging Service (RAS)** responder, so the locator gets real distance values
- **On-chip IMU detection.** Wake-up, double tap, free fall and activity/inactivity all run inside the LSM6DS3TR-C and reach the MCU as a single INT1 interrupt; the handler reads `WAKE_UP_SRC` / `TAP_SRC` to work out which one fired. Thresholds, durations and the inactivity mode are one configuration struct.
- **Gesture and drop handling** — see [Interaction](#interaction) below
- Custom **GATT profile** over 128-bit UUIDs, every characteristic carrying a User Description descriptor
- Timed output primitives: one-shot pulses and repeating blink patterns, each output driving its own delayable work item, so nothing blocks a work queue
- Board I/O straight off devicetree — `DT_ALIAS(sw0)` / `DT_ALIAS(led0)` with `gpio_dt_spec`, GPIO interrupt for the button, work queue hand-off so no Bluetooth call runs in ISR context
- RF switch brought up and the ceramic antenna selected before `bt_enable()`
- IMU sampling loop alongside the interrupts: accelerometer and gyroscope read at 104 Hz, low-pass filtered and exposed through a single accessor — the seam a gesture classifier will read from. The shared PDM/IMU supply rail is enabled from a `SYS_INIT` hook that runs before the sensor driver probes the chip.

### The Zephyr driver only exposes data-ready

Zephyr's `lsm6dsl` driver implements exactly one trigger, `SENSOR_TRIG_DATA_READY`, and asserts on anything else — the wake-up, tap, free-fall and inactivity engines are not reachable through the sensor API at all. This firmware configures them by writing the chip's registers directly over the same I²C bus and takes INT1 as a plain GPIO interrupt, which works as long as the driver is built with `CONFIG_LSM6DSL_TRIGGER_NONE` so it does not claim the pin for itself.

### Interaction

| Trigger | Result |
|---|---|
| Two double taps within 2 s | Vibration buzzes for 1 s and the tag arms |
| The same gesture again within 5 s | **Find phone** — counter incremented and notified |
| More than 4 free-fall reports within 1 s | **Fall detected** — buzzer and motor alternate at 500 ms, and the fall characteristic is notified |
| Phone writes 0, or the tap gesture is repeated | Alarm stops and the counter clears |

The find-phone request needs two gestures rather than one because a single knock against a table is far too easy to produce by accident. While the alarm is sounding the same gesture silences it instead of asking the phone for anything.

### GATT profile

Built on the Nordic LED Button Service UUID base, so the first characteristics are recognised by generic clients.

| UUID suffix | Name | Access |
|---|---|---|
| `1524` | Button | read, notify |
| `1525` | LED | read, write |
| `1526` | Buzzer | read, write |
| `1527` | Vibration motor | read, write |
| `1528` | Find phone | read, write, notify |
| `1529` | Fall detected | read, write, notify |

The two event characteristics carry a counter rather than a flag, so a client that was disconnected or missed a notification can still tell that something happened; writing 0 acknowledges and clears it. Distance values do not appear here — those go over RAS.

**Not yet**

- Distance-aware behaviour on the tag itself: ranging data is served to the locator, but the tag does not act on distance (no feedback tied to range)
- MCU-side power state machine — the sensor sleeps itself, the CPU does not
- PWM buzzer tones (the pattern layer exists, the tone generation does not)
- Locator/initiator firmware (currently the upstream sample on the DK)

---

## 6. Measurements

Numbers are only claimed here once they have been measured on hardware.

| Metric | Value |
|---|---|
| Usable range, ceramic antenna, line of sight | **> 12 m** (vs ~2–3 m with the RF switch left in its power-on state) |
| Sleep current | <!-- TODO: µA, PPK2 --> |
| Current during a ranging burst | <!-- TODO: mA --> |
| Current during Find-Me feedback | <!-- TODO: mA --> |
| Estimated battery life | <!-- TODO: from the above + 250 mAh --> |
| Ranging accuracy | <!-- TODO: ± cm, over N measurement series --> |

<!-- TODO: PPK2 screenshot; distance-vs-truth plot -->

---

## 7. Status

| Module | Status |
|---|---|
| Board bring-up (XIAO nRF54L15 Sense), incl. RF path fix | ✅ Working |
| Channel Sounding ranging (tag ↔ nRF54L15 DK) | ✅ Working — distance values over RAS |
| Custom GATT profile — button, outputs, find-phone, fall | ✅ Working |
| IMU on-chip detection — wake-up, double tap, free fall, inactivity | ✅ Working — interrupt-driven |
| Tap gesture → find phone, free-fall burst → alarm | ✅ Working |
| IMU readout (polled, filtered, logged to serial) | ✅ Working |
| Ranging accuracy characterised against ground truth | 🟡 Not measured yet |
| Buzzer / vibration output | 🟡 Pulse and blink patterns work; pins not wired, no PWM tones |
| MCU-side power state machine (sleep, wake, adv tuning) | 🔜 Planned — the sensor sleeps itself, the CPU does not |
| Power profiling with PPK2 | 🔜 Planned — needs the power state machine first |
| Secure boot + signed OTA (MCUboot) | 🔜 Planned |
| TinyML gesture recognition | 🔜 Planned |
| NLOS detection / correction | 🔜 Planned |
| Custom PCB | 🔜 Planned |

---

## 8. Known limitations

- **Reboot on disconnect.** The tag calls `sys_reboot(SYS_REBOOT_COLD)` when the link drops, inherited from the upstream Channel Sounding sample. It works around connection teardown state but is incompatible with the low-power design and has to go.
- **Latent memory corruption.** An MPU fault inside `net_buf` pool allocation appears once a GPIO interrupt is enabled, always at the same instruction but with a fault address that moves between builds — the pool's buffer pointer reads back as garbage. Adding unrelated logging made it stop reproducing, and rearranging the source into modules brought it back, so it is a layout-sensitive corruption rather than anything the surrounding code does. One capture showed the core in LOCKUP after a double fault, meaning the fault handler itself could not run, which points at a corrupted stack. Prime suspect is a stack overflow on the system work queue, which runs both `bt_gatt_notify()` and `bt_le_adv_update_data()`. Not yet diagnosed.
- **Advertising is not power-aware.** Advertising runs continuously at a fast interval; the slow-interval parameters in the source are unused.

---

## 9. Roadmap

**Secure update chain** — MCUboot as immutable bootloader with ED25519 signature verification, public key held in the nRF54L15 hardware KMU; firmware delivered over the air via MCUmgr/SMP over BLE, dual-slot with rollback. Signing automated in CI.

**Edge AI layer** — gesture recognition on the IMU with Edge Impulse, running inference on-device so a gesture can trigger ranging or silence the alarm without a phone.

**NLOS detection** — Channel Sounding overestimates range when the direct path is blocked and the signal arrives via reflections. Detecting non-line-of-sight conditions and flagging or correcting those readings is the difference between a demo and something usable indoors.

**Custom PCB** — once firmware and power behaviour are settled, move from perfboard to a purpose-built board with a proper antenna layout.

---

## 10. Build

Tag / reflector:

```bash
west build -b xiao_nrf54l15/nrf54l15/cpuapp -p always
west flash
```

`-p always` is needed whenever a new devicetree overlay or board config file is added — CMake only collects those at configure time.

<!-- TODO: board target and any overlays/snippets for the locator -->

**Tooling:** nRF Connect SDK · Zephyr · west · CMake · nRF Connect for Desktop · Power Profiler Kit II · logic analyser, oscilloscope

---

## License

To be added.
