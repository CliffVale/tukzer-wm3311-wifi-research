# WCN3620 WiFi Troubleshooting — Tukzer WM3311 / UZ801 v3 on OpenWrt

## 1. DEVICE IDENTITY

| Field | Value |
|-------|-------|
| Model | uz801 v3.0 4G Modem Stick |
| Compatible | yiming,uz801-v3 qcom,msm8916 |
| SoC | Qualcomm MSM8916 (4x Cortex-A53 @ 1.2GHz) |
| WiFi Chip | WCN3620 (QCA WCN3610 family) — detected via DT iris node |
| RAM | 512MB (likely) |
| Flash | 8GB eMMC (likely) |

The device is the **Tukzer WM3311** (also branded as **UZ801 v3**). It's an MSM8916-based 4G LTE dongle/ router stick with built-in WiFi (WCN3620), Bluetooth, and a Qualcomm modem.

---

## 2. SOFTWARE STACK

| Layer | Version |
|-------|---------|
| OS | OpenWrt 25.12.5 (r33051-f5dae5ece4) |
| Kernel | 6.12.94 #0 SMP PREEMPT (aarch64) |
| GCC | OpenWrt GCC 14.3.0 (aarch64-openwrt-linux-musl-gcc) |
| Target | msm89xx/msm8916 |
| Arch | aarch64_generic |
| Build date | Mon Jun 29 12:59:20 2026 |

### Loaded WiFi-related Modules

```
cfg80211              352256  2 wcn36xx,mac80211
compat                 12288  3 wcn36xx,mac80211,cfg80211
mac80211              679936  1 wcn36xx
wcn36xx                86016  0
qcom_wcnss_pil         16384  0
```

### Kernel Config (relevant items)

```
# CONFIG_MODVERSIONS is not set   ← CRC symbol versioning OFF
# CONFIG_CFG80211 is not set      ← But cfg80211.ko is loaded as module
# CONFIG_WLAN_VENDOR_ATH is not set  ← ath modules built outside kernel tree
```

The wcn36xx driver and mac80211/cfg80211 are **built as external modules** (OpenWrt package system), not built into the kernel image. The `compat` module provides a compatibility layer.

---

## 3. THE PROBLEM

### Symptom

WiFi does not work. The wcn36xx driver sends `WCN36XX_HAL_START_REQ` (message ID 55) but the firmware **never responds**, causing a 10-second timeout:

```
[   25.811777] wcn36xx: ERROR Timeout! No SMD response to req 55 in 10000ms
[   25.811929] wcn36xx: ERROR Failed to push NV to chip
```

After the timeout, the firmware watchdog (running on the WCNSS RIVA processor) fires because the WLAN subsystem was never properly initialized:

```
[   74.248633] qcom-wcnss-pil a204000.remoteproc: fatal error received: dog.c:1676:Watchdog detects task starvation
```

This causes a WCNSS crash + recovery cycle that repeats indefinitely (~63s cycle):

```
wcnss boots → driver probes → MAC read OK → HAL_START_REQ timeout → 
watchdog fires → WCNSS crash → WCNSS recovery → reboot cycle
```

### Timing Analysis

| Event | Time (s) | Delta |
|-------|----------|-------|
| WCNSS firmware boots | 11.13 | - |
| MAC address read (1st) | 11.52 | +0.4s |
| HAL_START_REQ timeout #1 | 25.81 | +14.3s |
| HAL_START_REQ timeout #2 | 36.05 | +10.3s |
| Watchdog fires (crash) | 74.25 | +63.2s (≈2× 31.5s watchdog) |
| WCNSS recovery complete | 79.85 | +5.6s |
| MAC address read (2nd) | 80.23 | +0.4s |

The watchdog timeout is **~63 seconds** from the first HAL_START_REQ attempt. The firmware's internal watchdog (`dog.c:1676`) detects task starvation on the WLAN processing thread because the WLAN MAC was never started.

---

## 4. WHAT WORKS

### SMD Communication ✅

The SMD (Shared Memory Driver) channels between the AP (Cortex-A53) and the WCNSS firmware (RIVA/M3 processor) are fully functional:

