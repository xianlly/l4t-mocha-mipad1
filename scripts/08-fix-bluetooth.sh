#!/bin/bash
# 08-fix-bluetooth.sh
# Fix the dead Bluetooth controller on L4T + Ubuntu 14.04/16.04 (Xiaomi Mi Pad 1,
# Tegra K1, Broadcom BCM43341 over UART).
#
# Symptom:
#   hciconfig -a  ->  "Can't read local name on hci0: Connection timed out (110)"
#   dmesg         ->  "Bluetooth: hci0 command 0x0c03 tx timeout" and friends
#   hci0 shows UP RUNNING PSCAN but no ISCAN, so the tablet is not discoverable.
#
# Root cause:
#   The stock /etc/init/nvwifibt.conf enables low-power mode:
#       echo 1 > /proc/bluetooth/sleep/lpm
#       brcm_patchram_plus ... --enable_lpm --tosleep=50000 ...
#   The controller sleeps after 50 ms idle and the LPM wake path does not work
#   on this board, so it never answers another HCI command.
#
# Fix:
#   Drop LPM, drop the 3 Mbaud speed switch, drop --patchram (not needed).
#   A reboot is required afterwards so the controller is power-cycled.
#
# Safe to re-run. Run ON THE DEVICE with sudo.
#
# Usage: sudo ./08-fix-bluetooth.sh [--apply|--revert|--verify]

set -eu

MODE="${1:---apply}"
CONF=/etc/init/nvwifibt.conf
BACKUP=/root/nvwifibt.conf.orig

[ "$(id -u)" = "0" ] || { echo "ERROR: run as root (sudo)"; exit 2; }

step()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
ok()    { printf '    \033[32mOK\033[0m  %s\n' "$*"; }
warn()  { printf '    \033[33m!!\033[0m  %s\n' "$*"; }

verify() {
    step "verify"
    echo "--- hciconfig ---"
    hciconfig -a 2>&1 | head -14 || true

    echo
    echo "--- HCI Reset (the decisive test) ---"
    if timeout 8 hcitool cmd 0x03 0x0003 2>&1 | grep -q 'HCI Event'; then
        ok "controller answers HCI commands"
    else
        warn "no HCI response (did you reboot?)"
    fi

    echo
    echo "--- recent kernel messages ---"
    dmesg 2>/dev/null | grep -iE 'bluetooth|hci' | tail -6 || true

    echo
    if hciconfig hci0 2>/dev/null | grep -q ISCAN; then
        ok "ISCAN enabled (device is discoverable)"
    else
        warn "ISCAN not set"
    fi
}

case "$MODE" in

--verify)
    verify
    ;;

--revert)
    step "revert to the stock configuration"
    if [ -f "$BACKUP" ]; then
        cp -f "$BACKUP" "$CONF"
        ok "restored $BACKUP -> $CONF"
        echo
        echo "Reboot for the (broken) stock behaviour to take effect."
    else
        warn "no backup found at $BACKUP"
        warn "reinstall the package that ships it, or restore by hand"
        exit 1
    fi
    ;;

--apply|*)
    step "1/3  back up the stock configuration"
    if [ -f "$BACKUP" ]; then
        ok "backup already exists: $BACKUP (kept)"
    else
        cp -f "$CONF" "$BACKUP"
        ok "saved $CONF -> $BACKUP"
    fi

    step "2/3  write the fixed configuration"
    cat > "$CONF" <<'EOF'
description     "NVIDIA bluetooth/wifi init script"
#
# L4T on Xiaomi Mi Pad 1 (mocha / Tegra K1) - Bluetooth bring-up
#
# WORKING CONFIGURATION.
#
# The stock script enabled low-power mode:
#     echo 1 > /proc/bluetooth/sleep/lpm
#     brcm_patchram_plus ... --enable_lpm --tosleep=50000 ...
# The LPM wake path does not work on this board, so the controller slept
# and never answered another HCI command ("tx timeout" on every command).
#
# Do NOT re-add --enable_lpm / --tosleep / --patchram without testing:
# the firmware patch is not needed for normal operation.
#
instance $SUBTASK

script
	exec /usr/sbin/rfkill unblock bluetooth
	exec /usr/sbin/brcm_patchram_plus --enable_hci --scopcm=0,2,0,0,0,0,0,0,0,0 --baudrate 115200 --no2bytes /dev/ttyTHS2
end script
EOF
    chmod 644 "$CONF"
    ok "wrote $CONF"

    step "3/3  done - reboot to power-cycle the controller"
    echo
    echo "    sudo reboot"
    echo
    echo "Then check with:  sudo $0 --verify"
    ;;
esac
