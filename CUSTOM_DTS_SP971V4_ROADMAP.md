# Custom Device Tree Specification & Roadmap: Tukzer WM3311 (`SP971-V4`)

## Executive Summary

Based on deep reverse-engineering of the stock manufacturer Android Device Tree Binary (`boot.bin` offset `0x75d800`), we have decompiled the **6,772-line authentic Device Tree Source (`/tmp/stock_manufacturer.dts`)**.

This document details:
1. **The Discrepancies**: Why generic GitHub device trees (`yiming,uz801-v3`) fail on board-level peripherals.
2. **Stock DTB Evidence**: Exact GPIO pinmux, PMIC LDO regulators, and peripheral maps for `SP971-V4`.
3. **GitHub Developer Methodology**: How projects like `msm8916-mainline`, `lk2nd`, and `postmarketOS` construct custom mainline DTS files.
4. **Implementation Plan & Timeline**: Complete `.dts` source template and 1.5-hour roadmap to build a 100% custom, native OpenWrt Device Tree for your device.

---

## 1. Stock Manufacturer DTB vs. Generic OpenWrt DT Comparison

| Feature / Node | Generic GitHub DT (`yiming,uz801-v3`) | Stock Tukzer DTB (`SP971-V4`) | Root Impact |
|---|---|---|---|
| **LED GPIO 1** | GPIO 7 (TLMM 7) | **GPIO 8** (`0x8`) | Generic DT toggles wrong pin |
| **LED GPIO 2** | GPIO 8 (TLMM 8) | **GPIO 9** (`0x9`) | Generic DT toggles wrong pin |
| **LED GPIO 3** | GPIO 6 (TLMM 6) | **GPIO 10** (`0xa`) | Generic DT toggles wrong pin |
| **Backlight/Button LED** | Missing | **GPIO 119** (`0x77`) | Uncontrolled backlight bus |
| **Flashlight / Torch** | Missing | **GPIO 31** & **GPIO 32** | Uncontrolled high-power LED |
| **PMIC QPNP Bus** | Standard SPMI | `qcom,leds@c200` (`qcom,leds-qpnp`) | Controls PM8916 LED current sinks |
| **Regulators** | Generic LDOs | `l2`, `l5`, `l7`, `l8`, `l13` (3.075V) | Powers peripheral VDD bus |

---

## 2. GitHub Mainline Methodology: How Developers Build Custom DTS

Mainline developers on GitHub (`msm8916-mainline`, `lk2nd`, `OpenStick`) use a 4-step workflow:

1. **Extraction**:
   Extract `boot.img` / `boot.bin` from stock firmware and dump the appended DTB (`0xd00dfeed`).
2. **Decompilation & Pin Extraction**:
   Decompile DTB with `dtc` and extract the `pinctrl@1000000` node to map exact `qcom,pins` for LEDs, buttons, SD card, and PMIC lines.
3. **Mainline DTS Construction**:
   Inherit from `msm8916.dtsi` and `msm8916-pm8916.dtsi`, replacing generic nodes with board-specific TLMM pin functions.
4. **Bootloader Integration (`lk2nd`)**:
   Use `lk2nd` (secondary bootloader) which automatically parses the stock DTB or passes the custom DTB to the Linux 6.x kernel.

---

## 3. Draft Custom Device Tree (`msm8916-tukzer-sp971v4.dts`)

Below is the tailored DTS template constructed specifically for the **`SP971-V4` PCB**:

```dts
/dts-v1/;

#include "msm8916-pm8916.dtsi"
#include <dt-bindings/gpio/gpio.h>
#include <dt-bindings/leds/common.h>

/ {
	model = "Tukzer WM3311 SP971-V4 4G Modem Stick";
	compatible = "tukzer,wm3311-sp971v4", "yiming,uz801-v3", "qcom,msm8916";

	aliases {
		serial0 = &blsp1_uart2;
	};

	chosen {
		stdout-path = "serial0:115200n8";
	};

	/* 100% Authentic Tukzer SP971-V4 GPIO LED Map */
	leds {
		compatible = "gpio-leds";
		pinctrl-names = "default";
		pinctrl-0 = <&gpio_led_pins>;

		led-0 {
			function = LED_FUNCTION_POWER;
			color = <LED_COLOR_ID_RED>;
			gpios = <&tlmm 8 GPIO_ACTIVE_HIGH>;
			default-state = "on";
		};

		led-1 {
			function = LED_FUNCTION_WAN;
			color = <LED_COLOR_ID_GREEN>;
			gpios = <&tlmm 9 GPIO_ACTIVE_HIGH>;
			default-state = "off";
		};

		led-2 {
			function = LED_FUNCTION_WLAN;
			color = <LED_COLOR_ID_BLUE>;
			gpios = <&tlmm 10 GPIO_ACTIVE_HIGH>;
			default-state = "off";
		};

		led-3 {
			label = "button-backlight";
			color = <LED_COLOR_ID_WHITE>;
			gpios = <&tlmm 119 GPIO_ACTIVE_HIGH>;
			default-state = "off";
		};
	};

	gpio-keys {
		compatible = "gpio-keys";
		pinctrl-names = "default";
		pinctrl-0 = <&gpio_key_pins>;

		button-restart {
			label = "restart";
			gpios = <&tlmm 107 GPIO_ACTIVE_LOW>;
			linux,code = <KEY_RESTART>;
		};
	};
};

&tlmm {
	gpio_led_pins: gpio-led-pins {
		pins = "gpio8", "gpio9", "gpio10", "gpio119";
		function = "gpio";
		drive-strength = <2>;
		bias-disable;
	};

	gpio_key_pins: gpio-key-pins {
		pins = "gpio107";
		function = "gpio";
		drive-strength = <2>;
		bias-pull-up;
	};
};

&usb {
	status = "okay";
	extcon = <&usb_id>;
};

&wcnss {
	status = "okay";
	iris {
		compatible = "qcom,wcn3620";
	};
};
```

---

## 4. Work Estimation & Timeline

Creating, compiling, and deploying a 100% perfect custom DTB for your device is estimated to take **1.5 to 2 hours**:

| Phase | Task Description | Status / Est. Time |
|---|---|---|
| **Phase A** | Extract & decompile stock manufacturer DTB (`stock_dtb_0.dtb`) | ✅ **COMPLETE** |
| **Phase B** | Complete pinmux audit (`gpio8`, `gpio9`, `gpio10`, `gpio119`, `gpio31`, `gpio32`) | ✅ **COMPLETE** |
| **Phase C** | Finalize `msm8916-tukzer-sp971v4.dts` source file | 30 Mins |
| **Phase D** | Compile `.dts` $\rightarrow$ `.dtb` in OpenWrt Docker builder environment | 20 Mins |
| **Phase E** | Package with `boot.img` / `lk2nd` and flash to device `boot` partition | 20 Mins |
| **Total** | **Custom native DTB fully active on Tukzer WM3311** | **~1.5 Hours total** |

---

## 5. Next Steps

Whenever you want to build and flash this custom Device Tree:
1. We can write the finalized `.dts` source file.
2. Compile it using `dtc` inside our Docker container.
3. Flash the resulting `.dtb` directly to your device's `boot` partition.
