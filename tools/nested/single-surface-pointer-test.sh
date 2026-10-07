#!/bin/bash
# HELIX_NESTED_SCRIPT: pointer and keyboard behaviour of a layer surface that hosts a bar and one panel (click-through, scrim, focus).
# Needs tools/nested/wl-pointer/build.sh first. The program under test must log "SET_OPEN true|false" when the panel opens or closes.
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); N=$ROOT/tools/nested
LOG=$SCRATCH/program.log; PROBE=$SCRATCH/probe.log; PT="$ROOT/tools/nested/wl-pointer/wl-pointer"
python3 "$N/probe_window.py" > $PROBE 2>/dev/null &
PROBEPID=$!
sleep 4
mark() { echo "--- $1"; }
c() { "$PT" "$1" "$2" click; sleep 0.8; }
n_open() { grep -c "SET_OPEN" $LOG; }
mark "1. click on empty area, panel closed (should reach the window below)"
c 0.5 0.6;  grep -c "PROBE button" $PROBE | sed 's/^/probe buttons: /'
mark "2. click Assistant in the bar (should open the panel)"
c 0.959 0.022; grep "SET_OPEN" $LOG | tail -1
mark "3. type abc while open (must not reach the window below)"
K0=$(grep -c "PROBE key" $PROBE); wtype abc; sleep 0.8; K1=$(grep -c "PROBE key" $PROBE); echo "probe keys while open: $((K1-K0))"
NIRI_SOCKET=$NIRI_SOCKET niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t $SCRATCH/shots/*.png | head -1 | xargs -I{} cp {} $SCRATCH/open-typed.png
mark "4. click inside the panel (must stay open)"
O0=$(n_open); c 0.5 0.373; echo "set_open calls caused: $(( $(n_open) - O0 ))"
mark "5. click outside the panel (scrim: closes, and must NOT reach the window below)"
B0=$(grep -c "PROBE button" $PROBE); c 0.2 0.8; grep "SET_OPEN" $LOG | tail -1; echo "probe buttons caused: $(( $(grep -c 'PROBE button' $PROBE) - B0 ))"
mark "6. type x after closing (should reach the window below)"
K0=$(grep -c "PROBE key" $PROBE); wtype x; sleep 0.8; echo "probe keys after close: $(( $(grep -c 'PROBE key' $PROBE) - K0 ))"
mark "7. click empty area again (click-through restored)"
B0=$(grep -c "PROBE button" $PROBE); c 0.5 0.6; echo "probe buttons caused: $(( $(grep -c 'PROBE button' $PROBE) - B0 ))"
mark "8. reopen, then Escape (closes)"
c 0.959 0.022; wtype -k Escape; sleep 0.8; grep "SET_OPEN" $LOG | tail -2 | tr '\n' ' '; echo
mark "9. final keyboard check"
K0=$(grep -c "PROBE key" $PROBE); wtype y; sleep 0.8; echo "probe keys after Escape-close: $(( $(grep -c 'PROBE key' $PROBE) - K0 ))"
kill $PROBEPID 2>/dev/null
