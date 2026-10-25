#!/bin/sh
#
# bt-mac-setup.sh - give the Broadcom controller a valid BD_ADDR at boot
#
# Why this exists
# ---------------
# On this board (Xiaomi Mi Pad 1 / Tegra K1 / BCM43341 over UART) the
# controller comes up with a placeholder address:
#
#     BD Address: 43:50:C0:00:00:00
#
# Other devices receive inquiry responses carrying that bogus address and
# silently drop them, so the tablet looks "invisible" even though
# hciconfig reports "UP RUNNING PSCAN ISCAN".
#
# The firmware patch (--patchram) is NOT usable here: BCM4350C0.hcd targets a
# different chip and brcm_patchram_plus hangs forever on it.
#
# Instead we write the address with the Broadcom vendor command
#   Write BD_ADDR = 0xFC01  (ogf 0x3f, ocf 0x0001)
# The value is derived from the WiFi MAC (same OUI, last octet + 1).
#
# The write lives in controller RAM, so it must be re-applied on every boot --
# that is exactly what this script is for.  It is called from the post-start
# section of /etc/init/nvwifibt.conf.
#
# NOTE on opcodes (a mistake here corrupts the device name):
#   HCI_Write_Local_Name   = ogf 0x03, ocf 0x0013   (248-byte payload!)
#   HCI_Write_Scan_Enable  = ogf 0x03, ocf 0x001A
#   HCI_Write_Class        = ogf 0x03, ocf 0x0024
# Use `hciconfig hci0 name ...` and `hciconfig hci0 piscan` rather than raw
# commands for name/scan, so the payload is built correctly.
#
set -u

log() { echo "bt-mac-setup: $*" >&2; }

# --- wait for hci0 to appear (brcm_patchram_plus creates it asynchronously)
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

# --- derive the Bluetooth address from the WiFi address
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
BTMAC=$(printf '%s:%s:%s:%s:%s:%02X' "$O1" "$O2" "$O3" "$O4" "$O5" "$NEW")
log "wlan0=$WLAN -> bd_addr=$BTMAC"

# --- HCI expects BD_ADDR little-endian (least significant octet first)
B6=$(printf '0x%02X' "$NEW")
B5="0x$O5"
B4="0x$O4"
B3="0x$O3"
B2="0x$O2"
B1="0x$O1"

if hcitool cmd 0x3f 0x0001 "$B6" "$B5" "$B4" "$B3" "$B2" "$B1" 2>&1 \
        | grep -q '01 01 FC 00'; then
    log "Write BD_ADDR accepted (Command Complete, status 0x00)"
else
    log "WARNING: Write BD_ADDR did not return a clean Command Complete"
fi

# Class of Device: 0x00010C = Computer / Laptop
hcitool cmd 0x03 0x0024 0x0C 0x01 0x00 >/dev/null 2>&1

# Local name (hciconfig builds the 248-byte payload correctly)
hciconfig hci0 name "tegra-ubuntu-0" >/dev/null 2>&1

# Inquiry scan + page scan (discoverable + connectable)
hciconfig hci0 piscan >/dev/null 2>&1

# --- report (goes to the upstart job log)
log "chip reports:"
hcitool cmd 0x04 0x0009 2>&1 | grep -A1 'HCI Event' | tail -2
log "kernel view:"
hciconfig -a 2>&1 | head -14

exit 0