- MAC address is read successfully: `wcn36xx: mac address: 02:00:c6:15:3d:10`
- WCNSS_CTRL channel responds with version: `WCNSS Version 1.5 1.2`
- SMD edge channels available:
  - `WCNSS_CTRL` (control)
  - `WLAN_CTRL` (WLAN control)
  - `IPCRTR` (IPC router)
  - `APPS_RIVA_BT_*` (Bluetooth)
  - `APPS_RIVA_ANT_*` (ANT+)
  - `APPS_RIVA_CTRL`, `APPS_RIVA_DATA` (RIVA data)
  - `APPS_FM` (FM radio)

### Remoteproc ✅

```
a204000.remoteproc: running (fw: wcnss.mdt)
4080000.remoteproc: offline (fw: mba.mbn) ← modem, currently not needed
```

### DT Correctness ✅

The Device Tree has the correct `iris` child node under the WCNSS remoteproc:

```
/sys/firmware/devicetree/base/soc@0/remoteproc@a204000/iris/compatible
  → "qcom,wcn3620"
```

The WCNSS node (`remoteproc@a204000`) also has the `qcom,mmio` phandle pointing to the pronto registers, which the wcn36xx driver uses via `wcn36xx_platform_get_resources()`.

### Internet Connectivity ✅

The device has internet access via USB tethering:
```
default via 192.168.1.238 dev br-lan
ping 8.8.8.8 → 41ms RTT
```

---

## 5. FIRMWARE FILES

```
/lib/firmware/wcnss.mdt            7260 B   ← main WCNSS firmware descriptor
/lib/firmware/wcnss.b00             436 B   ← firmware segments
/lib/firmware/wcnss.b01            6824 B
/lib/firmware/wcnss.b02           13052 B
/lib/firmware/wcnss.b04           61440 B
/lib/firmware/wcnss.b06         3367740 B   ← largest segment (3.2 MB)
/lib/firmware/wcnss.b09              52 B
/lib/firmware/wcnss.b10          655360 B
/lib/firmware/wcnss.b11           39796 B

/lib/firmware/wlan/prima/WCNSS_qcom_wlan_nv.bin   29816 B ← NV calibration
```

The firmware files contain WLAN-related symbols in segment b06 (3.2 MB):
```
wlan_bal.c, wlan_phy_main1_tx_clk, wcss_wlan_rfif_clk,
Sending HAL msg to host, LLM_StartLinklessBroadcasts, etc.
```

The WCNSS firmware version is **1.5 1.2** (matches MSM8916 WCNSS standard firmware).

---

## 6. KERNEL SOURCE ANALYSIS

### Source: kernel.org 6.12.94 (linux-6.12.94.tar.xz)

The vanilla kernel.org 6.12.94 kernel **already includes** the WCN3620 patches from the msm8916-mainline project:

| Component | Upstream 6.12.94 | msm8916-mainline 7.0 |
|-----------|-------------------|----------------------|
| rf_id field in `wcn36xx.h` | ✅ `RF_IRIS_WCN3620 = 0x3620` | ✅ same |
| DT iris detection in `main.c` | ✅ `platform_get_resources` reads iris node | ✅ same |
| 5GHz band disable for WCN3620 | ✅ `rf_id != RF_IRIS_WCN3620` | ✅ same |
| cfg_vals selection in `smd.c` | ✅ `rf_id == RF_IRIS_WCN3680` → 3680 cfg, else default | ✅ same |
| DXE regs selection in `dxe.c` | ⚠️ `rf_id == RF_IRIS_WCN3680` (does NOT include WCN3620) | ⚠️ same? |
| `chip_version` usage | ❌ removed entirely | ❌ removed entirely |

**The kernel.org 6.12.94 source is functionally identical to msm8916-mainline for the wcn36xx driver.** There are no additional msm8916-mainline patches that change wcn36xx behavior — the upstream has already absorbed them all.

### What `rf_id` Controls

