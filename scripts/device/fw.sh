#!/bin/sh
while true; do
  ts=$(date +%H:%M:%S)
  up=$(cut -d. -f1 /proc/uptime)
  last=$(dmesg | grep D2QATM | tail -1 | grep -oE '\[ *[0-9]+' | tr -d ' [')
  [ -z "$last" ] && last=$up
  for t in 133 187; do
    p=$(ps axo pid,args | grep "crtc_commit:$t" | grep -v grep | awk '{print $1}' | head -1)
    if [ -n "$p" ]; then
      fn=$(sed -n '3p' /proc/$p/stack 2>/dev/null | sed 's/.*] //')
      echo "$ts age=$((up-last))s pid=$p crtc$t=$fn" >> /userdata/fw.log
    fi
  done
  sleep 4
done
