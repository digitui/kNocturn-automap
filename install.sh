#!/bin/sh
# Installs NightCapBridge (USB driver + HUD) and the Ableton Live remote script.
#   sh install.sh                  driver + Live script   (needs only Xcode Command Line Tools)
#   sh install.sh --with-wrapper   also builds the optional kNocTurn NightCap AU/VST3 wrapper plug-ins (needs CMake)
set -e
cd "$(dirname "$0")"

echo "== NightCapBridge (USB driver + HUD)"
sh bridge/build.sh
mkdir -p "$HOME/Applications/NightCapBridge"
cp bridge/NightCapBridge "$HOME/Applications/NightCapBridge/"
echo "   installed to ~/Applications/NightCapBridge/NightCapBridge"

echo "== Ableton Live remote script"
DEST="$HOME/Music/Ableton/User Library/Remote Scripts/kNocTurn"
mkdir -p "$DEST"
rm -rf "$DEST/__pycache__"
cp live-script/kNocTurn/*.py "$DEST/"
echo "   installed to $DEST"

# Upgrade from the earlier "Nocturn" version of this script (identified by its Automap.py):
# keep its learned mappings, then remove it so Live doesn't list two control surfaces.
OLD="$HOME/Music/Ableton/User Library/Remote Scripts/Nocturn"
if [ -f "$OLD/Automap.py" ]; then
    if [ -d "$OLD/mappings" ]; then
        mkdir -p "$DEST/mappings"
        for f in "$OLD/mappings/"*.json; do
            [ -f "$f" ] || continue
            n=$(basename "$f" .json)
            # file names are "<kind> - <device>.json"; the device part is unchanged
            [ -f "$DEST/mappings/$n.json" ] || cp "$f" "$DEST/mappings/$n.json"
        done
        echo "   kept your learned mappings from the previous version"
    fi
    rm -rf "$OLD"
    echo "   removed the previous 'Nocturn' version of this script (now called kNocTurn)"
fi
for old in NocturnBridge kNocTurnBridge; do
    if [ -d "$HOME/Applications/$old" ]; then
        echo "   note: a previous driver is still at ~/Applications/$old - quit it and delete that folder"
    fi
done

if [ "$1" = "--with-wrapper" ]; then
    echo "== kNocTurn NightCap wrapper plug-ins (AU + VST3) - first build downloads JUCE"
    command -v cmake >/dev/null 2>&1 || { echo "   CMake not found: brew install cmake"; exit 1; }
    cmake -S wrapper -B wrapper/build -DCMAKE_BUILD_TYPE=Release
    cmake --build wrapper/build --config Release -j "$(sysctl -n hw.ncpu)"
    killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true
    echo "   plug-ins copied to ~/Library/Audio/Plug-Ins/Components and ~/Library/Audio/Plug-Ins/VST3"
fi

echo
echo "Done. Start the driver with:  ~/Applications/NightCapBridge/NightCapBridge"
echo "Then in Live: Settings > Link, Tempo & MIDI > Control Surface 'kNocTurn', Input 'kNocTurn', Output 'kNocTurn'."
echo "(If you used the earlier 'Nocturn' version, re-select these in Live: the names have changed.)"
