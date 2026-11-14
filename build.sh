#!/bin/sh
set -e
mkdir -p out
x86_64-w64-mingw32-windres res/app.rc -O coff -o out/res.o
x86_64-w64-mingw32-g++ -std=c++17 -O2 -s -municode -mwindows -static -static-libgcc -static-libstdc++ \
  -finput-charset=UTF-8 -fexec-charset=UTF-8 \
  src/main.cpp out/res.o -o out/PopupDesktop.exe \
  -lgdiplus -lshell32 -lole32 -luuid -lshlwapi -lcomdlg32 -lgdi32 -luser32 -ladvapi32
ls -la out/PopupDesktop.exe
