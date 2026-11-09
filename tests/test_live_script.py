"""Tests the Live remote script against a small mock of Live's Python API (tests/live_mock)."""
import os, shutil, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'live_mock'))
sys.path.insert(0, os.path.join(HERE, '..', 'live-script'))
from _Framework.InputControlElement import SENT

class Param(object):
    def __init__(self, name, mn=0.0, mx=1.0, q=False, default=None):
        self.name, self.min, self.max, self.is_quantized = name, mn, mx, q
        self._v = mn; self.listeners = []; self.is_enabled = True
        self.default_value = default if default is not None else mn
    @property
    def value(self): return self._v
    @value.setter
    def value(self, v):
        assert self.min <= v <= self.max, (self.name, v)
        self._v = v
        for cb in list(self.listeners): cb()
    def add_value_listener(self, cb): self.listeners.append(cb)
    def remove_value_listener(self, cb): self.listeners.remove(cb)
    def __str__(self): return '%.2f' % self._v

class Device(object):
    def __init__(self, name, n, cls='PluginDevice'):
        self.name = name; self.class_name = cls; self.class_display_name = name
        self.parameters = [Param('Device On')] + [Param('%s P%d' % (name, i)) for i in range(n)]
        self.pl = []
    def add_parameters_listener(self, cb): self.pl.append(cb)
    def remove_parameters_listener(self, cb): self.pl.remove(cb)

class TrackView(object): selected_device = None
class MixerDev(object):
    def __init__(self): self.volume = Param('Volume', 0.0, 1.0); self.volume._v = 0.85
class Track(object):
    _n = 0
    def __init__(self, devs):
        Track._n += 1; self.name = 'Track %d' % Track._n; self.mute = False
        self.devices = devs; self.view = TrackView(); self.mixer_device = MixerDev()
class SongView(object):
    def __init__(self, song): self.song = song; self.selected_track = None
    def select_device(self, d): self.selected_track.view.selected_device = d; self.song.appoint(d)
class Song(object):
    def __init__(self, tracks):
        self.tracks = tracks; self.visible_tracks = tracks; self.return_tracks = []; self.master_track = Track([])
        self.view = SongView(self); self.view.selected_track = tracks[0]; self.appointed_device = None; self.al = []
    def add_appointed_device_listener(self, cb): self.al.append(cb)
    def remove_appointed_device_listener(self, cb): self.al.remove(cb)
    def appoint(self, d):
        self.appointed_device = d
        for cb in self.al: cb()

import NightCap.NightCap as AM
AM.MAPPING_DIR = os.path.join(HERE, 'mappings_test')
shutil.rmtree(AM.MAPPING_DIR, ignore_errors=True)
from NightCap.NightCapSurface import NightCapSurface

serum = Device('Serum', 20); eq = Device('EQ Eight', 5, 'Eq8'); simpler = Device('Simpler', 7, 'OriginalSimpler')
song = Song([Track([serum, eq]), Track([simpler])])
s = NightCapSurface(song); s.tick()
assert s.midi[0] == (0xF0, 0x7D, 0x4B, 0x4E, 0x43, 1, 0, 0xF7)
def ok(msg): print('ok:', msg)

song.view.select_device(serum); s.tick()
am = s._nightcap
assert am.num_pages == 3 and s._encoders[0].mapped is serum.parameters[1]; ok('plug-in params paged: 20 params -> 3 pages, knob1 = P0')
s._top_buttons[2].receive(127); assert am.page == 2 and s._encoders[3].mapped is serum.parameters[20] and s._encoders[4].mapped is None
ok('page 3 maps P16..P19, empty slots released')
s._page_down.receive(127); assert am.page == 1; ok('page down')
s._touches[1].receive(127); assert 'Serum P9' in s.messages[-1]; ok('touch shows name/value: ' + s.messages[-1])
s._speed_dial.receive(5); assert abs(serum.parameters[10].value - 5/1270.) < 1e-9; ok('speed dial fine adjust')
del SENT[:]; serum.parameters[10].value = 0.5; assert (65, 64) in SENT; ok('LED ring follows param value')
s._view.receive(127); assert serum.parameters[10].value == 0.0; ok('View resets to default')

# learn: press+release Learn, touch knob 3, move P2 in the plug-in UI
s._learn.receive(127); s._learn.receive(0); assert am.learn_armed and s._encoders[0].mapped is None; ok('learn armed, knobs released')
s._touches[2].receive(127)
serum.parameters[3].value = 0.3; s.tick()
assert not am.learn_armed and s._encoders[2].mapped is serum.parameters[3] and am.page == 1; ok('learned P2 -> knob 3 on page 2')
assert s._encoders[0].mapped is serum.parameters[9]; ok('other knobs kept their parameters')
f = os.listdir(AM.MAPPING_DIR); assert f == ['PluginDevice - Serum.json'], f; ok('mapping saved: ' + f[0])

