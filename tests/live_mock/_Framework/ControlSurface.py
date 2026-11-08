import contextlib
class ControlSurface(object):
    def __init__(self, c): self.messages = []; self.queue = []; self.midi = []; self._song = c
    def component_guard(self): return contextlib.nullcontext()
    def song(self): return self._song
    def set_device_component(self, d): pass
    def set_highlighting_session_component(self, s): pass
    def show_message(self, m): self.messages.append(m)
    def log_message(self, m): print('LOG', m)
    def schedule_message(self, n, cb): self.queue.append(cb)
    def request_rebuild_midi_map(self): pass
    def _send_midi(self, b): self.midi.append(b)
    def handle_sysex(self, b): pass
    def disconnect(self): pass
    def update_display(self): pass
    def tick(self):
        while self.queue:
            q, self.queue = self.queue, []
            for cb in q: cb()