In `main.c`:
- **Line 1440**: `if (wcn->rf_id != RF_IRIS_WCN3620)` — skips adding 5GHz band (WCN3620 is 2.4GHz only)
- **Line 1443**: `if (wcn->rf_id == RF_IRIS_WCN3680)` — sets VHT capabilities (WCN3620 doesn't support VHT)

In `smd.c`:
- **Line 632**: `if (wcn->rf_id == RF_IRIS_WCN3680)` — selects `wcn3680_cfg_vals` vs `wcn36xx_cfg_vals`

In `dxe.c`:
- **Lines 340, 356, 632, 1512, 1691, 2444**: `if (wcn->rf_id == RF_IRIS_WCN3680)` — selects register offsets/layout

**CRITICAL INSIGHT**: Since `rf_id == RF_IRIS_WCN3620` makes ALL these conditions evaluate to `false`, WCN3620 always falls to the "else/default" path. It gets:
- `wcn36xx_cfg_vals` (default config values) ✅
- Default DXE register layout (same as WCN3660) ✅
- No 5GHz band ✅

So rf_id detection alone **does not explain the HAL_START_REQ timeout**. The default paths are the same whether rf_id is detected (WCN3620) or not (rf_id=0).

---

## 7. MODULE BUILD SETUP

### Cross-compilation Environment

The host machine has a Docker container (`openwrt-builder`) set up with:
- Base: Ubuntu 22.04 (via `arm64v8/ubuntu:22.04`)
- Toolchain: `aarch64-linux-gnu-*` (from `gcc-aarch64-linux-gnu` package)
- Kernel source: `linux-6.12.94.tar.xz` from kernel.org
- Device config: extracted via `zcat /proc/config.gz` from the running device

### Build Commands

```bash
# Extract kernel
cd /tmp && tar xf linux-6.12.94.tar.xz

# Get device config
sshpass -p "456+" ssh root@192.168.1.1 "zcat /proc/config.gz" > .config

# Configure for aarch64
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig

# Build wcn36xx module (modpost warnings are OK — CONFIG_MODVERSIONS is off)
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KBUILD_MODPOST_WARN=1 \
  M=drivers/net/wireless/ath/wcn36xx modules -j$(nproc)
```

The built module (`wcn36xx.ko`, 602 KB) was deployed to the device at `/lib/modules/6.12.94/wcn36xx.ko` (backup saved as `.bak`).

**IMPORTANT**: Because `CONFIG_MODVERSIONS=n` on the device, the CRC symbol versioning is OFF. The module only needs to pass the **vermagic** check:
```
vermagic: 6.12.94 SMP preempt mod_unload aarch64
```

### Vermagic Match ✅
- Device module: `6.12.94 SMP preempt mod_unload aarch64`
- Built module: `6.12.94 SMP preempt mod_unload aarch64`

### Important: dependencies field empty
The new module has `depends:` (empty) because it was built without the mac80211/cfg80211 Module.symvers. The original OpenWrt module has `depends: mac80211,cfg80211,compat`. On OpenWrt (no `depmod`, no `modules.dep`), modules are loaded via `insmod` directly — the dependency info in modinfo is advisory. Since mac80211 and cfg80211 are already loaded at boot, the kernel resolves symbols at runtime.

---

## 8. EXPERIMENTS & FINDINGS

### Experiment 1: HAL_START_REQ at different times
Interacting with `/sys/kernel/debug/ieee80211/*/` or the wcn36xx debugfs to trigger `wcn36xx_start()` at various points in the recovery cycle was attempted. The HAL_START_REQ always timed out regardless of timing.

### Experiment 2: Creating wlan0 and bringing it up
Creating wlan0 via iw and bringing it up (`ip link set wlan0 up`) triggers `wcn36xx_start()` → HAL_START_REQ → timeout → watchdog fires → crash.

### Experiment 3: Kernel module swap
A rebuilt wcn36xx.ko from kernel.org 6.12.94 source (with WCN3620 patches) was deployed. The rf_id detection should now work (reading iris node from DT → RF_IRIS_WCN3620). The module was tested but the HAL_START_REQ timeout persisted.

### Experiment 4: Disabling watchdog
The WCNSS watchdog is internal to the firmware (running on the RIVA processor in `dog.c:1676`). It cannot be disabled from the ARM side. It fires when the WLAN processing thread doesn't respond within ~2× the watchdog interval (~31.5s each, so ~63s total).

### Experiment 5: RPMSG Direct Channel Test (2026-07-28)
A direct RPMSG test was written to verify whether the firmware's WLAN control channel can be opened at the RPMSG (SMD) level, independent of the wcn36xx driver.

**Source**: `/home/cliff/rpmsg_test.c` (cross-compiled to static binary via `aarch64-linux-gnu-gcc`)

**Procedure**:
1. Open `/dev/rpmsg_ctrl1` (WCNSS rpmsg control device)
2. Create endpoints via `RPMSG_CREATE_EPT_IOCTL` for both `WLAN_CTRL` (channel 14) and `WCNSS_CTRL` (channel 1)
3. Attempt to write HAL-style messages to the endpoint file descriptors
4. Monitor SMD channel state transitions

**Results**:
| Step | Result |
|------|--------|
| Open `/dev/rpmsg_ctrl1` | ✅ Success |
| Create WLAN_CTRL endpoint (ch 14) | ✅ Success (ept created) |
| Create WCNSS_CTRL endpoint (ch 1) | ✅ Success (ept created) |
| Write to WLAN_CTRL fd | ❌ `write()` returned EINVAL |
| Write to WCNSS_CTRL fd | ❌ `write()` returned EINVAL |
| SMD channel state | ❌ Never enters OPENED state |

**Conclusion**: The firmware does NOT accept RPMSG channel creation on WLAN_CTRL or WCNSS_CTRL at the RPMSG level. The SMD channel stays in a non-OPENED state, meaning the firmware's WLAN subsystem is either:
1. Not initialized to accept SMD communication on these channels
2. Expecting a different initialization sequence (e.g., proprietary prima driver setup)
3. The firmware image is incompatible with the open-source SMD/RPMSG protocol

This confirms the firmware was designed for the proprietary prima WLAN driver and does not respond to the open-source wcn36xx HAL protocol at any layer.

**File**: `/home/cliff/rpmsg_test` (compiled binary on host), `/home/cliff/rpmsg_test.c` (source)

### Firmware File Anomaly (2026-07-28)

During investigation, a mismatch in firmware file metadata was discovered:

| File | Size | Owner | Timestamp |
|------|------|-------|-----------|
| `wcnss.b00` | 436 B | root:root | Jun 29 (OpenWrt build) |
| `wcnss.mdt` | 7260 B | root:root | Jun 29 (OpenWrt build) |
| `wcnss.b01` | 6824 B | 1000:1000 | Jul 28 (device) |
| `wcnss.b02` | 13052 B | 1000:1000 | Jul 28 (device) |
| `wcnss.b04` | 61440 B | 1000:1000 | Jul 28 (device) |
| `wcnss.b06` | 3.2 MB | 1000:1000 | Jul 28 (device) |
| `wcnss.b09` | 52 B | 1000:1000 | Jul 28 (device) |
| `wcnss.b10` | 655360 B | 1000:1000 | Jul 28 (device) |
| `wcnss.b11` | 39796 B | 1000:1000 | Jul 28 (device) |

`wcnss.b00` and `wcnss.mdt` have OpenWrt build timestamps (part of the firmware-archive package), while all other `.b*` segment files have uid `1000:1000` with Jul 28 timestamp — suggesting they were extracted by `msm-firmware-dumper` on first boot from the device's own modem partition. This is the expected behavior (the OpenWrt firmware-archive package contains only the loader segments; the rest come from the device's stock firmware), but it means the firmware is from the stock Android build, not from a known-working OpenWrt source.

