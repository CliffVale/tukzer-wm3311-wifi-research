# Tukzer WM3311 (UZ801 v3.0) — WCN3620 WiFi Firmware Crash Research

> **Device**: Tukzer WM3311 / UZ801 v3.0 — MSM8916 SoC, 384MB RAM, 4GB eMMC  
> **OS**: OpenWrt 25.12.5 (Linux 6.12.94, aarch64) — from [hkfuertes/msm8916-openwrt](https://github.com/hkfuertes/msm8916-openwrt)  
> **WiFi Chip**: WCN3620 (802.11 b/g/n 2.4GHz only)  
> **Problem**: WCNSS firmware crashes with `dog.c:1676:Watchdog detects task starvation` every ~63 seconds after boot.  
> **Status**: Root cause identified. No fix deployed yet.

---

## Table of Contents

1. [Device Overview](#1-device-overview)
2. [The Crash in Detail](#2-the-crash-in-detail)
3. [Experiments & Findings](#3-experiments--findings)
4. [Root Cause Analysis](#4-root-cause-analysis)
5. [Driver Gaps: What mainline wcn36xx is missing for WCN3620](#5-driver-gaps)
6. [Historical Context](#6-historical-context)
7. [Approach for Fix](#7-approach-for-fix)
8. [References](#8-references)

---

## 1. Device Overview

### Hardware

| Component | Detail |
|-----------|--------|
| SoC | Qualcomm MSM8916 (Snapdragon 410) — 4× Cortex-A53 @ 1.2 GHz |
| RAM | 384 MB |
| Storage | 4 GB eMMC |
| Modem | Qualcomm MDM9x07 (Chinese variant) |
| **WiFi** | **WCN3620** (2.4 GHz only, 802.11 b/g/n) |
| USB | USB 2.0 — gadget modes (NCM/RNDIS/ACM/Mass Storage) |

### Partition Layout (GPT)

| Partition | Size | Content |
|-----------|------|---------|
| p3 (modem) | 64 MB | FAT16 — CMNLIB firmware + WCNSS firmware blobs |
| p6 (persist) | 32 MB | Raw EFS / calibration (contains WLAN NV) |
| p12 (aboot) | 1 MB | ELF with qtestsign certs |

### Firmware Files

- **WCNSS firmware**: `wcnss.b00`–`wcnss.b11` + `wcnss.mdt` — from stock modem partition, byte-identical to EDL backup
- **WLAN NV calibration**: `/lib/firmware/wlan/prima/WCNSS_qcom_wlan_nv.bin` (29816 B) — from stock persist partition
- **Modem firmware**: `modem.b00`–`modem.b27` + `modem.mdt` — from stock modem partition

### LEDs

| LED | GPIO | Notes |
|-----|------|-------|
| Red | GPIO7 | Power indicator — solid when booted |
| Green | GPIO8 | WAN — lit when USB gadget active |
| Blue | GPIO6 | WLAN — intended for WiFi status |

---

## 2. The Crash in Detail

### Crash Signature

```
qcom-wcnss-pil a204000.remoteproc: fatal error received: dog.c:1676:Watchdog detects task starvation
remoteproc remoteproc1: crash detected in a204000.remoteproc: type fatal error
remoteproc remoteproc1: handling crash #N in a204000.remoteproc
remoteproc remoteproc1: recovering a204000.remoteproc
```

- **Source**: WCNSS firmware internal watchdog (`dog.c:1676`) — **not** a host kernel crash
- **Nature**: Firmware task starvation — a task inside the firmware blocks for ~60 seconds
- **Host reaction**: Auto-recovery — old WCNSS instance stopped, new one booted automatically

### Timing Analysis

| Boot | WCNSS "up" time | Crash time | Active interval |
|------|-----------------|------------|-----------------|
| #1   | 11.52 s         | 74.26 s    | **62.7 s** |
| #2   | 80.23 s         | 142.99 s   | **62.8 s** |
| #3   | 149.12 s        | 211.87 s   | **62.8 s** |
| #4   | 217.82 s        | 280.58 s   | **62.8 s** |
| ...  | ...             | ...        | **~62.8 s each** |
| #14  | 914.99 s        | 977.75 s   | **62.8 s** |

**Pattern**: Deterministic — always ~63 seconds between firmware version report and watchdog crash. Not affected by host load, not random.

### Crash Flow Diagram

```
WCNSS firmware boots (via PIL/remoteproc)
  │
  ├── Firmware reports version (WCNSS Version 1.5 1.2)
  ├── wcn36xx downloads NV calibration (req 55)
  │     └── Succeeds on reboot, fails on cold boot (modem interferes)
  │
  ├── Firmware enters idle mode (no WiFi interface started)
  │
  ├── [~60s timer fires] → firmware task does something → BLOCKS
  │
  ├── Firmware watchdog task (dog.c) detects no progress
  │     └── Fires watchdog
  │           └── WCNSS hardware watchdog interrupt on host
  │                 └── wcnss_wdog_interrupt() → rproc_report_crash()
  │                       └── Remoteproc: crash #N, recovering...
  │
  └── Auto-recovery → goto start
```

### Dmesg Excerpt (First Boot — Cold)

```
[   11.520] wcn36xx: mac address: 02:00:c6:15:3d:10        ← WCNSS booted
[   11.973] qcom-wcnss-pil: unexpected response to sysmon event  ← Modem signals SSR
[   25.812] wcn36xx: ERROR Timeout! No SMD response to req 55   ← NV upload blocked
[   36.052] wcn36xx: ERROR Timeout! No SMD response to req 55   ← Retry also fails
[   74.258] remoteproc remoteproc1: handling crash #1            ← Watchdog fires
```

### Dmesg Excerpt (Typical Recovery Cycle)

```
[  t+0] remoteproc: recovering a204000.remoteproc
[  t+5] remoteproc: remote processor is now up
[  t+6] WCNSS Version 1.5 1.2
[  t+6] mac address: 02:00:c6:15:3d:10
[t+69] fatal error: dog.c:1676:Watchdog detects task starvation
[t+69] crash #N in a204000.remoteproc
```

**Full logs**: See [logs/device-dump.txt](logs/device-dump.txt), [logs/crash-pattern.txt](logs/crash-pattern.txt).

---

## 3. Experiments & Findings

### Experiment 1: Stop the Modem (Mid-Session)

**Hypothesis**: The `"unexpected response to sysmon event"` — when the modem boots and sends an SSR (subsystem restart) event to the WCNSS firmware via rpmsg — causes the firmware to enter a bad state.

**Method**: Stopped the modem remoteproc (`echo stop > /sys/class/remoteproc/remoteproc0/state`), then monitored WCNSS behavior over the next few crash cycles.

**Result**: ❌ **Hypothesis disproved.** WCNSS continued crashing at exactly the same ~63 s interval with the modem offline.

| Crash | Modem state | Crash time | Interval from previous |
|-------|-------------|------------|----------------------|
| #12 | Running | 831.19 s | 68.9 s |
| #13 | **Stopped** (at 833.56 s) | 901.11 s | 69.9 s |
| #14 | **Stopped** | 977.75 s | 76.6 s (includes recovery) |

**Conclusion**: The modem sysmon event is a red herring — not the root cause.

### Experiment 2: SSCTL Timeout Analysis

**Observation**: On some recovery cycles, the message `"timeout waiting for ssctl service"` appears, immediately followed by WCNSS being stopped and restarted a second time.

**Finding**: This is `sysmon_stop()` in `qcom_sysmon.c` — a 500 ms timeout (`HZ/2`) waiting for the SSCTL QMI service to appear on the freshly-booted firmware before sending a graceful shutdown. It occurs during crash recovery and is a **consequence** of the crash, not a cause.

### Experiment 3: WCN3620 Chip Identification in Driver

**Method**: Checked `iw list` for available bands (2.4 GHz only → WCN3620 confirmed). Analyzed `wcn36xx.ko` module strings for chip type handling.

**Finding**: The mainline `wcn36xx` kernel module does **not** contain `WCN36XX_CHIP_3620`. The chip detection logic (`wcn36xx_detect_chip_version()`) only knows WCN3660 and WCN3680. For WCN3620 (no 802.11ac), it falls through to WCN3660 mode, which is **incorrect**.

Consequences of this misidentification:
- Wrong DXE register layout (needs WCN3680 layout, gets WCN3660)
- Power saving (BMPS) not disabled (WCN3620 BMPS is unstable)
- `WCN36XX_HAL_AVOID_FREQ_RANGE_IND` message type 233 not handled
- WCN3620-specific trigger_BA response format not recognized

---

## 4. Root Cause Analysis

### Primary Finding: WCN3620 × Mainline wcn36xx Incompatibility

The WCN3620 is a Wi-Fi + Bluetooth combo chip that was **never properly supported** by the mainline `wcn36xx` driver. The driver was written for WCN3660 and WCN3680. WCN3620 support was attempted via patches in 2015 but never fully merged upstream.

**The crash at ~63 s is the WCN3620 firmware's internal watchdog firing because its main task blocks** — likely waiting for a response or signal from the host that the mainline driver never sends.

### Key Evidence

| # | Evidence | Source |
|---|----------|--------|
| 1 | **Deterministic timing** — always ~63 s, regardless of modem, activity, or host load | Dmesg analysis across 14+ crashes |
| 2 | **Firmware-origin crash** — message comes from firmware itself (`dog.c:1676`) | Crash reason SMEM |
| 3 | **Known since 2014** — WCN3620 incompatibility documented from the beginning | [Mailing list](http://lists.infradead.org/pipermail/wcn36xx/2014-December/001397.html) |
| 4 | **Driver missing WCN3620 support** — no CHIP_3620 constant in running module | `strings` analysis of wcn36xx.ko |
| 5 | **Existing fix patches** — msm8916-mainline project has working patches | [GitHub](https://github.com/msm8916-mainline/linux) |
| 6 | **2026 known limitation** — "wcn36xx misclassifies 2.4 GHz as 5 GHz on WCN3620" | [Kernel patch series](https://lists.openwall.net/linux-kernel/2026/06/01/630) |

---

## 5. Driver Gaps

What the mainline `wcn36xx` driver (kernel 6.12.94) is missing for WCN3620:

| Gap | Detail | Impact | Reference |
|-----|--------|--------|-----------|
| **WCN36XX_CHIP_3620** | No 3620 chip type constant — detected as WCN3660 | Wrong code paths activated | [Patch 1/7](https://www.spinics.net/lists/netdev/msg312687.html) |
| **DXE register layout** | WCN3620 needs WCN3680-style DXE regs | Possible TX/RX DMA issues | [dxe.c patch](https://www.spinics.net/lists/netdev/msg312687.html) |
| **Power saving (BMPS)** | Must be disabled for WCN3620 | BMPS causes firmware hangs | [Patch 6/7](https://lists.openwall.net/netdev/2015/02/09/153) |
| **AVOID_FREQ_RANGE_IND** | Message type 233 not handled — firmware sends it asynchronously | Driver ignores → firmware may block | [hal.h patch](https://www.spinics.net/lists/netdev/msg312687.html) |
| **Trigger BA response** | WCN3620 uses different trigger_ba format | BA session state corruption | [trigger_ba patch](https://www.spinics.net/lists/netdev/msg312687.html) |
| **RF band=0 packets** | WCN3620 firmware sometimes sends rf_band=0 → interpreted as 5 GHz | Kernel warning, invalid channel | [cc4abc6](https://github.com/msm8916-mainline/linux/commit/cc4abc694fcf2c942410136bc58a61e79bf21e83) |
| **Scan band classification** | 2.4 GHz networks misclassified as 5 GHz | Can't associate with APs | Known issue (2026) |

### Current Module Capabilities (from strings analysis)

```
wcn36xx.ko (kernel 6.12.94):
  ✔ Recognizes DT compatibles: qcom,wcn3620, qcom,wcn3660, qcom,wcn3660b, qcom,wcn3680
  ✔ Downloads NV calibration to firmware
  ✔ Handles WCN36XX_HAL_DOWNLOAD_NV_RSP (correctly at line 3269 of smd.c)
  ✗ No WCN36XX_CHIP_3620 constant
  ✗ No "Chip is 3620" info message
  ✗ No WCN36XX_HAL_AVOID_FREQ_RANGE_IND handler
  ✗ No BMPS disable for WCN3620
```

---

## 6. Historical Context

The WCN3620 + mainline Linux saga spans over a decade:

| Date | Event |
|------|-------|
| **2014-12** | Andy Green reports WCN3620 unsupported by mainline wcn36xx on the [wcn36xx mailing list](http://lists.infradead.org/pipermail/wcn36xx/2014-December/001397.html). Firmware returns error 233 on NV download. |
| **2015-02** | Green posts [7-patch series](https://www.spinics.net/lists/netdev/msg312687.html) adding basic WCN3620 support. Blocked from merging by lack of mainline PIL (remoteproc) support. |
| **2015-02** | Bjorn Andersson reports same crash pattern on WCN3680, notes it's a generic mainline issue, not WCN3620-specific. |
| **2016-03** | Bjorn adds mainline WCNSS PIL support. WCN3620 patches still not merged. |
| **2016-04** | "wcn36xx fixes v3" series — Bjorn fixes BSS deletion ordering "as this crashes the firmware on Dragonboard 410c (apq8016 with pronto & wcn3620)". |
| **2020** | [msm8916-mainline/linux](https://github.com/msm8916-mainline/linux) project starts collecting device-specific fixes for MSM8916. |
| **2022-03** | `cc4abc6` — WCN3620-specific fix for rf_band=0 scan packets added to msm8916-mainline. |
| **2023** | [hkfuertes/msm8916-openwrt](https://github.com/hkfuertes/msm8916-openwrt) project provides OpenWrt builds for these devices. |
| **2026-04** | "wcn36xx: fix OOB read from short trigger BA firmware response" — fixes still being posted. |
| **2026-06** | MSM8960 WCNSS enablement patch series cites known limitation: *"The wcn36xx driver appears to misclassify 2.4 GHz networks as 5 GHz during hardware scanning"* on WCN3620. |

---

## 7. Approach for Fix

### Phase 1: Confirm Hypothesis (Next Step — No Tools Required)

**Test**: Restart WCNSS, then immediately create wlan0 and bring it up within 30 seconds (before the ~60 s firmware timer fires).

```bash
# After WCNSS boots (wait for "WCNSS Version 1.5 1.2" in dmesg):
iw phy phyX interface add wlan0 type managed
ip link set wlan0 up
```

If the firmware timer is waiting for `HAL_START_REQ` (sent by `wcn36xx_start()` when the interface comes up), this will prevent the watchdog crash. If it still crashes, the issue is deeper (firmware timer waiting for something else).

### Phase 2: Fix Options (Ranked by Feasibility)

| Option | Effort | What | Risk |
|--------|--------|------|------|
| **A. Patch wcn36xx.ko + deploy** | Low | Add WCN3620 patches, rebuild module, scp to device | Low — module can be hot-replaced |
| **B. Build full OpenWrt kernel** | Medium | Rebuild with msm8916-mainline patches applied | Medium — full flash required |
| **C. Patch qcom_wcnss_pil.ko** | Low | Skip sysmon registration (prevent "unexpected response") | Low — workaround only |
| **D. Find alternate firmware** | High | Source WCNSS firmware from a known-working device | High — may not exist |
| **E. Upstream fix** | Very High | Write proper WCN3620 support and contribute | Very High — but fixes it for everyone |

### Phase 3: Modem & Network (After WiFi Works)

1. Source Indian carrier `mcfg_sw.mbn` for cellular registration
2. Insert SIM, test with ModemManager
3. Connect to IITD_WIFI (WPA2-Enterprise PEAP/MSCHAPv2)

---

## 8. References

### Kernel Source Code

| File | Purpose |
|------|---------|
| [wcn36xx/main.c](https://github.com/torvalds/linux/blob/master/drivers/net/wireless/ath/wcn36xx/main.c) | Main driver entry, chip detection |
| [wcn36xx/smd.c](https://github.com/torvalds/linux/blob/master/drivers/net/wireless/ath/wcn36xx/smd.c) | SMD message handling, NV download |
| [wcn36xx/dxe.c](https://github.com/torvalds/linux/blob/master/drivers/net/wireless/ath/wcn36xx/dxe.c) | Data path engine (TX/RX DMA) |
| [wcn36xx/hal.h](https://github.com/torvalds/linux/blob/master/drivers/net/wireless/ath/wcn36xx/hal.h) | HAL message type definitions |
| [qcom_wcnss.c](https://github.com/jcarp10/android_kernel_oneplus6/blob/development/drivers/remoteproc/qcom_wcnss.c) | WCNSS PIL — watchdog/fatal interrupt handlers |
| [wcnss_ctrl.c](https://github.com/torvalds/linux/blob/master/drivers/soc/qcom/wcnss_ctrl.c) | WCNSS control channel — NV upload |
| [qcom_sysmon.c](https://github.com/torvalds/linux/blob/master/drivers/remoteproc/qcom_sysmon.c) | Sysmon — SSCTL QMI service, SSR handling |

### Projects

| Project | URL |
|---------|-----|
| hkfuertes/msm8916-openwrt | https://github.com/hkfuertes/msm8916-openwrt |
| msm8916-mainline/linux | https://github.com/msm8916-mainline/linux |
| msm8953-mainline/linux | https://github.com/msm8953-mainline/linux |
| OpenStick | https://github.com/OpenStick/OpenStick |
| postmarketOS MSM8916 | https://wiki.postmarketos.org/wiki/MSM8916_Mainlining |

### Mailing List Archives

| Thread | Year | Link |
|--------|------|------|
| "msm8916 + wcn3620 mainline support?" | 2014 | [wcn36xx list](http://lists.infradead.org/pipermail/wcn36xx/2014-December/001397.html) |
| "[PATCH 0/7] add basic wcn3620 support" | 2015 | [netdev](https://www.spinics.net/lists/netdev/msg312687.html) |
| "Remove powersaving for wcn3620" | 2015 | [netdev](https://lists.openwall.net/netdev/2015/02/09/153) |
| "wcn36xx fixes v3" (BSS delete fix) | 2016 | [LKML](https://lkml.indiana.edu/1604.2/01732.html) |
| "SSCTL service wait fix" | 2022 | [LKML](https://lkml.indiana.edu/hypermail/linux/kernel/2208.1/08308.html) |
| "MSM8960 WCNSS enablement" | 2026 | [openwall](https://lists.openwall.net/linux-kernel/2026/06/01/630) |

### Key Commits

| Commit | Description |
|--------|-------------|
| [cc4abc6](https://github.com/msm8916-mainline/linux/commit/cc4abc694fcf2c942410136bc58a61e79bf21e83) | wcn36xx: txrx: Ignore 5 GHz scan packets on WCN3620 |
| [779c962](https://github.com/msm8953-mainline/linux/commit/779c9627ec0b971bf466588e64fe530cf78a414d) | Same fix for MSM8953 |
| [3244442](https://github.com/AMDESE/linux/commit/3244442406ff49e8f75a1f2def211c497710570f) | arm64: dts: qcom: msm8916: Move WCN compatible to boards |
| [47c04e0](https://lkml.indiana.edu/hypermail/linux/kernel/2208.1/08308.html) | remoteproc: sysmon: Wait for SSCTL service to come up |

### OpenWrt Forum

- [UF896 / MSM8916 LTE router thread](https://forum.openwrt.org/t/uf896-qualcomm-msm8916-lte-router-384mib-ram-2-4gib-flash-android-openwrt/131712)

---

## Directory Structure

```
tukzer-wm3311-wifi-research/
├── README.md               ← This file
├── logs/
│   ├── device-dump.txt     ← Full dmesg + device info dump
│   ├── crash-pattern.txt   ← Extracted crash cycle timestamps
│   ├── current-state.txt   ← Latest device state
│   └── iw-list.txt         ← iw list output (WCN3620: 2.4GHz only)
```

---

*Documentation — freely reusable. Use at your own risk.*

*Last updated: 2026-07-28*
