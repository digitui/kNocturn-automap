class Strip(object):
    """Like Live's ChannelStripComponent: assigning a control connects it to the track volume."""
    def __init__(self, i):
        self.vol = None; self.mute = None
        self.volume_param = 'volume of strip %d' % i
    def set_volume_control(self, c):
        if self.vol is not None and self.vol is not c:
            self.vol.release_parameter()
        self.vol = c
        if c is not None:
            c.connect_to(self.volume_param)
    def set_mute_button(self, b): self.mute = b
class MixerComponent(object):
    def __init__(self, n): self.strips = [Strip(i) for i in range(n)]; self.xf = None
    def channel_strip(self, i): return self.strips[i]
    def set_crossfader_control(self, c): self.xf = c
