#!/bin/bash
# 08-fix-bluetooth.sh
# Bring the dead Bluetooth controller on L4T + Ubuntu 14.04/16.04 back to life
# (Xiaomi Mi Pad 1, Tegra K1, Broadcom BCM43341 over UART).
#
# Two independent problems are fixed:
#
#   1) The controller never answers HCI commands
#        hciconfig -a -> "Can't read local name on hci0: Connection timed out"
#        dmesg        -> "Bluetooth: hci0 command 0x0c03 tx timeout" ...
#      Cause: the stock unit file enables low-power mode
#        (echo 1 > /proc/bluetooth/sleep/lpm and --enable_lpm --tosleep=50000).
#      The LPM wake path does not work on this board, so the chip sleeps
#      forever.  --patchram is also unusable: BCM4350C0.hcd is for a different
#      chip and brcm_patchram_plus hangs on it.
#
#   2) The tablet is invisible to other devices even though hciconfig looks fine
#        BD Address: 43:50:C0:00:00:00
#      Inquiry responses carry that placeholder address and phones drop them.
#      /usr/local/sbin/bt-mac-setup.sh writes a real BD_ADDR (derived from the
#      WiFi MAC via the Broadcom 0xFC01 vendor command) on every boot, sets a
#      sane Class of Device and enables inquiry+page scan.
#
# Safe to re-run. Run ON THE DEVICE with sudo.
#
# Usage: sudo ./08-fix-bluetooth.sh [--apply|--revert|--verify]

set -eu

MODE="${1:---apply}"
CONF=/etc/init/nvwifibt.conf
MAC_HELPER=/usr/local/sbin/bt-mac-setup.sh
BACKUP=/root/nvwifibt.conf.orig

[ "$(id -u)" = "0" ] || { echo "ERROR: run as root (sudo)"; exit 2; }

step()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
ok()    { printf '    \033[32mOK\033[0m  %s\n' "$*"; }
warn()  { printf '    \033[33m!!\033[0m  %s\n' "$*"; }

verify() {
    step "verify"

    echo "--- hciconfig ---"
    hciconfig -a 2>&1 | head -16 || true

    echo
    echo "--- HCI Reset (proves the controller answers) ---"
    if timeout 8 hcitool cmd 0x03 0x0003 2>&1 | grep -q 'HCI Event'; then
        ok "controller answers HCI commands"
    else
        warn "no HCI response (did you reboot?)"
    fi

    echo
    echo "--- BD address (must NOT be 43:50:C0:00:00:00) ---"
    local bd
    bd=$(hciconfig hci0 2>/dev/null | grep -o 'BD Address: [0-9A-Fa-f:]*' | awk '{print $3}')
    case "$bd" in
        43:50:C0:00:00:00|"") warn "placeholder BD_ADDR ($bd) -> other devices will ignore it" ;;
        *) ok "BD_ADDR = $bd" ;;
    esac

    echo
    echo "--- discoverable? ---"
    if hciconfig hci0 2>/dev/null | grep -q ISCAN; then
        ok "ISCAN enabled (device is discoverable)"
    else
        warn "ISCAN not set"
    fi

    echo
    echo "--- recent kernel messages ---"
    dmesg 2>/dev/null | grep -iE 'bluetooth|hci' | tail -6 || true
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
    else
        warn "no backup found at $BACKUP"
    fi
    if [ -f "$MAC_HELPER" ]; then
        rm -f "$MAC_HELPER" && ok "removed $MAC_HELPER"
    fi
    echo
    echo "Reboot for the (broken) stock behaviour to take effect."
    ;;

--apply|*)
    step "1/4  back up the stock unit file"
    if [ -f "$BACKUP" ]; then
        ok "backup already exists: $BACKUP (kept)"
    else
        cp -f "$CONF" "$BACKUP"
        ok "saved $CONF -> $BACKUP"
    fi

    step "2/4  install the BD_ADDR helper"
    mkdir -p /usr/local/sbin
    cat > "$MAC_HELPER" <<'HELPER'
#!/bin/sh
#
# bt-mac-setup.sh - give the Broadcom controller a valid BD_ADDR at boot
#
# The controller boots with the placeholder address 43:50:C0:00:00:00.
# Inquiry responses carry it and other devices silently drop them, so the
# tablet looks invisible even though hciconfig says "UP RUNNING PSCAN ISCAN".
#
# The firmware patch cannot fix this here (BCM4350C0.hcd is for a different
# chip and brcm_patchram_plus hangs on it), so we write the address with the
# Broadcom vendor command Write BD_ADDR = 0xFC01 (ogf 0x3f, ocf 0x0001).
#
# The value is derived from the WiFi MAC (same OUI, last octet + 1), which is
# the conventional way to address a combo chip.  The write lives in controller
# RAM, hence this script runs on every boot from the unit file's post-start.
#
# Opcode warning: HCI_Write_Local_Name is 0x03 0x0013 (248-byte payload!) and
# HCI_Write_Scan_Enable is 0x03 0x001A.  Use `hciconfig hci0 name/piscan`
# rather than hand-built commands, or you will rename the device to garbage.
#
set -u

