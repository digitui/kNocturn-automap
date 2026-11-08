#!/bin/sh
# Installs NocturnBridge (USB driver + HUD) and the Ableton Live remote script.
#   sh install.sh                  driver + Live script   (needs only Xcode Command Line Tools)
#   sh install.sh --with-wrapper   also builds the optional Nocturn Automap AU/VST3 wrapper plug-ins (needs CMake)
set -e
cd "$(dirname "$0")"

echo "== NocturnBridge (USB driver + HUD)"
sh bridge/build.sh
mkdir -p "$HOME/Applications/NocturnBridge"
cp bridge/NocturnBridge "$HOME/Applications/NocturnBridge/"
echo "   installed to ~/Applications/NocturnBridge/NocturnBridge"

echo "== Ableton Live remote script"
DEST="$HOME/Music/Ableton/User Library/Remote Scripts/Nocturn"
mkdir -p "$DEST"
rm -rf "$DEST/__pycache__"
cp live-script/Nocturn/*.py "$DEST/"
echo "   installed to $DEST"

# Old Novation Automap installs leave a "Nocturn" in Live's own User Remote Scripts folder.
# It has the same name and Live loads it instead of this script, so warn about it.
for d in "$HOME/Library/Preferences/Ableton/"Live*/"User Remote Scripts/Nocturn"; do
    if [ -d "$d" ]; then
        echo
        echo "   WARNING: an old Novation 'Nocturn' script exists at:"
        echo "     $d"
        echo "   Live will load it instead of this one. Move it somewhere else (e.g. your Desktop) and restart Live."
    fi
done

if [ "$1" = "--with-wrapper" ]; then
    echo "== Nocturn Automap wrapper plug-ins (AU + VST3) - first build downloads JUCE"
    command -v cmake >/dev/null 2>&1 || { echo "   CMake not found: brew install cmake"; exit 1; }
    cmake -S wrapper -B wrapper/build -DCMAKE_BUILD_TYPE=Release
    cmake --build wrapper/build --config Release -j "$(sysctl -n hw.ncpu)"
    killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true
    echo "   plug-ins copied to ~/Library/Audio/Plug-Ins/Components and ~/Library/Audio/Plug-Ins/VST3"
fi

echo
echo "Done. Start the driver with:  ~/Applications/NocturnBridge/NocturnBridge"
echo "Then in Live: Settings > Link, Tempo & MIDI > Control Surface 'Nocturn', Input 'Nocturn', Output 'Nocturn'."
