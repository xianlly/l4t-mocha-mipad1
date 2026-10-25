#!/bin/bash
# fix-login-greeter.sh
# Repair the LightDM / unity-greeter stack on L4T + Ubuntu 16.04 (Xiaomi Mi Pad 1).
#
# Fixes four independent problems that cause:
#   - black login screen
#   - login screen with wrong (plain GTK) appearance
#   - desktop session exiting back to the login screen ~30 s after login
#
# Safe to re-run. Run ON THE DEVICE with sudo.
#
# Usage: sudo ./fix-login-greeter.sh [--autologin|--greeter-only]
set -eu

MODE="${1:---autologin}"
[ "$(id -u)" = "0" ] || { echo "ERROR: run as root (sudo)"; exit 2; }

step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

# ---------------------------------------------------------------- fix 1
step "1/5  create the missing org.gnome.SessionManager D-Bus service"
cat > /usr/share/dbus-1/services/org.gnome.SessionManager.service <<'EOF'
[D-BUS Service]
Name=org.gnome.SessionManager
Exec=/usr/bin/gnome-session
EOF
chmod 644 /usr/share/dbus-1/services/org.gnome.SessionManager.service
ls -l /usr/share/dbus-1/services/org.gnome.SessionManager.service

# ---------------------------------------------------------------- fix 2
step "2/5  install ConsoleKit (needed by unity-greeter)"
if ! dpkg -s consolekit >/dev/null 2>&1; then
    DEBIAN_FRONTEND=noninteractive apt-get install -y consolekit
else
    echo "consolekit already installed"
fi

# ---------------------------------------------------------------- fix 3
step "3/5  link window-stack-bridge -> hud-service (the 16.04 rename)"
HUD_DIR=/usr/lib/arm-linux-gnueabihf/hud
if [ ! -x "$HUD_DIR/hud-service" ]; then
    echo "ERROR: $HUD_DIR/hud-service missing - is the 'hud' package installed?" >&2
    exit 1
fi
ln -sf "$HUD_DIR/hud-service" "$HUD_DIR/window-stack-bridge"
mkdir -p /usr/lib/hud
ln -sf "$HUD_DIR/hud-service" /usr/lib/hud/window-stack-bridge
ls -l "$HUD_DIR/window-stack-bridge" /usr/lib/hud/window-stack-bridge

# verify against whatever path the upstart job actually uses
EXEC_PATH=$(grep -m1 '^exec' /usr/share/upstart/sessions/window-stack-bridge.conf 2>/dev/null | awk '{print $2}' || true)
if [ -n "${EXEC_PATH:-}" ]; then
    if [ -x "$EXEC_PATH" ]; then
        echo "upstart job target exists: $EXEC_PATH  OK"
    else
        echo "WARNING: upstart job wants $EXEC_PATH which still does not exist" >&2
    fi
fi

# ---------------------------------------------------------------- fix 4
step "4/5  configure lightdm (highest-priority file wins)"
if [ "$MODE" = "--greeter-only" ]; then
    cat > /etc/lightdm/lightdm.conf <<'EOF'
[Seat:*]
greeter-session=unity-greeter
EOF
else
    gpasswd -a ubuntu nopasswdlogin 2>/dev/null || true
    cat > /etc/lightdm/lightdm.conf <<'EOF'
[Seat:*]
autologin-user=ubuntu
autologin-user-timeout=0
autologin-session=ubuntu
greeter-session=lightdm-gtk-greeter
EOF
fi
cat /etc/lightdm/lightdm.conf

# the gtk greeter drops a 60-*.conf that outranks unity's 50-*.conf
if [ -f /usr/share/lightdm/lightdm.conf.d/60-lightdm-gtk-greeter.conf ]; then
    mv /usr/share/lightdm/lightdm.conf.d/60-lightdm-gtk-greeter.conf \
       /usr/share/lightdm/lightdm.conf.d/60-lightdm-gtk-greeter.conf.disabled
    echo "disabled 60-lightdm-gtk-greeter.conf (it hijacked the greeter choice)"
fi

# ---------------------------------------------------------------- verify
step "5/5  verification"
echo "--- greeter-session sources (last one wins) ---"
grep -rhE '^greeter-session' /usr/share/lightdm/lightdm.conf.d/ /etc/lightdm/ 2>/dev/null || true
echo ""
echo "--- nopasswdlogin ---"
getent group nopasswdlogin || true
echo ""
echo "--- current session (before reboot) ---"
who || true

cat <<'EOF'

DONE.

Next step: reboot, then verify:
  who
    -> expect: ubuntu  tty7  ... (:0)
  ps -ef | grep -E "[c]ompiz|[u]nity-settings"
    -> expect: both running
  sleep 60 && who
    -> the session must still be there (the regression was ~32 s)

If the session still dies, check:
  sudo tail -30 /home/ubuntu/.xsession-errors
  sudo strings /var/log/lightdm/lightdm.log | grep -E "Session stopped|Exited with"
EOF
