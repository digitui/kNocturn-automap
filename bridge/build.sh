#!/bin/sh
# Builds NightCapBridge as a universal (arm64 + x86_64) binary. Needs Xcode Command Line Tools.
set -e
cd "$(dirname "$0")"
clang -O2 -Wall -Wno-deprecated-declarations -fobjc-arc -arch arm64 -arch x86_64 -mmacosx-version-min=12.0 \
  -o NightCapBridge main_macos.m knocturn_core.c \
  -framework Cocoa -framework CoreFoundation -framework IOKit -framework CoreMIDI
codesign -s - -f NightCapBridge >/dev/null 2>&1 || true
echo "Built bridge/NightCapBridge"
