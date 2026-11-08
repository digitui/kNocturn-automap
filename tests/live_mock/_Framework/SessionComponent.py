class SessionComponent(object):
    def __init__(self, *a): pass
    def set_mixer(self, m): pass
    def set_track_bank_buttons(self, r, l): self.bank = (r, l)
    def track_offset(self): return 0
