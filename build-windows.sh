#!/bin/sh
# Cross-build the Windows client and stage it on the VM share.
#
#   ./build-windows.sh [share-dir]
#
# Builds build-windows/ (configuring it first if needed), then copies
# gamex86_64x.dll, q2prox.exe, install-windows.bat, fonts/ and pics/ to the share
# dir, which is /home/antti/share by default. Run install-windows.bat from
# there inside Windows to drop the files into the Steam AQtion install.
#
# Needs gcc-mingw-w64, nasm, meson and ninja. Every library is built from
# its subprojects/ wrap; the options are the ones .github/workflows/build.yml
# gives the mingw job.

set -e

SRCDIR=$(cd "$(dirname "$0")" && pwd)
BUILDDIR="$SRCDIR/build-windows"
SHAREDIR=${1:-/home/antti/share}

if [ ! -f "$BUILDDIR/build.ninja" ]; then
    echo "Configuring $BUILDDIR"
    meson setup "$BUILDDIR" "$SRCDIR" \
        --cross-file "$SRCDIR/x86_64-w64-mingw32-local.txt" \
        --auto-features=enabled \
        -Danticheat-server=true \
        -Davcodec=disabled \
        -Dclient-gtv=true \
        -Dpacketdup-hack=true \
        -Dtests=false \
        -Dvariable-fps=true \
        -Dwerror=false \
        -Daqtion-build=true \
        -Ddiscord-sdk=false \
        -Dsdl2=disabled \
        -Dx11=disabled \
        -Dwayland=disabled \
        -Dwindows-egl=true \
        -Dwrap_mode=forcefallback
fi

ninja -C "$BUILDDIR"

if [ ! -d "$SHAREDIR" ]; then
    echo "ERROR: share dir not found: $SHAREDIR" >&2
    exit 1
fi

# q2pro.exe is renamed on the way out; the Steam install already has a
# stock q2pro.exe and this build sits next to it.
# Both under names of their own - q2prox.exe, which looks for
# gamex86_64x.dll first - so nothing of the stock install is overwritten.
cp -f "$BUILDDIR/gamex86_64.dll" "$SHAREDIR/gamex86_64x.dll"
cp -f "$BUILDDIR/q2pro.exe"      "$SHAREDIR/q2prox.exe"
cp -f "$SRCDIR/install-windows.bat" "$SHAREDIR/install-windows.bat"
# The TrueType text reads its fonts from action/fonts; without them every
# *_font cvar quietly falls back to conchars
mkdir -p "$SHAREDIR/fonts"
cp -f "$SRCDIR"/action/fonts/*.ttf "$SRCDIR/action/fonts/OFL.txt" "$SHAREDIR/fonts/"
# jmod's spawnpoint marker; the game falls back to a crosshair ring without it
mkdir -p "$SHAREDIR/pics"
cp -f "$SRCDIR/action/pics/jmod_spawn.png" "$SHAREDIR/pics/"

echo "Staged in $SHAREDIR:"
ls -l "$SHAREDIR/gamex86_64x.dll" "$SHAREDIR/q2prox.exe" "$SHAREDIR/install-windows.bat" \
      "$SHAREDIR"/fonts/* "$SHAREDIR"/pics/*
