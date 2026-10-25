#!/bin/sh
#
# bt-mac-setup.sh - give the Broadcom controller a FIXED BD_ADDR at boot
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
# The firmware patch (--patchram) cannot fix this here: BCM4350C0.hcd targets
# a different chip and brcm_patchram_plus hangs on it forever.
#
# Instead we write the address with the Broadcom vendor command
#   Write BD_ADDR = 0xFC01  (ogf 0x3f, ocf 0x0001)
#
# The address is HARDCODED on purpose
# -----------------------------------
# An earlier version derived the Bluetooth address from the WiFi MAC (same
# OUI, last octet + 1).  That is wrong for this tablet: bcmdhd has no MAC in
# nvram, so it invents a different WiFi address on every boot ...
#
#     boot A: wlan0 = 00:90:4c:16:70:01
#     boot B: wlan0 = 00:90:4c:11:22:33
#
# ... which made the Bluetooth address change on every boot too, so every
# paired device (headsets, watches) had to be re-paired.
#
# The address below is therefore fixed.  It uses the NVIDIA/Epigram OUI
# 00:90:4C, the real vendor prefix of this combo chip.
#
# IMPORTANT: this script must NOT call `rfkill` anything.
# /etc/udev/rules.d/99-nv-wifibt.rules stops the nvwifibt job whenever the
# bluedroid_pm rfkill state flips, so touching rfkill here would kill the
# very job that runs us.  rforkill is handled by the unit file itself.
#
# The write lives in controller RAM, so this script runs on every boot from
# the post-start hook in /etc/init/nvwifibt.conf.
#
# NOTE on opcodes (a mistake here corrupts the device name):
#   HCI_Write_Local_Name   = ogf 0x03, ocf 0x0013   (248-byte payload!)
#   HCI_Write_Scan_Enable  = ogf 0x03, ocf 0x001A
#   HCI_Write_Class        = ogf 0x03, ocf 0x0024
# Use `hciconfig hci0 name ...` and `hciconfig hci0 piscan` rather than raw
# commands for name/scan, so the payload is built correctly.
#
set -u

# ---------------------------------------------------------------------------
# The fixed Bluetooth address for this tablet.
BT_MAC="00:90:4C:16:70:02"

# Optional override: if this file exists its first line wins.
BT_MAC_FILE="/etc/bt-mac"
# ---------------------------------------------------------------------------

log() { echo "bt-mac-setup: $*" >&2; }

if [ -r "$BT_MAC_FILE" ]; then
    override=$(head -1 "$BT_MAC_FILE" 2>/dev/null | tr -d ' \t\r')
    if [ -n "${override:-}" ]; then
        BT_MAC="$override"
        log "using override from $BT_MAC_FILE"
    fi
fi

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

# --- bring it up (retry: the kernel may still be initialising)
i=0
while [ $i -lt 5 ]; do
    hciconfig hci0 up 2>/dev/null && break
    sleep 1
    i=$((i + 1))
done

if ! hciconfig hci0 2>/dev/null | grep -q 'UP RUNNING'; then
    log "WARNING: hci0 did not come up"
    hciconfig hci0 2>&1 | head -4
    exit 1
fi

# --- parse the hardcoded address, no runtime derivation
O1=$(echo "$BT_MAC" | cut -d: -f1)
O2=$(echo "$BT_MAC" | cut -d: -f2)
O3=$(echo "$BT_MAC" | cut -d: -f3)
O4=$(echo "$BT_MAC" | cut -d: -f4)
O5=$(echo "$BT_MAC" | cut -d: -f5)
O6=$(echo "$BT_MAC" | cut -d: -f6)

if [ -z "$O6" ]; then
    log "malformed BT_MAC='$BT_MAC', giving up"
    exit 1
fi

log "fixed bd_addr=$BT_MAC"

# --- HCI expects BD_ADDR little-endian (least significant octet first)
B6="0x$O6"
B5="0x$O5"
B4="0x$O4"
B3="0x$O3"
B2="0x$O2"
B1="0x$O1"

if hcitool cmd 0x3f 0x0001 "$B6" "$B5" "$B4" "$B3" "$B2" "$B1" 2>&1 \
        | grep -q '01 01 FC 00'; then
    log "Write BD_ADDR accepted"
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