# crossfader learn
s._learn.receive(127); s._learn.receive(0); s._crossfader.receive(64); serum.parameters[5].value = 0.7; s.tick()
assert s._crossfader.mapped is serum.parameters[5] and s._mixer.xf is None; ok('crossfader learned to P4')

# switch device and back -> mapping reloaded from disk
song.view.select_device(eq); s.tick()
assert am.num_pages == 1 and s._encoders[0].mapped is eq.parameters[1] and s._crossfader.mapped is None and s._mixer.xf is s._crossfader
ok('other device: default layout, crossfader back to Live')
song.view.select_device(serum); s.tick()
assert am.page == 0; s._page_up.receive(127); assert s._encoders[2].mapped is serum.parameters[3]; ok('learned mapping restored from file')

# Live device uses Live curated bank first
song.view.select_device(simpler); s.tick()
assert s._encoders[0].mapped is simpler.parameters[6] and s._encoders[2].mapped is None and am.num_pages == 1
ok('Live device: curated bank used')

# lock
s._inst.receive(127); assert am.locked; song.view.select_device(eq); s.tick(); assert s._encoders[0].mapped is simpler.parameters[6]; ok('lock keeps device')
s._inst.receive(127); song.view.select_device(eq); s.tick(); assert s._encoders[0].mapped is eq.parameters[1]; ok('unlock follows again')

# long-press learn resets mapping
song.view.select_device(serum); s.tick()
import time; t = time.time; time.time = lambda: t() - 0 ; s._learn.receive(127); s._learn_pressed_at -= 2; s._learn.receive(0)
assert os.listdir(AM.MAPPING_DIR) == [] and s._crossfader.mapped is None; ok('long-press Learn resets mapping')

# mixer mode
s._mixer_button.receive(127)
assert all(s._encoders[i].mapped == 'volume of strip %d' % i for i in range(8)); ok('mixer mode: knobs control track volumes')
song.view.select_device(eq); s.tick()
assert all(s._encoders[i].mapped == 'volume of strip %d' % i for i in range(8)); ok('mixer mode: changing device keeps knobs on volumes')
serum.parameters[3].value = 0.9; s.tick()
assert s._encoders[2].mapped == 'volume of strip 2'; ok('mixer mode: parameter changes elsewhere do not steal knobs')
song.view.select_device(serum); s.tick()
s._mixer_button.receive(127); assert s._encoders[0].mapped is serum.parameters[1]; ok('back to nightcap mode')

# parameters reconfigured (Live Configure) -> rebuild
serum.parameters.append(Param('Serum New')); [cb() for cb in serum.pl]; s.tick(); assert am.num_pages == 3; ok('param list change rebuilds pages')
# ---- HUD over SysEx
import json
def hud():
    s.midi = []; s.update_display()
    msgs = [m for m in s.midi if m[:6] == (0xF0, 0x7D, 0x4B, 0x4E, 0x43, 0x10)]
    if not msgs: return None
    assert all(len(m) <= 9 + 96 and all(b < 0x80 for b in m[1:-1]) for m in msgs)
    assert [m[6] for m in msgs] == list(range(len(msgs))) and all(m[7] == len(msgs) for m in msgs)
    return json.loads(bytes(b for m in msgs for b in m[8:-1]).decode('ascii'))
s.request_hud(True); h = hud()
assert h['t'] == 'Serum' and h['p'] == 'Page 1 / 3' and h['s'][0][0] == 'Serum P0' and h['pop']; ok('HUD: device, page, names over 7-bit SysEx chunks')
s._touches[3].receive(127); h = hud(); assert h['touch'] == 3 and h['pop']; ok('HUD pops on touch, highlights knob 4')
s._touches[3].receive(0); h = hud(); assert h['touch'] == -1; ok('HUD release')
serum.parameters[2].value = 0.5; h = hud(); assert h['s'][1][2] == 0.5 and not h['pop']; ok('HUD value update without popping')
assert hud() is None; ok('no resend when nothing changed')
s._learn.receive(127); s._learn.receive(0); s._touches[5].receive(127); h = hud(); assert h['learn'] and h['lt'] == 5; ok('HUD learn target')
s._learn.receive(127); s._learn.receive(0); s._touches[5].receive(0); hud()
serum.name = 'Sérum ✓'; s.request_hud(True); h = hud(); assert h['t'] == 'Sérum ✓'; ok('non-ASCII names survive (JSON escapes)')
s._mixer_button.receive(127); h = hud(); assert h['t'] == 'Mixer' and h['s'][0][0].startswith('Track') and h['s'][2] is None; ok('mixer HUD: track names/volumes')
s._mixer_button.receive(127); s._wrapper_active = True; s.request_hud(); assert hud()['hide'] is True; s._wrapper_active = False; ok('HUD hidden when wrapper active')
# ---- racks: each rack keeps its own mapping (they all share the type name "Instrument Rack")
s._mixer_button.receive(127) if s._mode != 0 else None
def rack(name, macros):
    d = Device(name, 0, 'InstrumentGroupDevice'); d.class_display_name = 'Instrument Rack'
    d.parameters = [Param('Device On')] + [Param(m) for m in macros]
    return d