### Root Cause Hypothesis (current best)

**The HAL_START_REQ timeout is NOT caused by the wcn36xx driver or the Linux kernel.** The WCNSS firmware (v1.5 1.2) loads and runs, the SMD channels work, but the firmware's WLAN subsystem does not respond to `WCN36XX_HAL_START_REQ` (message ID 55).

Possible causes:

1. **Wrong firmware version**: The WCNSS firmware from the stock Android build might be incompatible with the prima WLAN HAL protocol version expected by the Linux wcn36xx driver. The firmware might expect a different initialization sequence or message format.

2. **Missing/incorrect NV file**: `WCNSS_qcom_wlan_nv.bin` (29816 bytes) might be missing calibration data specific to the WCN3620 on this hardware revision. An incorrect NV file could cause the firmware to reject HAL_START_REQ.

3. **Wrong NV file path**: The wcn36xx driver requests firmware `wlan/prima/WCNSS_qcom_wlan_nv.bin`. The OpenWrt firmware-archive package for msm89xx might need the correct NV file from the stock firmware.

4. **Modem integration issue**: `qcom-wcnss-pil` reports `"unexpected response to sysmon event"` during boot. The WCNSS (WiFi/BT/FM) and the modem (MSS) share a system monitor (sysmon) for IPC. If the modem subsystem is not initialized or the sysmon handshake fails, the WCNSS WLAN might not fully initialize.

5. **NVRAM/EFS partition**: On stock Android, the WCNSS reads calibration data from the modem's EFS (Embedded File System) partition. On OpenWrt, this data is provided via the NV file, but the firmware might expect additional parameters from EFS that aren't provided.

