#!/bin/bash
# Stands in for `pw-dump --monitor`: prints batches of changes, each when the test creates the next step file ($XDG_STATE_HOME/pw-step-N), so a test
# can watch the shell's indicator follow in a known order. Real PipeWire is not touched.
node() { printf '[{"id":%s,"type":"PipeWire:Interface:Node","info":{"state":"%s","props":{"media.class":"%s","application.name":"%s"}}}]\n' "$1" "$2" "$3" "$4"; }
step() { until [ -e "$XDG_STATE_HOME/pw-step-$1" ]; do sleep 0.2; done; }
node 40 running Stream/Output/Audio Music
step 1; node 41 running Stream/Input/Audio Zoom
step 2; node 42 running Stream/Input/Video Cheese
step 3; printf '[{"id":41,"info":null}]\n'
step 4; printf '[{"id":42,"info":null}]\n'
sleep 600
