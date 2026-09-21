#!/bin/sh
# Wrapper around tools/dep-pins.cmake so the workflows and make-prebuilt.sh
# don't each spell out the cmake -P call.
set -eu
exec cmake -P "$(dirname "$0")/dep-pins.cmake"
