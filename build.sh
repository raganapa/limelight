#!/usr/bin/env bash
# Cross-compiles Limelight for Windows (x64) on Linux with mingw-w64,
# then builds the installer with NSIS.
#   sudo apt install g++-mingw-w64-x86-64-posix nsis
#   ./build.sh            -> dist/Limelight.exe and dist/Limelight-Setup.exe
#   ./build.sh test       -> also builds build/test_presenter.exe (run it on Windows or with wine)
set -euo pipefail
cd "$(dirname "$0")"

# Linux cross-compiler names first; plain names work in an MSYS2 MINGW64 shell on Windows.
pick() { for c in "$@"; do command -v "$c" >/dev/null && { echo "$c"; return; }; done; echo "$1"; }
CXX=${CXX:-$(pick x86_64-w64-mingw32-g++-posix x86_64-w64-mingw32-g++ g++)}
WINDRES=${WINDRES:-$(pick x86_64-w64-mingw32-windres windres)}

mkdir -p build dist
"$WINDRES" -I res res/limelight.rc -O coff -o build/res.o

CXXFLAGS="-std=c++17 -O2 -municode -mwindows -Wall -Wextra -Wno-unused-parameter \
  -Wno-missing-field-initializers -Wno-cast-function-type -DUNICODE -D_UNICODE"
LIBS="-lgdiplus -ldwmapi -lshcore -lhid -lsetupapi -ldxgi -lcomctl32 -lole32 -luuid \
  -lwinmm -lshell32 -luser32 -lgdi32 -ladvapi32"

"$CXX" $CXXFLAGS src/*.cpp build/res.o -o dist/Limelight.exe \
  -static -static-libgcc -static-libstdc++ -s $LIBS

echo "built dist/Limelight.exe ($(du -h dist/Limelight.exe | cut -f1))"

if [ "${1:-}" = "test" ]; then
  "$CXX" -std=c++17 -O1 -DUNICODE -D_UNICODE tests/test_presenter.cpp tests/mock_hid.cpp \
    src/presenter.cpp src/hidpp.cpp src/hid_key.cpp src/log.cpp -o build/test_presenter.exe \
    -static -lole32 -lshell32 -luser32 -luuid
  echo "built build/test_presenter.exe"
  "$CXX" -std=c++17 -O1 -DUNICODE -D_UNICODE -DLIMELIGHT_DIAG_TEST tests/test_diag.cpp \
    tests/mock_hid.cpp src/diag.cpp src/hidpp.cpp src/hid_key.cpp src/log.cpp src/settings.cpp \
    -o build/test_diag.exe -static -lole32 -lshell32 -luser32 -luuid -ladvapi32
  echo "built build/test_diag.exe"
fi

if command -v makensis >/dev/null; then
  makensis -V2 -NOCD installer/limelight.nsi
  echo "built dist/Limelight-Setup.exe ($(du -h dist/Limelight-Setup.exe | cut -f1))"
fi
