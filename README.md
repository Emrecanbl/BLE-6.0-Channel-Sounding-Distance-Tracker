# EmreTag — BLE 6.0 Channel Sounding Distance Tracker

A battery-powered tracker that measures the **real distance** between two nRF54L15 devices using **Bluetooth 6.0 Channel Sounding**, and guides the user to the tag with distance-aware haptic and acoustic feedback.

> **Status:** In development. BLE base (custom GATT profile, notifications) running on the nRF54L15. Channel Sounding ranging in integration.

Built on **Zephyr RTOS** with the nRF Connect SDK.

---

## 1. Why Channel Sounding

Most BLE trackers estimate distance from **RSSI** — signal strength. That works badly: RSSI drops with obstacles, body shadowing and orientation, so "weak signal" and "far away" are not the same thing. In practice RSSI-based trackers can only tell you *warmer* or *colder*, and they lie whenever something is in the way.

**Channel Sounding** (introduced in Bluetooth 6.0) measures distance directly. Two devices — an **initiator** and a **reflector** — exchange tones across many frequencies and derive range from phase and timing, rather than from how loud the signal is. The result is a distance in metres instead of a signal-strength guess.

This project exists to build that pipeline end to end on real hardware, on a battery budget, rather than on a bench with a debugger attached.

---

## 2. System overview

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
| Tag / reflector | **Seeed XIAO nRF54L15 Sense** | on-board **LSM6DS3TR-C** IMU used for wake-on-motion |
| Locator / initiator | **nRF54L15 DK** | development-side counterpart |
| Prototype build | Perfboard | tag-side carrier for battery, buzzer and vibration motor |

**Tag peripherals**

| Function | Detail |
|---|---|
| Battery | ~250 mAh LiPo |
| Acoustic feedback | Buzzer, PWM-driven |
| Haptic feedback | Vibration motor |
| Motion sensing | LSM6DS3TR-C, wake-on-motion interrupt |

> This is a **firmware and RF project**, not a PCB project — the hardware is a module-based prototype on perfboard. A custom PCB is a later stage, once the firmware and power behaviour are settled.

<!-- TODO: buzzer part number + drive circuit (direct GPIO / MOSFET / driver IC) -->
<!-- TODO: vibration motor part + driver -->
<!-- TODO: photo of the assembled prototype -->

---

## 4. Power architecture

On a 250 mAh cell, the interesting engineering is not the radio — it is **staying asleep**. The tag uses a tiered wake-up scheme instead of running continuously:

1. **Deep sleep** — radio and CPU down, IMU running autonomously
2. **Motion trigger** — LSM6DS3TR-C wake-on-motion interrupt brings the system up; the MCU is not involved in detecting motion
3. **Connectable / ranging** — BLE active, Channel Sounding bursts on demand
4. **Find-Me** — buzzer and vibration motor active; the highest-current state by a wide margin

The buzzer and vibration motor draw far more peak current than the radio ever does, so the feedback pattern is part of the power budget, not an afterthought. Energy behaviour of each state is profiled with a **Nordic Power Profiler Kit II (PPK2)**.

---

## 5. Firmware

**Zephyr RTOS** on the nRF Connect SDK.

- Distance service: custom **GATT profile**, live values pushed via **notify**
- Channel Sounding: initiator and reflector roles
- Motion-driven power state machine (sensor interrupt → wake → advertise/range → back to sleep)
- Find-Me output: PWM buzzer patterns and vibration profiles
- Devicetree/Kconfig-based board configuration for both roles

---

## 6. Measurements

Numbers are only claimed here once they have been measured on hardware.

| Metric | Value |
|---|---|
| Sleep current | <!-- TODO: µA, PPK2 --> |
| Current during a ranging burst | <!-- TODO: mA --> |
| Current during Find-Me feedback | <!-- TODO: mA --> |
| Estimated battery life | <!-- TODO: from the above + 250 mAh --> |
| Ranging accuracy | <!-- TODO: ± cm, over N measurement series --> |
| Usable range | <!-- TODO: m, line of sight --> |

<!-- TODO: PPK2 screenshot; distance-vs-truth plot -->

---

## 7. Status

| Module | Status |
|---|---|
| BLE base — custom GATT profile, notifications | ✅ Working |
| Board bring-up (XIAO nRF54L15 Sense) | ✅ Working |
| Channel Sounding ranging | 🟡 In integration |
| IMU wake-on-motion power state machine | 🟡 In progress |
| Buzzer / vibration Find-Me output | 🟡 In progress |
| Power profiling with PPK2 | 🟡 In progress |
| Secure boot + signed OTA (MCUboot) | 🔜 Planned |
| TinyML gesture recognition | 🔜 Planned |
| NLOS detection / correction | 🔜 Planned |
| Custom PCB | 🔜 Planned |

---

## 8. Roadmap

**Secure update chain** — MCUboot as immutable bootloader with ED25519 signature verification, public key held in the nRF54L15 hardware KMU; firmware delivered over the air via MCUmgr/SMP over BLE, dual-slot with rollback. Signing automated in CI.

**Edge AI layer** — gesture recognition on the IMU with Edge Impulse, running inference on-device so a gesture can trigger ranging or silence the alarm without a phone.

**NLOS detection** — Channel Sounding overestimates range when the direct path is blocked and the signal arrives via reflections. Detecting non-line-of-sight conditions and flagging or correcting those readings is the difference between a demo and something usable indoors.

**Custom PCB** — once firmware and power behaviour are settled, move from perfboard to a purpose-built board with a proper antenna layout.

---

## 9. Build

```bash
west build -b <board> -p always
west flash
```

<!-- TODO: exact board targets for tag and locator, plus any required overlays/snippets -->

**Tooling:** nRF Connect SDK · Zephyr · west · CMake · nRF Connect for Desktop · Power Profiler Kit II · logic analyser, oscilloscope

---

## License

To be added.
