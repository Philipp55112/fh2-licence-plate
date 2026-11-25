#!/bin/sh
# Builds PlateLight.dll (x64 Windows) with zig as cross compiler: pip install ziglang
# LicencePlates by Philipp55112
set -e
cd "$(dirname "$0")"
Z="python3 -m ziglang"
for f in hook buffer trampoline; do $Z cc -target x86_64-windows-gnu -O2 -Iinclude -c minhook/src/$f.c -o $f.o; done
$Z cc -target x86_64-windows-gnu -O2 -Iinclude -c minhook/src/hde/hde64.c -o hde64.o
$Z c++ -target x86_64-windows-gnu -shared -O2 -std=c++17 -Iinclude platelight_eo.cpp plate_editor.cpp ext/bc7enc.cpp ext/rgbcx.cpp hook.o buffer.o trampoline.o hde64.o -o PlateLight.dll -lkernel32 -luser32 -static
echo "built PlateLight.dll"
