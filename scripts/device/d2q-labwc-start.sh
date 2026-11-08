#!/bin/sh
# d2q labwc session launcher (wlroots 0.17.4 / labwc 0.7.4, Vulkan renderer on kgsl)
export XDG_RUNTIME_DIR=/run/user/1000
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
export HOME=/home/d2q
unset WAYLAND_DISPLAY
export WLR_BACKENDS=drm,libinput
export WLR_RENDERER=vulkan
export VK_ICD_FILENAMES=/opt/mesa-kgsl/share/vulkan/icd.d/freedreno_icd.aarch64.json

rm -f /run/user/1000/wayland-0 /run/user/1000/wayland-0.lock
exec /usr/local/bin/labwc
