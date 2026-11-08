#!/bin/sh
# d2q phosh session launcher — replaces the fragile phoc -E "bash -lc gnome-session"
# spawn (whose -E child silently never runs under systemd on this box; a manual
# gnome-session against the running compositor works every time).
export XDG_RUNTIME_DIR=/run/user/1000
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
export HOME=/home/d2q
unset WAYLAND_DISPLAY
export WLR_BACKENDS=drm,libinput

rm -f /run/user/1000/wayland-0 /run/user/1000/wayland-0.lock

/usr/bin/phoc -C /usr/share/phosh/phoc.ini &
PHOC=$!

i=0
while [ $i -lt 100 ]; do
    [ -S /run/user/1000/wayland-0 ] && break
    kill -0 $PHOC 2>/dev/null || exit 1
    sleep 0.2
    i=$((i + 1))
done

trap 'kill $PHOC 2>/dev/null' EXIT INT TERM
export WAYLAND_DISPLAY=wayland-0
exec gnome-session --disable-acceleration-check --session=phosh --builtin
