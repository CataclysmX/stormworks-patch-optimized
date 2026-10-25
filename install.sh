#!/bin/bash
# stwpatch installer for the Steam (Proton) Stormworks install.
#   ./install.sh install [server] | uninstall | status
set -e
GAME="${STORMWORKS_DIR:-$HOME/.local/share/Steam/steamapps/common/Stormworks}"
HERE="$(cd "$(dirname "$0")" && pwd)"


# $1 = DLL base name, $2 = minimum size of the real DLL (the patch is much smaller)
proxy_install() {
    local n=$1 min=$2
    if [ -f "$GAME/${n}_orig.dll" ]; then
        if [ "$(stat -c %s "$GAME/${n}_orig.dll")" -lt "$min" ]; then
            echo "error: ${n}_orig.dll is a copy of the patch, the real ${n}.dll is gone."
            echo "fix: delete it, run Steam > Properties > Installed files > Verify integrity, then reinstall."
            exit 1
        fi
    else
        if [ "$(stat -c %s "$GAME/${n}.dll")" -lt "$min" ]; then
            echo "error: ${n}.dll is already the patch but ${n}_orig.dll is missing."
            echo "fix: run Steam > Properties > Installed files > Verify integrity, then reinstall."
            exit 1
        fi
        mv "$GAME/${n}.dll" "$GAME/${n}_orig.dll"
    fi
    cp "$HERE/patch/build/${n}.dll" "$GAME/${n}.dll"
    echo "  ${n}.dll patched (original kept as ${n}_orig.dll)"
}

proxy_uninstall() {
    local n=$1 min=$2
    [ -f "$GAME/${n}_orig.dll" ] || return 0
    if [ "$(stat -c %s "$GAME/${n}_orig.dll")" -lt "$min" ]; then
        echo "error: ${n}_orig.dll is not the original; delete it and verify files in Steam"; return 1
    fi
    mv -f "$GAME/${n}_orig.dll" "$GAME/${n}.dll"
    echo "  ${n}.dll restored"
}

# steam_api64.dll is SHA-1 checked by the game (anti-piracy): an older stwpatch proxied it.
# Put the original back if needed and remove the leftover copy.
steam_api_cleanup() {
    [ -f "$GAME/steam_api64_orig.dll" ] || return 0
    if [ "$(stat -c %s "$GAME/steam_api64.dll" 2>/dev/null || echo 0)" -lt 250000 ]; then
        mv -f "$GAME/steam_api64_orig.dll" "$GAME/steam_api64.dll"
        echo "  steam_api64.dll: original restored (the old proxy triggered the game's anti-piracy check)"
    else
        rm -f "$GAME/steam_api64_orig.dll"
        echo "  removed leftover steam_api64_orig.dll"
    fi
}

running() { grep -qil "^stormworks\|^server64" /proc/[0-9]*/comm 2>/dev/null; }

case "$1" in
install)
    running && { echo "close the game / server first"; exit 1; }
    make -s -C "$HERE/patch"
    steam_api_cleanup
    proxy_install OpenAL64 1000000                        # client: stormworks64.exe
    if [ "$2" = "server" ]; then                          # dedicated server: server64.exe
        cp "$HERE/patch/build/opengl32.dll" "$GAME/opengl32.dll"
        echo "  opengl32.dll added (server64.exe loads stwpatch through it; the game ships no opengl32.dll)"
    fi
    [ -f "$GAME/stwpatch.ini" ] || cp "$HERE/patch/stwpatch.ini" "$GAME/stwpatch.ini"
    echo "installed. config: $GAME/stwpatch.ini   logs: stwpatch.log (game), stwpatch_server.log (server)"
    echo "note: a Steam 'verify files' or game update restores the original DLLs; just reinstall."
    ;;
uninstall)
    running && { echo "close the game / server first"; exit 1; }
    proxy_uninstall OpenAL64 1000000
    steam_api_cleanup
    [ -f "$GAME/opengl32.dll" ] && rm -f "$GAME/opengl32.dll" && echo "  opengl32.dll removed"
    ;;
status)
    [ -f "$GAME/OpenAL64_orig.dll" ] && echo "client (OpenAL64.dll): patched" || echo "client: not patched"
    [ -f "$GAME/opengl32.dll" ] && echo "server (opengl32.dll): patched" || echo "server: not patched"
    [ -f "$GAME/steam_api64_orig.dll" ] && echo "WARNING: steam_api64_orig.dll present, run install again to clean it"
    [ -f "$GAME/stwpatch.log" ] && { echo "--- last log:"; tail -20 "$GAME/stwpatch.log"; }
    ;;
*)
    echo "usage: $0 install [server] | uninstall | status"; exit 1 ;;
esac
