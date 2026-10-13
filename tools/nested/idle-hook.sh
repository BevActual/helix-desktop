#!/bin/bash
# A stand-in for the lock and screen commands in nested tests: records that it was called.
echo "$1" >> "${IDLE_HOOK_LOG:?}"
