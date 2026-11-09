# kNocTurn control surface (for the Novation Nocturn controller) for Ableton Live 11/12 (Apple Silicon).
# Requires NightCapBridge running. In Live: Settings > Link, Tempo & MIDI >
# Control Surface "kNocTurn", Input "kNocTurn", Output "kNocTurn".
from .kNocTurn import kNocTurn


def create_instance(c_instance):
    return kNocTurn(c_instance)
