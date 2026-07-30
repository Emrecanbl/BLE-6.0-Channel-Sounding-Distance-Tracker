# EmreTag — BLE 6.0 Channel Sounding Distance Tracker

A battery-powered tracker that measures the **real distance** between two nRF54L15 devices using **Bluetooth 6.0 Channel Sounding**, and guides the user to the tag with distance-aware haptic and acoustic feedback.

> **Status:** In development. Channel Sounding ranging works between the tag and an nRF54L15 DK acting as locator — distance values are produced over the air. The tag also exposes a custom GATT profile for its button and its LED / buzzer / vibration outputs. Everything in [Power architecture](#4-power-architecture) is still design intent, not running code.

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
| Acoustic feedback | Buzzer | driven as plain GPIO on/off; PWM tone patterns planned |
| Haptic feedback | Vibration motor | plain GPIO on/off |
| Motion sensing | LSM6DS3TR-C | polled at 104 Hz over I²C; wake-on-motion interrupt planned |

Both the buzzer and the vibration motor are switched from GATT today, but their pins are still placeholders in firmware and are not yet wired to the prototype.

> This is a **firmware and RF project**, not a PCB project — the hardware is a module-based prototype on perfboard. A custom PCB is a later stage, once the firmware and power behaviour are settled.

### RF path

The XIAO nRF54L15 routes the radio through an RF switch that selects between the on-board ceramic antenna and the external u.FL connector. Neither switch node is enabled at boot by the board devicetree, and the upstream Channel Sounding samples are unaware of it, so out of the box the RF path is left undefined and the link budget suffers. The tag firmware powers the switch and selects the ceramic antenna explicitly before starting the Bluetooth stack — see [Measurements](#6-measurements) for what that was worth.

<!-- TODO: buzzer part number + drive circuit (direct GPIO / MOSFET / driver IC) -->
<!-- TODO: vibration motor part + driver -->
<!-- TODO: photo of the assembled prototype -->

---

## 4. Power architecture

> **Not implemented yet.** This section describes the intended design. The firmware currently keeps the CPU awake, advertises continuously at a fast interval, and polls the IMU — none of the states below exist in code.

On a 250 mAh cell, the interesting engineering is not the radio — it is **staying asleep**. The tag will use a tiered wake-up scheme instead of running continuously:

1. **Deep sleep** — radio and CPU down, IMU running autonomously
2. **Motion trigger** — LSM6DS3TR-C wake-on-motion interrupt brings the system up; the MCU is not involved in detecting motion
3. **Connectable / ranging** — BLE active, Channel Sounding bursts on demand
4. **Find-Me** — buzzer and vibration motor active; the highest-current state by a wide margin

The buzzer and vibration motor draw far more peak current than the radio ever does, so the feedback pattern is part of the power budget, not an afterthought. Energy behaviour of each state will be profiled with a **Nordic Power Profiler Kit II (PPK2)**.

---

## 5. Firmware

**Zephyr RTOS** on the nRF Connect SDK.

**Running today**

- **Channel Sounding ranging** against an nRF54L15 DK: the tag takes the reflector role, applies CS default settings and procedure parameters on connection, and serves ranging data through the **Ranging Service (RAS)** responder, so the locator gets real distance values
- Custom **GATT profile** over 128-bit UUIDs: button state (read + notify), and LED, buzzer and vibration motor as writable on/off characteristics, each named with a User Description descriptor
- Board I/O straight off devicetree — `DT_ALIAS(sw0)` / `DT_ALIAS(led0)` with `gpio_dt_spec`, GPIO interrupt for the button, work queue hand-off so no Bluetooth call runs in ISR context
- RF switch brought up and the ceramic antenna selected before `bt_enable()`
- IMU sampling: LSM6DS3TR-C polled in its own thread, accelerometer and gyroscope logged over the serial console; the shared PDM/IMU supply rail is enabled from a `SYS_INIT` hook that runs before the sensor driver probes the chip

**Not yet**

- Distance-aware behaviour on the tag itself: ranging data is served to the locator, but the tag does not act on distance (no Find-Me feedback tied to range)
- Motion-driven power state machine
- PWM buzzer patterns and vibration profiles
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
| Custom GATT profile — button, LED, buzzer, vibration | ✅ Working |
| IMU readout (polled, logged to serial) | ✅ Working |
| Ranging accuracy characterised against ground truth | 🟡 Not measured yet |
| Buzzer / vibration Find-Me output | 🟡 Switchable over GATT; pins not wired, no PWM patterns |
| IMU wake-on-motion power state machine | 🔜 Planned — IMU is polled today |
| Power profiling with PPK2 | 🔜 Planned — needs the power state machine first |
| Secure boot + signed OTA (MCUboot) | 🔜 Planned |
| TinyML gesture recognition | 🔜 Planned |
| NLOS detection / correction | 🔜 Planned |
| Custom PCB | 🔜 Planned |

---

## 8. Known limitations

- **Reboot on disconnect.** The tag calls `sys_reboot(SYS_REBOOT_COLD)` when the link drops, inherited from the upstream Channel Sounding sample. It works around connection teardown state but is incompatible with the low-power design and has to go.
- **Latent memory corruption.** An MPU fault inside `net_buf` pool allocation appeared once the button GPIO was initialised, with a fault address that moved between builds. It stopped reproducing after unrelated logging was added, which means it is hidden rather than fixed. Prime suspect is a thread stack overflow on the system work queue, which runs both `bt_gatt_notify()` and `bt_le_adv_update_data()`. Not yet diagnosed.
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
