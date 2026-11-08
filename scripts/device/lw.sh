#!/bin/sh
while true; do
  ts=$(date +%H:%M:%S)
  p=$(pgrep -x labwc | head -1)
  if [ -n "$p" ]; then
    for t in /proc/$p/task/*; do
      tid=$(basename "$t")
      st=$(awk '{print $3}' "$t/stat" 2>/dev/null)
      fn=$(sed -n '2p' "$t/stack" 2>/dev/null | sed 's/.*] //')
      wc=$(cut -c1-30 "$t/wchan" 2>/dev/null)
      echo "$ts labwc tid=$tid st=$st wchan=$wc stack=$fn" >> /userdata/lw.log
    done
  fi
  sleep 4
done
