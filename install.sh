#!/bin/sh
# Installs NightCapBridge (USB driver + HUD) and the Ableton Live remote script.
#   sh install.sh                  driver + Live script   (needs only Xcode Command Line Tools)
#   sh install.sh --with-wrapper   also builds the optional NightCap AU/VST3 wrapper plug-ins (needs CMake)
set -e
cd "$(dirname "$0")"

echo "== NightCapBridge (USB driver + HUD)"
sh bridge/build.sh
mkdir -p "$HOME/Applications/NightCapBridge"
cp bridge/NightCapBridge "$HOME/Applications/NightCapBridge/"
echo "   installed to ~/Applications/NightCapBridge/NightCapBridge"

echo "== Ableton Live remote script"
DEST="$HOME/Music/Ableton/User Library/Remote Scripts/NightCap"
mkdir -p "$DEST"
rm -rf "$DEST/__pycache__"
cp live-script/NightCap/*.py "$DEST/"
echo "   installed to $DEST"

# Upgrade from earlier versions of this script, which were called "Nocturn" (with Automap.py)
# and then "kNocTurn" (with kNocTurn.py): keep their learned mappings, then remove them so
# Live doesn't list duplicate control surfaces.
for name in kNocTurn:kNocTurn.py Nocturn:Automap.py; do   # newest first: its mappings win
    OLD="$HOME/Music/Ableton/User Library/Remote Scripts/${name%%:*}"
    [ -f "$OLD/${name#*:}" ] || continue
    if [ -d "$OLD/mappings" ]; then
        mkdir -p "$DEST/mappings"
        for f in "$OLD/mappings/"*.json; do
            [ -f "$f" ] || continue
            [ -f "$DEST/mappings/$(basename "$f")" ] || cp "$f" "$DEST/mappings/"
        done
        echo "   kept your learned mappings from the previous '${name%%:*}' version"
    fi
    rm -rf "$OLD"
    echo "   removed the previous '${name%%:*}' version of this script (now called NightCap)"
done
for old in NocturnBridge kNocTurnBridge; do
    if [ -d "$HOME/Applications/$old" ]; then
        echo "   note: a previous driver is still at ~/Applications/$old - quit it and delete that folder"
    fi
done

if [ "$1" = "--with-wrapper" ]; then
    echo "== NightCap wrapper plug-ins (AU + VST3) - first build downloads JUCE"
    command -v cmake >/dev/null 2>&1 || { echo "   CMake not found: brew install cmake"; exit 1; }
    cmake -S wrapper -B wrapper/build -DCMAKE_BUILD_TYPE=Release
    cmake --build wrapper/build --config Release -j "$(sysctl -n hw.ncpu)"
    killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true
    echo "   plug-ins copied to ~/Library/Audio/Plug-Ins/Components and ~/Library/Audio/Plug-Ins/VST3"
fi

echo
echo "Done. Start the driver with:  ~/Applications/NightCapBridge/NightCapBridge"
echo "Then in Live: Settings > Link, Tempo & MIDI > Control Surface 'NightCap', Input 'NightCap', Output 'NightCap'."
echo "(If you used an earlier 'Nocturn' or 'kNocTurn' version, re-select these in Live: the names have changed.)"
