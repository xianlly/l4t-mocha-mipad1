# BUG: Device cannot be powered on after shutdown — power button unresponsive ("black brick")

**Status:** diagnosed, root-caused, workaround + fix verified
**Device:** Xiaomi Mi Pad 1 (`mocha`, A0101, Tegra K1 / Tegra124)
**OS:** Ubuntu 14.04 LTS, NVIDIA L4T R24.1, kernel `3.10.108_SmokeR24.1-V3.5.1`
**Filed by:** the debugging session that produced this repository

---

## Summary

After shutting the system down, the tablet does not respond to the power button.
The screen stays black, no USB enumeration (not even APX), and `fastboot` sees nothing.
The only way out is a full power removal (battery connector pull) followed by a long press.

This makes the L4T installation effectively unusable: any shutdown can brick the unit
until the back cover is opened.

---

## Steps to reproduce

1. Boot the L4T system (Ubuntu 14.04, stock L4T kernel/DTB/initramfs).
2. Shut down **from the running system**, either by:
   * pressing the power key (default `HandlePowerKey`/`button-power` behaviour → poweroff), or
   * `sudo poweroff` / desktop "Shut Down" menu.
3. Wait for the device to power down.
4. Press the power button.
5. Also try a 30 s long press, then plugging in a charger.

## Expected

The device powers on normally (as it does after a shutdown in Android).

## Actual

* Screen stays black.
* No USB device appears on the host (no fastboot, no APX/RCM).
* `fastboot devices` → empty.
* Power button short/long press → no effect.
* Recovery only possible by disconnecting the battery, waiting, reconnecting and
  pressing power (sometimes requiring several attempts).

---

## Root cause

Two independent facts combine into a trap.

### 1. The vendor power-off driver enters a state that needs a long press

`drivers/power/reset/palmas-poweroff.c` (NVIDIA downstream driver, enabled by
`CONFIG_POWER_OFF_PALMAS=y`, which `select`s `CONFIG_SYSTEM_PMIC=y`):

```c
static void palmas_power_off(void *drv_data)
{
        ...
        /* Mask all COLD RST condition */
        palmas_write(palmas, PALMAS_PMU_CONTROL_BASE,
                                PALMAS_SWOFF_COLDRST, 0x0);

        dev_info(palmas_pm->dev, "Powering off the device\n");

        /* Power off the device */
        palmas_update_bits(palmas, PALMAS_PMU_CONTROL_BASE,
                                PALMAS_DEV_CTRL, 1, 0);
}
```

* `SWOFF_COLDRST` bit 7 is `PWRON_LPK` — "power-on **long press key**", i.e. the only
  wake event for the PMIC's soft-off state. The driver **masks every bit**, including it.
* `DEV_CTRL` bit 0 (`DEV_ON`) is then cleared → the PMIC performs a *soft* power-off.

So the PMIC ends up in a state where a long press is required, while the long press is
exactly what was disabled.

### 2. The register is write-protected, so nobody can re-enable it

Reading the PMIC directly over I2C (bus 4, slave `0x5a`, 8-bit registers) on a live system:

```
0x5a[0xa0] = 0x0b     # DEV_CTRL: DEV_ON=1, status bits  -> device is on
0x5a[0xb0] = 0x5b     # SWOFF_COLDRST = 0b01011011      -> PWRON_LPK(0x80) = 0 !
```

Attempting to set the missing bit from userspace:

```sh
i2cset -f -y 4 0x5a 0xb0 0xdb   # 0x5b | 0x80
i2cget -f -y 4 0x5a 0xb0
0x5b                            # <-- write silently ignored
```

The value is unchanged, so the register (or that bit) is effectively locked on this PMIC
configuration, and `0x5b` is also what the boot chain leaves behind on every cold boot.

> Note: the L4T kernel's own `palmas_write(..., SWOFF_COLDRST, 0x0)` also appears to be a
> no-op for the same reason — the real damage is the `DEV_ON` clear combined with the
> permanently-masked `PWRON_LPK`.

### 3. Why Android and the mainline Debian do not hit it

* **Mainline Debian (6.12 / 6.14):** the upstream kernel has **no** `palmas-poweroff`
  driver at all (it is an NVIDIA downstream patch). `pm_power_off` is served by the
  Tegra **PMC** instead, which cuts the SoC power domains but leaves the PMIC in its
  normal state — the power button still starts it.
