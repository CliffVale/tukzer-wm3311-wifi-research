# Log Files

This directory contains raw device data from the Tukzer WM3311 running OpenWrt 25.12.5.

| File | Description |
|------|-------------|
| `device-dump.txt` | Full dmesg + kernel info + firmware listing (520 lines) |
| `crash-pattern.txt` | Extracted crash cycles from the full dump |
| `iw-list.txt` | `iw list` output showing WCN3620 (2.4 GHz only, no 5 GHz band) |
| `current-state.txt` | Latest live device state |

**Note**: These logs were captured on 2026-07-28 during active crash cycling. The device was in crash #14+ territory when captured.