wooley = rack('Wooley Bass', ['Filter Cutoff', 'Filter Reso', 'Filter Drive', 'Filter Type', 'Filter Env', 'Arp Gate', 'Chorus', 'Volume'])
pad = rack('Dream Pad', ['Drive', 'Filter Freq', 'Reso', 'Glide', 'FM', 'Mod 1', 'Mod 2', 'Space'])
song.tracks.append(Track([wooley, pad]))
song.view.select_device(pad); s.tick()
s._learn.receive(127); s._learn.receive(0); s._touches[0].receive(127); pad.parameters[8].value = 0.4; s.tick(); s._touches[0].receive(0)
assert s._encoders[0].mapped is pad.parameters[8]; ok('learned on rack "Dream Pad"')
assert os.path.exists(os.path.join(AM.MAPPING_DIR, 'InstrumentGroupDevice - Dream Pad.json')); ok('rack mapping saved under the rack\'s own name')
song.view.select_device(wooley); s.tick()
assert s._nightcap._custom is None and s._encoders[0].mapped is wooley.parameters[1]; ok('other rack keeps its default layout')

# the old shared "Instrument Rack" file is no longer applied to any rack
os.remove(os.path.join(AM.MAPPING_DIR, 'InstrumentGroupDevice - Dream Pad.json'))
with open(os.path.join(AM.MAPPING_DIR, 'InstrumentGroupDevice - Instrument Rack.json'), 'w') as f:
    json.dump({'slots': ['Reso', 'Drive', 'Filter Freq', '', '', '', '', ''], 'crossfader': ''}, f)
song.view.select_device(pad); s.tick()
assert s._nightcap._custom is None and s._encoders[0].mapped is pad.parameters[1]; ok('old shared rack file ignored: rack shows its own macros')
# a mapping only "matching" on generic Macro N names is not used
with open(os.path.join(AM.MAPPING_DIR, 'InstrumentGroupDevice - Wooley Bass.json'), 'w') as f:
    # 8 of 10 names exist, but only because of the generic ones: without the filter this would pass 80%
    json.dump({'slots': ['Drive', 'Glide'] + ['Macro %d' % i for i in range(9, 17)], 'crossfader': ''}, f)
wooley.parameters += [Param('Macro %d' % i) for i in range(9, 17)]
song.view.select_device(wooley); s.tick()
assert s._nightcap._custom is None and s._encoders[0].mapped is wooley.parameters[1]; ok('generic Macro N names do not count as a match')
os.remove(os.path.join(AM.MAPPING_DIR, 'InstrumentGroupDevice - Wooley Bass.json'))

# plug-in whose parameters appear after selection (Live opening a set): saved mapping applies once they load
late = Device('Late Synth', 0)
song.tracks.append(Track([late]))
with open(os.path.join(AM.MAPPING_DIR, 'PluginDevice - Late Synth.json'), 'w') as f:
    json.dump({'slots': ['Cutoff', 'Res'], 'crossfader': ''}, f)
song.view.select_device(late); s.tick()
assert s._nightcap._custom is None; ok('mapping not applied while parameters are missing')
late.parameters = [Param('Device On'), Param('Res'), Param('Cutoff')]
[cb() for cb in late.pl]; s.tick()
assert s._encoders[0].mapped is late.parameters[2] and s._encoders[1].mapped is late.parameters[1]; ok('mapping applied when parameters finish loading')

s.disconnect(); print('ALL LIVE SCRIPT TESTS PASSED')
try:
    os.remove(os.path.join(HERE, '..', 'live-script', 'NightCap', 'nightcap_debug.log'))
except OSError:
    pass
shutil.rmtree(AM.MAPPING_DIR, ignore_errors=True)