* **Android:** the stock Android defconfig also sets `CONFIG_POWER_OFF_PALMAS=y`
  (verified in `arch/arm/configs/mocha_android_defconfig`, line 293), so the static
  configuration does not explain it. The most likely explanation is that Android's
  "power off" ends up in the bootloader's off/charging state rather than a PMIC soft-off.
  This has **not** been verified on hardware and is left as an open question.

Evidence for the mainline difference — no `palmas_power_off` symbol in the mainline tree:

```
$ grep -rn 'pm_power_off' drivers/power/reset/    # mainline 6.12
(only gpio-poweroff / restart-poweroff / ... ; no palmas)
```

---

## Fix (verified 3/3 on hardware)

Remove the DT binding so the kernel has **no** `pm_power_off` and therefore halts instead
of issuing the PMIC soft-off. The PMIC keeps `PWRDOWN` (bit 6 of `SWOFF_COLDRST`, already
set in `0x5b`), so pressing the power button performs a cold reset.

```sh
dtc -I dtb -O dts -o fix.dts tegra124-mocha.dtb
# in node /i2c@7000d000/tps65913/power_pm  delete the line:  system-pmic-power-off;
dtc -I dts -O dtb -o dtb-final.dtb fix.dts
```

Script: [`scripts/04-build-dtb-nopmoff.sh`](../scripts/04-build-dtb-nopmoff.sh)

Verification after the change:

```sh
# the property must be gone from the live tree
test -e /proc/device-tree/i2c@7000d000/tps65913/power_pm/system-pmic-power-off \
  && echo "still there - fix not applied" || echo "no pm_power_off (good)"
```

Shutdown → halt → press power → boots. Measured three consecutive cycles:

| cycle | result | boot marker |
|---|---|---|
| 1 | booted | `uptime` 48 s, `system boot 12:54` |
| 2 | booted | `uptime` 37 s, `system boot 12:56` |
| 3 | booted | `uptime` 36 s, `system boot 12:58` |

### Alternative workaround (no DTB change)

Replace the final power-off action of the SysV halt script so a shutdown reboots instead
of powering off:

```sh
# /etc/init.d/halt  (backup first!)
- halt -d -f $netdown $poweroff $hddown
+ reboot -d -f $netdown $hddown
```

This never enters the PMIC soft-off state. Downside: you can no longer really power off
from software (use the battery connector for that).

---

## Diagnostic commands used (for anyone reproducing this)

```sh
# 1. confirm which driver owns pm_power_off
grep -rn 'pm_power_off *=' drivers/ arch/arm/mach-tegra/

# 2. confirm the config
grep -E 'POWER_OFF_PALMAS|SYSTEM_PMIC|POWER_RESET' .config

# 3. read the PMIC over I2C (needs i2c-tools, -f because the palmas driver owns the address)
i2cget -f -y 4 0x5a 0xa0     # DEV_CTRL
i2cget -f -y 4 0x5a 0xb0     # SWOFF_COLDRST

# 4. check the live device tree (what the kernel actually uses)
ls /proc/device-tree/i2c@7000d000/tps65913/power_pm/

# 5. check whether any driver was bound for it
dmesg | grep -iE 'palmas|system.pmic'
```

---

## Open questions

1. Does the tps65913 really lock `SWOFF_COLDRST` permanently, or is a specific unlock
   sequence needed (page register / OTP key)? If it can be unlocked, the cleanest fix is
   a one-line kernel patch setting `PWRON_LPK` before the soft-off.
2. Does Android's "power off" actually reach `palmas_power_off`, or does the bootloader
   take over first? Testing: power off in Android, then measure the power draw, or check
   whether the device shows a charging UI when a cable is plugged in.

## References

* `drivers/power/reset/palmas-poweroff.c`, `drivers/power/reset/system-pmic.c`
  (L4T-kernel-mocha, 3.10.107 tree)
* `include/linux/mfd/palmas.h` — `PALMAS_DEV_CTRL`, `PALMAS_SWOFF_COLDRST` and bit definitions
* `arch/arm/configs/mocha_linux_defconfig` / `mocha_android_defconfig`
* This repository's `README.md` §5.4 and `scripts/04-build-dtb-nopmoff.sh`