---

## 9. DT STRUCTURE

```
soc@0
  └── remoteproc@a204000          (qcom,pronto-v2-pil, qcom,pronto)
       ├── compatible = "qcom,pronto-v2-pil", "qcom,pronto"
       ├── iris                   (iris child node)
       │    └── compatible = "qcom,wcn3620"
       ├── smd-edge
       │    └── wcnss
       │         └── wifi         (qcom,wcnss-wlan)
       │              └── qcom,mmio (phandle to mmio regs)
       ├── firmware-name = "wcnss.mdt"
       └── status = "okay"
```

The wcn36xx driver (probing for `qcom,wcnss-wlan`) gets its device node from the `wcnss/wifi` child of the smd-edge. The `qcom,mmio` phandle points to the parent resource region for register access. The `iris` child at the remoteproc level provides the WCN3620 compatible string.

**Key**: The `iris` node is a **sibling** of `smd-edge`, not a child of the wifi node. The driver finds it via:
```c
mmio_node = of_parse_phandle(pdev->dev.parent->of_node, "qcom,mmio", 0);
iris_node = of_get_child_by_name(mmio_node, "iris");
```

`mmio_node` resolves to the `remoteproc@a204000` node, and then `iris` is found as its child. This works because the remoteproc DT node has `qcom,mmio` as a phandle pointing to itself.

---

## 10. OPENWRT BUILD ENVIRONMENT

### Docker Container Details

- Image: `arm64v8/ubuntu:22.04` (for simulating aarch64 build, but actually using cross-compiler)
- Or: standard Ubuntu container with cross-compiler packages
- Container named: `openwrt-builder`
- Build user: `builder` (UID 1000)
- Toolchain: `aarch64-linux-gnu-gcc` (package `gcc-aarch64-linux-gnu`)
- Also needed: `bc`, `bison`, `flex`, `libssl-dev`

### Cross-toolchain Notes

The device's kernel was built with `aarch64-openwrt-linux-musl-gcc` (OpenWrt GCC 14.3.0), while the module was built with the Ubuntu `aarch64-linux-gnu-gcc` (glibc-based). For kernel modules this is acceptable since:
1. The kernel doesn't link against userspace libc
2. CONFIG_MODVERSIONS is off (no CRC checks)
3. Only the vermagic string must match

However, if issues arise, the proper OpenWrt SDK toolchain should be used:
```
https://downloads.openwrt.org/releases/25.12.5/targets/msm89xx/msm8916/
openwrt-sdk-25.12.5-msm89xx-msm8916_gcc-14.3.0_musl.Linux-x86_64.tar.xz
```

---

## 11. NEXT STEPS (ordered by likelihood of success)

### High Priority

1. **Check sysmon/modem integration**
   The `"unexpected response to sysmon event"` message during boot might be critical. The WCNSS and modem (MSS) share a system monitor. If the modem hasn't booted, the WCNSS WLAN might wait for it. Check if the modem remoteproc needs to be started, or if `qcom-sysmon` needs attention.

2. **Use stock firmware NV file**
   Extract `WCNSS_qcom_wlan_nv.bin` from the stock Android firmware (or from a working UZ801 v3 device) and compare with the current one. The NV file might be missing WCN3620-specific calibration data.

3. **Try WCN3610/3620-specific firmware**
   The WCN3620 is part of the WCN3610 family. Some references suggest it needs different firmware or NV data than the standard MSM8916 WCNSS firmware. Search for `wcn3610` or `wcn3620` in the msm8916-mainline kernel for any hints.

### Medium Priority

4. **Enable WLAN_CTRL SMD logging**
   Add debug prints to `wcn36xx_smd_send_and_wait()` to see exactly what data goes over the SMD channel and what (if anything) comes back. This could show if the firmware sends an error response instead of a success response.

5. **Try the msm8916-mainline kernel directly**
   Flash the msm8916-mainline kernel (wip/msm8916/7.0 branch) instead of OpenWrt's kernel. If WiFi works there, the issue is in OpenWrt's kernel patches or configuration.

6. **Try compat-drivers from msm8916-mainline**
   The msm8916-mainline project may use different firmware or patches for the WCN3620 that aren't in the mainline kernel yet.

7. **Source firmware from working UZ801v3**
   The hkfuertes/msm8916-openwrt GitHub repo (our reference build source) appears no longer accessible (returned 404, API search returns 0 results). If firmware files vary between devices, getting firmware from a known-working UZ801v3 unit might resolve the HAL_START_REQ timeout.

