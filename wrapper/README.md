# Nocturn Automap wrapper plug-ins (optional)

AU + VST3 plug-ins (**Nocturn Automap** for instruments, **Nocturn Automap FX** for effects) that host another
plug-in and map all of its parameters to the Nocturn, with pages, learn, and a parameter display. They are only needed
for DAWs other than Ableton Live, or for plug-ins with more than 128 parameters (Live's limit for exposing parameters).

Build: `sh install.sh --with-wrapper` from the repository root (needs CMake; JUCE 8.0.4 is downloaded automatically),
or `cmake -S wrapper -B wrapper/build && cmake --build wrapper/build`.

**Licence:** this folder's source is MIT like the rest of the project, but it links JUCE, which is AGPLv3 (or commercial).
Distributing a binary built from it means following the AGPLv3 unless you have a JUCE licence.
