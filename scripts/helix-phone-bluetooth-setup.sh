#!/bin/sh
# Apply the bluetoothd settings Helix Phone (iPhone AssistiveTouch pointer) needs.
# Idempotent. Run as root (the shell launches it through pkexec).
#
#   [General] Privacy = on              own resolvable address for the HID identity
#   [General] JustWorksRepairing = always  iOS may re-pair after forgetting the device
set -eu

CONF=/etc/bluetooth/main.conf

if [ "$(id -u)" -ne 0 ]; then
  echo "run as root" >&2
  exit 1
fi

[ -f "$CONF" ] || printf '[General]\n' > "$CONF"

set_key() {
  section=$1 key=$2 value=$3
  tmp=$(mktemp)
  awk -v section="$section" -v key="$key" -v value="$value" '
    BEGIN { in_section = 0; done = 0; seen_section = 0 }
    /^\[.*\]$/ {
      if (in_section && !done) { print key " = " value; done = 1 }
      in_section = ($0 == "[" section "]")
      if (in_section) seen_section = 1
      print; next
    }
    in_section && $0 ~ "^[#[:space:]]*" key "[[:space:]]*=" {
      if (!done) { print key " = " value; done = 1 }
      next
    }
    { print }
    END {
      if (!seen_section) { print ""; print "[" section "]" }
      if (!done) print key " = " value
    }
  ' "$CONF" > "$tmp"
  if cmp -s "$tmp" "$CONF"; then
    rm -f "$tmp"
    return 1
  fi
  cat "$tmp" > "$CONF"
  rm -f "$tmp"
  return 0
}

changed=0
set_key General Privacy on && changed=1
set_key General JustWorksRepairing always && changed=1

if [ "$changed" -eq 1 ]; then
  systemctl restart bluetooth.service
  echo "bluetooth configured for Helix Phone and restarted"
else
  echo "bluetooth already configured for Helix Phone"
fi
