MIDI_CC_TYPE = 1
SENT = []
class InputControlElement(object):
    def __init__(self, msg_type, channel, ident, *a):
        self.cc = ident; self.listeners = []; self.mapped = None; self.name = ''
    def add_value_listener(self, cb): self.listeners.append(cb)
    def receive(self, v):
        for cb in self.listeners: cb(v)
    def connect_to(self, p): self.mapped = p
    def release_parameter(self): self.mapped = None
    def send_value(self, v, force=False): SENT.append((self.cc, v))
