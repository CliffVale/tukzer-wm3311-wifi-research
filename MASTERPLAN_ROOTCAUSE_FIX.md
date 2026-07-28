# Master Blueprint: Bug-Free OpenWrt on Tukzer WM3311 (UZ801 v3.0)

## Executive Summary & Breakthrough Discovery

During deep analysis of the OpenCode session logs and binary firmware dumps, a **critical root-cause flaw** was identified in the OpenWrt initialization logic for the **Tukzer WM3311 / UZ801 v3.0 (Qualcomm MSM8916)**.

The stock OpenWrt build relies on an init script (`/usr/bin/msm-firmware-dumper`) to extract proprietary Qualcomm firmware blobs from the device's eMMC partitions on first boot. However, the script hardcodes incorrect device nodes:
* Script expected `modem` on `/dev/mmcblk0p3` and `persist` on `/dev/mmcblk0p6`.
* On the Tukzer WM3311, GPT Partition 1 (`mmcblk0p1`) is `modem` (FAT16), and Partition 24 (`mmcblk0p24`) is `persist` (ext4).

As a result, both partition mounts failed on boot, leaving `/lib/firmware/` without the authentic `WCNSS_qcom_wlan_nv.bin` calibration binary, `WCNSS_qcom_cfg.ini`, `mba.mbn` (Modem Boot Authenticator), and `modem.mdt` firmware. This caused:
1. **WiFi Failure**: `wcn36xx` timed out waiting for SMD response to `HAL_START_REQ` (req 55), triggering the internal RIVA watchdog (`dog.c:1676`) every ~63 seconds.
2. **Modem Offline**: Modem remoteproc stayed offline because `mba.mbn` was never populated in `/lib/firmware/`.

---

## Device & Partition Profile

| Field | Description |
|-------|-------------|
| Model | Tukzer WM3311 (UZ801 v3.0 4G LTE Stick) |
| SoC | Qualcomm MSM8916 (4x Cortex-A53 @ 1.2GHz) |
| WiFi/BT | WCN3620 (qcom,wcn3620 via DT Iris node) |
| RAM / eMMC | 512MB RAM / 8GB eMMC |
| OS | OpenWrt 25.12.5 (Linux Kernel 6.12.94 aarch64) |

### Verified GPT Partition Layout (eMMC)

```text
mmcblk0p 1 : modem    (64.00 MB, FAT16)  <-- Contains mba.mbn, modem.mdt, wcnss.mdt
mmcblk0p 2 : sbl1     (0.50 MB)
mmcblk0p 3 : sbl1bak  (0.50 MB)          <-- Incorrectly mounted as modem by old script!
mmcblk0p 4 : aboot    (1.00 MB)
mmcblk0p 6 : rpm      (0.50 MB)          <-- Incorrectly mounted as persist by old script!
mmcblk0p20 : fsg      (1.50 MB)          <-- Modem EFS/NV storage
mmcblk0p22 : boot     (16.00 MB)
mmcblk0p23 : system   (800.00 MB)
mmcblk0p24 : persist  (32.00 MB, ext4)   <-- Contains WCNSS_qcom_wlan_nv.bin!
mmcblk0p27 : userdata (2542.44 MB)
```

---

## Step-by-Step Fix Implementation

### Phase 1: Deploy Authentic Firmware & Fix Dumper Script

All 35 authentic stock firmware blobs have been extracted from `modem.bin` and `persist.bin` and staged locally at:
`/home/cliff/tukzer-wm3311-wifi-research/firmware_fix/`

#### 1. Fix `/usr/bin/msm-firmware-dumper` on OpenWrt
Update lines 29-30 in `/usr/bin/msm-firmware-dumper`:
```bash
- mount -t vfat -o ro,... /dev/mmcblk0p3 "$MNT/modem"
- mount -t ext4 -o ro,... /dev/mmcblk0p6 "$MNT/persist"
+ mount -t vfat -o ro,... /dev/mmcblk0p1 "$MNT/modem"
+ mount -t ext4 -o ro,... /dev/mmcblk0p24 "$MNT/persist"
```

#### 2. Manual Firmware Sync Command (Host to Device)
To apply the fix immediately without re-flashing:
```bash
# Push firmware fix bundle to OpenWrt device
cd /home/cliff/tukzer-wm3311-wifi-research/firmware_fix
sshpass -p "456+" ssh root@192.168.1.1 "mkdir -p /lib/firmware/wlan/prima"
scp -r * root@192.168.1.1:/lib/firmware/
```

### Phase 2: Modem Activation & SIM Setup (QMI Protocol)

1. Verify Modem Boot Authenticator and remoteproc status:
   ```bash
   sshpass -p "456+" ssh root@192.168.1.1 "dmesg | grep -i rproc"
   ```
2. Enable ModemManager and QMI interface:
   ```bash
   sshpass -p "456+" ssh root@192.168.1.1 '
   uci set network.wan=interface
   uci set network.wan.proto="qmi"
   uci set network.wan.device="/dev/cdc-wdm0"
   uci set network.wan.apn="jionet"   # Use "airtelgprs.com" for Airtel or "www" for Vi
   uci set network.wan.auth="none"
   uci set network.wan.pdptype="ipv4"
   uci commit network
   ifup wan
   '
   ```

### Phase 3: WiFi Driver Verification (`wcn36xx`)

With `wlan/prima/WCNSS_qcom_wlan_nv.bin` (29,816 bytes, MD5: `348f95c2bcabaf9b89887c48ec7d6f31`) in place:
1. Reload `wcn36xx` module:
   ```bash
   rmmod wcn36xx
   insmod /lib/modules/6.12.94/wcn36xx.ko
   ```
2. Check `dmesg`:
   `wcn36xx: mac address: 02:00:...` and `wcn36xx: WCN3620 initialized successfully`.
3. Configure OpenWrt Wireless AP:
   ```bash
   uci set wireless.radio0.disabled='0'
   uci set wireless.default_radio0.ssid='Tukzer_OpenWrt'
   uci set wireless.default_radio0.encryption='psk2'
   uci set wireless.default_radio0.key='1234567890'
   uci commit wireless
   wifi reload
   ```

### Phase 4: LED Indicators Setup

Enforce verified UCI triggers:
```bash
uci set system.led_power=led
uci set system.led_power.name='Power'
uci set system.led_power.sysfs='red:power'
uci set system.led_power.trigger='default-on'

uci set system.led_wan=led
uci set system.led_wan.name='WAN'
uci set system.led_wan.sysfs='green:wan'
uci set system.led_wan.trigger='heartbeat'

uci set system.led_wlan=led
uci set system.led_wlan.name='WLAN'
uci set system.led_wlan.sysfs='blue:wlan'
uci set system.led_wlan.trigger='timer'
uci set system.led_wlan.delayon='500'
uci set system.led_wlan.delayoff='500'

uci commit system
/etc/init.d/led restart
```

---

## Verification & Validation Plan

1. **Watchdog Loop Elimination**: Monitor `dmesg` for 5+ minutes to verify 0 occurrences of `Watchdog detects task starvation` (`dog.c:1676`).
2. **Modem Ping Test**: `ping -I wwan0 8.8.8.8` to ensure SIM data connectivity.
3. **WiFi Station Connection**: Connect client device to `Tukzer_OpenWrt` AP and verify DHCP IP allocation.

---

## Repository Update

All files and scripts have been packaged in `/home/cliff/tukzer-wm3311-wifi-research/firmware_fix` and will be pushed to the GitHub repository:
`https://github.com/CliffVale/tukzer-wm3311-wifi-research`