### Lower Priority

8. **Rebuild wcn36xx with OpenWrt SDK toolchain**
   Use the proper OpenWrt SDK toolchain to eliminate any ABI concerns from using the Ubuntu cross-compiler.

9. **Check DT for missing WCNSS resources**
   Compare the device's DT with the msm8916-mainline DT for msm8916-based devices. The WCNSS node might need additional properties.

10. **Try different firmware files**
    Extract WCNSS firmware from a different MSM8916 device stock ROM (e.g., BQ Aquaris X5, Samsung Galaxy J5, etc.) and test.

---

## 12. USEFUL COMMANDS

```bash
# Device access
sshpass -p "456+" ssh root@192.168.1.1

# File transfer
cat local_file | sshpass -p "456+" ssh root@192.168.1.1 'cat > /tmp/dest_file'

# Check kernel log for WiFi
dmesg | grep -E "wcn36xx|wcnss|smd|rproc|hal"

# Module operations
lsmod | grep wcn
modinfo wcn36xx
insmod /lib/modules/6.12.94/wcn36xx.ko
rmmod wcn36xx

# Module swap with backup
cp /lib/modules/6.12.94/wcn36xx.ko{,.bak}
cp /tmp/wcn36xx_fixed.ko /lib/modules/6.12.94/wcn36xx.ko

# Reboot
reboot

# Check DT
cat /sys/firmware/devicetree/base/soc@0/remoteproc@a204000/iris/compatible | tr '\0' ' '

# Check config
zcat /proc/config.gz | grep CONFIG_WCN

# Check remote processor status
cat /sys/class/remoteproc/remoteproc1/state

# Docker build commands
docker exec openwrt-builder bash -c 'cd /tmp/linux-6.12.94 && \
  make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
  KBUILD_MODPOST_WARN=1 M=drivers/net/wireless/ath/wcn36xx modules -j$(nproc)'

# Check module vermagic
modinfo wcn36xx.ko | grep vermagic
aarch64-linux-gnu-readelf -p .modinfo wcn36xx.ko | grep vermagic
```

---

## 13. FILE LOCATIONS

