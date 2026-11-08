class Strip(object):
    def set_volume_control(self, c): self.vol = c
    def set_mute_button(self, b): self.mute = b
class MixerComponent(object):
    def __init__(self, n): self.strips = [Strip() for _ in range(n)]; self.xf = None
    def channel_strip(self, i): return self.strips[i]
    def set_crossfader_control(self, c): self.xf = c