log() { echo "bt-mac-setup: $*" >&2; }

i=0
while [ $i -lt 30 ]; do
    [ -d /sys/class/bluetooth/hci0 ] && break
    sleep 1
    i=$((i + 1))
done

if [ ! -d /sys/class/bluetooth/hci0 ]; then
    log "hci0 never appeared, giving up"
    exit 1
fi

hciconfig hci0 up >/dev/null 2>&1

WLAN=$(cat /sys/class/net/wlan0/address 2>/dev/null) || {
    log "cannot read wlan0 address"
    exit 1
}
[ -n "$WLAN" ] || { log "wlan0 address empty"; exit 1; }

O1=$(echo "$WLAN" | cut -d: -f1)
O2=$(echo "$WLAN" | cut -d: -f2)
O3=$(echo "$WLAN" | cut -d: -f3)
O4=$(echo "$WLAN" | cut -d: -f4)
O5=$(echo "$WLAN" | cut -d: -f5)
LAST=$(printf '%d' "0x$(echo "$WLAN" | cut -d: -f6)")
NEW=$(( (LAST + 1) % 256 ))
log "wlan0=$WLAN -> bd_addr=$(printf '%s:%s:%s:%s:%s:%02X' "$O1" "$O2" "$O3" "$O4" "$O5" "$NEW")"

B6=$(printf '0x%02X' "$NEW")
B5="0x$O5"; B4="0x$O4"; B3="0x$O3"; B2="0x$O2"; B1="0x$O1"

if hcitool cmd 0x3f 0x0001 "$B6" "$B5" "$B4" "$B3" "$B2" "$B1" 2>&1 \
        | grep -q '01 01 FC 00'; then
    log "Write BD_ADDR accepted"
else
    log "WARNING: Write BD_ADDR did not return a clean Command Complete"
fi

hcitool cmd 0x03 0x0024 0x0C 0x01 0x00 >/dev/null 2>&1   # Class = Laptop
hciconfig hci0 name "tegra-ubuntu-0" >/dev/null 2>&1
hciconfig hci0 piscan >/dev/null 2>&1

log "final state:"
hciconfig -a 2>&1 | head -14
exit 0
HELPER
    chmod 755 "$MAC_HELPER"
    ok "installed $MAC_HELPER"

    step "3/4  write the fixed unit file"
    cat > "$CONF" <<'EOF'
description     "NVIDIA bluetooth/wifi init script"
#
# L4T on Xiaomi Mi Pad 1 (mocha / Tegra K1) - Bluetooth bring-up
#
# DO NOT re-add low-power mode.  The stock file had:
#     echo 1 > /proc/bluetooth/sleep/lpm
#     brcm_patchram_plus ... --enable_lpm --tosleep=50000 ...
# The LPM wake path does not work on this board: the BCM43341 sleeps and never
# answers another HCI command (every command, including HCI Reset, times out).
#
# DO NOT add --patchram: /lib/firmware/BCM4350C0.hcd targets a different chip
# and brcm_patchram_plus hangs on it forever.  It is not needed.
#
# post-start writes a valid BD_ADDR, because the controller otherwise reports
# the placeholder 43:50:C0:00:00:00 and other devices ignore its inquiry
# responses.
#
instance $SUBTASK

script
	exec /usr/sbin/rfkill unblock bluetooth
	exec /usr/sbin/brcm_patchram_plus --enable_hci --scopcm=0,2,0,0,0,0,0,0,0,0 --baudrate 115200 --no2bytes /dev/ttyTHS2
end script

post-start script
	/usr/local/sbin/bt-mac-setup.sh
end script
EOF
    chmod 644 "$CONF"
    ok "wrote $CONF"

    step "4/4  done - reboot to power-cycle the controller"
    echo
    echo "    sudo reboot"
    echo
    echo "Then check with:  sudo $0 --verify"
    echo "Expected: BD_ADDR 00:90:4C:... (derived from the WiFi MAC),"
    echo "          Name 'tegra-ubuntu-0', Class 0x00010c, UP RUNNING PSCAN ISCAN"
    ;;
esac