| File | Location |
|------|----------|
| Troubleshooting guide | `/home/cliff/tukzer-wm3311-wifi-research/troubleshooting_openwrt.md` |
| Built fix module | `/tmp/wcn36xx_fixed.ko` (on host) |
| Deployed fix module | `/lib/modules/6.12.94/wcn36xx.ko` (on device, backup at .bak) |
| Kernel source | `/tmp/linux-6.12.94/` (in Docker container) |
| Docker container | `openwrt-builder` (Ubuntu 22.04 + aarch64 toolchain) |
| Original OpenWrt module | `/lib/modules/6.12.94/wcn36xx.ko.bak` (on device) |
| Host SSH password | `456+` |
| Device SSH | `root@192.168.1.1` (via USB RNDIS/Ethernet) |
| Device default route | `192.168.1.238` (host's USB IP) |
| Host eth0 | `192.168.1.238/24` |

---

## 14. TIMELINE OF ISSUE

```
boot → WCNSS firmware loads (11.13s) → driver probe, MAC read OK (11.52s) →
HAL_START_REQ sent → firmware does NOT respond (25.81s timeout) → retry (36.05s) →
firmware watchdog fires: "dog.c:1676:Watchdog detects task starvation" (74.25s) →
WCNSS crashes → remoteproc recovery → cycle repeats every ~63s
```

Crash #62 was observed, meaning the device had been running for >71 minutes in a watchdog loop.

---

## 15. LED CONFIGURATION

### GPIO Mapping

The Tukzer WM3311 has 3 physical LEDs, mapped to MSM8916 TLMM GPIOs:

| LED Label | GPIO | DT Node | Physical Color | Active Level |
|-----------|------|---------|----------------|
| `red:power` | TLMM 7 (GPIO 519) | `led-r` | Red/Purple | Active High |
| `green:wan` | TLMM 8 (GPIO 520) | `led-g` | Green | Active High |
| `blue:wlan` | TLMM 6 (GPIO 518) | `led-b` | Blue | Active High |

GPIOs are offset by 512 on MSM8916 TLMM (`gpiochip0: GPIOs 512-633`).

### Current Configuration (as of 2026-07-28)

Configured via OpenWrt UCI `system` config and applied by `/etc/init.d/led`:

| LED | Trigger | Brightness | Intent |
|-----|---------|------------|--------|
| `red:power` | `default-on` | 1 | Power indicator — always on when device has power |
| `green:wan` | `heartbeat` | pulsing | Device alive — pulses to show system is running |
| `blue:wlan` | `timer` | blinking | WiFi placeholder — blinks at 1Hz. Will switch to `phy268radio` when WiFi works |

### DT Properties

All three LEDs use `function` and `color` properties (already set in the running firmware, possibly by hkfuertes patch 803):

```
led-b: function = "wlan", color = 3 (LED_COLOR_ID_BLUE)
led-g: function = "wan",  color = 2 (LED_COLOR_ID_GREEN)
led-r: function = "power", color = 1 (LED_COLOR_ID_RED)
```

### Available Triggers

| Trigger | Available | Notes |
|---------|-----------|-------|
| `default-on` | ✅ Built-in | Used for `red:power` |
| `heartbeat` | ✅ Built-in | Used for `green:wan` |
| `timer` | ✅ Built-in | Used for `blue:wlan` |
| `phy268radio` | ✅ Listed | Requires `CONFIG_LED_TRIGGER_PHY` (not set in this kernel) |
| `netdev` | ❌ Not built | `CONFIG_LED_TRIGGER_NETDEV` is not set; no `kmod-ledtrig-netdev` available (no `opkg`) |
| `usb-gadget` | ✅ Built-in | Available but not useful for current setup |

### Hardware Verification

All three LEDs respond to brightness control via `/sys/class/leds/*/brightness`:
```bash
echo 1 > /sys/class/leds/blue:wlan/brightness   # Blue LED ON
echo 0 > /sys/class/leds/green:wan/brightness   # Green LED OFF
echo 1 > /sys/class/leds/red:power/brightness    # Red LED ON
```

GPIO control is fully functional. The previous "LEDs not working" issue was due to all triggers being set to `[none]` — the GPIO hardware was fine, but no trigger was driving the LEDs.

### Comparison with Stock Android

| Context | Red (GPIO 7) | Green (GPIO 8) | Blue (GPIO 6) |
|---------|-------------|---------------|--------------|
| **Stock Android** | Power/Network (purple) | Battery (green) | WiFi activity (blue) |
| **OpenWrt (before fix)** | default-on (solid) | none (off) | none (off) |
| **OpenWrt (after fix)** | default-on (solid) | heartbeat (pulsing) | timer (blinking) |
| **Ideal (when WiFi works)** | default-on (solid) | netdev on usb0 | phy268radio |

### hkfuertes LED Patch (803)

The `hkfuertes/msm8916-openwrt` reference build includes patch `0803-arm64-dts-qcom-uf02-add-leds-function-and-color.patch` which adds `function` and `color` properties to the LED nodes. These properties are semantic (they affect `/sys/class/leds/` naming and classification) but do NOT set trigger behavior. The actual trigger must be set via `linux,default-trigger` in DT or via UCI/sysfs at runtime.

**Current status**: The running firmware already has `function` and `color` set (confirming patch 803 or equivalent was applied). The remaining fix was to set appropriate triggers via UCI, which is now done.

### Notes
- The `netdev` trigger is not available in this kernel build. To get WAN activity indication, either rebuild with `CONFIG_LED_TRIGGER_NETDEV=y` or install the module (requires `opkg`).
- The PHY radio trigger (`phy268radio`) appears in the sysfs trigger list but requires `CONFIG_LED_TRIGGER_PHY=y` to work. Currently setting it via sysfs succeeds but `/etc/init.d/led` rejects it with "missing kernel module".
- Manually installed `opkg` would resolve this, but the device has no package manager.

## 16. KEY FILES FOR REFERENCE

- `/home/cliff/tukzer-wm3311-wifi-research/` — main research directory
- `troubleshooting_openwrt.md` — this file
- `/home/cliff/tukzer-wm3311-wifi-research/README.md` — project README with device info, references
- `/home/cliff/Documents/Tukzer_WM311/SESSION-LOG.md` — full session log from earlier Android exploration
- We should also save these raw data dumps:
  - Full dmesg output
  - Device DT dump (`dtc -I fs -O dts /sys/firmware/devicetree/base`)
  - Current wireless config (`cat /etc/config/wireless`)
  - Module info from both old and new modules
