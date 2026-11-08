# Novation Nocturn control surface for Ableton Live 11/12 (Apple Silicon).
# Requires NocturnBridge running. In Live: Settings > Link, Tempo & MIDI >
# Control Surface "Nocturn", Input "Nocturn", Output "Nocturn".
from .Nocturn import Nocturn


def create_instance(c_instance):
    return Nocturn(c_instance)
