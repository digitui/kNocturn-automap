# NightCap control surface (for the Novation Nocturn controller) for Ableton Live 11/12 (Apple Silicon).
# Requires NightCapBridge running. In Live: Settings > Link, Tempo & MIDI >
# Control Surface "NightCap", Input "NightCap", Output "NightCap".
from .NightCapSurface import NightCapSurface


def create_instance(c_instance):
    return NightCapSurface(c_instance)
