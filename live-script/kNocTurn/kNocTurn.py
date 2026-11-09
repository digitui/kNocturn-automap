"""
kNocTurn - Ableton Live remote script with native NightCap (no wrapper plug-in needed).

  Control         CC        NightCap mode (default)                    Mixer mode
  Encoders 1-8    64-71     the 8 parameters on the current page      volumes of the 8 tracks in the red box
  Encoder touch   96-103    show name/value, pick knob for Learn      -
  Speed dial      74        fine-adjust the last touched parameter     select track
  Crossfader      72        learned parameter, else Live crossfader    Live crossfader
  Buttons 1-8     112-119   jump to page 1-8                          mute tracks
  Learn           120       learn (hold 1.5 s: reset this device's mapping)
  View            121       reset last touched parameter to default
  Page - / +      122/123   previous / next page                      move red box
  User / FX       124/125   previous / next device on the track
  Inst            126       lock to this device (stays while you select other tracks)
  Mixer           127       switch NightCap / Mixer mode

Works with Live devices, VST and AU plug-ins directly. If a plug-in shows no parameters,
click Live's "Configure" button on the device once (Live only exposes up to 128 plug-in
parameters automatically).

If a "kNocTurn NightCap" wrapper plug-in is selected, the kNocTurn is handed to it instead.
"""
import json
import os
import time
import traceback

_DEBUG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'knocturn_debug.log')


def debug(msg):
    """Always-on diagnostics: written next to this script (Live's own log doesn't show everything)."""
    try:
        if os.path.exists(_DEBUG_FILE) and os.path.getsize(_DEBUG_FILE) > 256 * 1024:
            os.remove(_DEBUG_FILE)          # keep it small
        with open(_DEBUG_FILE, 'a') as f:
            f.write('%s  %s\n' % (time.strftime('%H:%M:%S'), msg))
    except Exception:
        pass

import Live
from _Framework.ControlSurface import ControlSurface
from _Framework.InputControlElement import MIDI_CC_TYPE
from _Framework.EncoderElement import EncoderElement
from _Framework.ButtonElement import ButtonElement
from _Framework.SliderElement import SliderElement
from _Framework.DeviceComponent import DeviceComponent
from _Framework.MixerComponent import MixerComponent
from _Framework.SessionComponent import SessionComponent

from .NightCap import NightCapEngine

CHANNEL = 0
ENCODER_CCS = list(range(64, 72))
TOUCH_CCS = list(range(96, 104))
SPEED_DIAL_CC = 74
CROSSFADER_CC = 72
TOP_BUTTON_CCS = list(range(112, 120))
LEARN, VIEW, PAGE_DOWN, PAGE_UP, USER, FX, INST, MIXER = range(120, 128)

SYSEX_HEADER = (0xF0, 0x7D, 0x4B, 0x4E, 0x43)
SYSEX_SET_MODE = 0x01
SYSEX_MODE_IS = 0x02
SYSEX_HUD = 0x10
HUD_CHUNK = 96
NIGHTCAP_FOCUS_PARAM = 'NightCap Focus'
LONG_PRESS = 1.5

MODE_NIGHTCAP, MODE_MIXER = 0, 1


def _relative(value):
    return value if value < 64 else value - 128


class kNocTurn(ControlSurface):

    def __init__(self, c_instance):
        ControlSurface.__init__(self, c_instance)
        debug('---- script starting (Live loaded kNocTurn) ----')
        try:
            self._init_surface()
            debug('startup complete')
        except Exception:
            debug('STARTUP ERROR:\n' + traceback.format_exc())
            raise

    def _init_surface(self):
        self._mode = MODE_NIGHTCAP
        self._wrapper_active = False
        self._learn_pressed_at = None
        self._hud_dirty = False
        self._hud_pop = False
        self._mixer_touch = -1
        self._last_hud = None
        self._hud_logged = False
        with self.component_guard():
            self._create_controls()
            self._create_components()
            self._nightcap = NightCapEngine(self, self._encoders, self._crossfader)
            self._update_mode()
        self.song().add_appointed_device_listener(self._on_appointed_device_changed)
        self._set_bridge_mode(0)
        self.show_message('kNocTurn connected (native NightCap)')
        self.log_message('kNocTurn: native NightCap script with HUD loaded')
        self._on_appointed_device_changed()

    # ------------------------------------------------------------------ setup
    def _create_controls(self):
        rel = Live.MidiMap.MapMode.relative_two_compliment
        self._encoders = tuple(EncoderElement(MIDI_CC_TYPE, CHANNEL, cc, rel) for cc in ENCODER_CCS)
        self._touches = tuple(ButtonElement(True, MIDI_CC_TYPE, CHANNEL, cc) for cc in TOUCH_CCS)
        self._speed_dial = EncoderElement(MIDI_CC_TYPE, CHANNEL, SPEED_DIAL_CC, rel)
        self._crossfader = SliderElement(MIDI_CC_TYPE, CHANNEL, CROSSFADER_CC)
        self._top_buttons = tuple(ButtonElement(True, MIDI_CC_TYPE, CHANNEL, cc) for cc in TOP_BUTTON_CCS)
        b = lambda cc: ButtonElement(True, MIDI_CC_TYPE, CHANNEL, cc)
        self._learn = b(LEARN)
        self._view = b(VIEW)
        self._page_down = b(PAGE_DOWN)
        self._page_up = b(PAGE_UP)
        self._user = b(USER)
        self._fx = b(FX)
        self._inst = b(INST)
        self._mixer_button = b(MIXER)

        for i, enc in enumerate(self._encoders):
            enc.name = 'Encoder_%d' % (i + 1)
        for i, t in enumerate(self._touches):
            t.name = 'Touch_%d' % (i + 1)
            t.add_value_listener(self._make_touch_listener(i))
        for i, btn in enumerate(self._top_buttons):
            btn.name = 'Button_%d' % (i + 1)
            btn.add_value_listener(self._make_page_listener(i))
        self._speed_dial.add_value_listener(self._on_speed_dial)
        self._crossfader.add_value_listener(self._on_crossfader)
        self._learn.add_value_listener(self._on_learn)
        self._view.add_value_listener(self._on_view)
        self._page_down.add_value_listener(lambda v: self._on_page_step(v, -1))
        self._page_up.add_value_listener(lambda v: self._on_page_step(v, 1))
        self._page_down.add_value_listener(self._on_mixer_bank)
        self._page_up.add_value_listener(self._on_mixer_bank)
        self._user.add_value_listener(lambda v: self._on_device_step(v, -1, self._user))
        self._fx.add_value_listener(lambda v: self._on_device_step(v, 1, self._fx))
        self._inst.add_value_listener(self._on_inst)
        self._mixer_button.add_value_listener(self._on_mixer_button)

    def _create_components(self):
        # Device component (no controls) keeps Live's blue hand on the selected device
        self._device = DeviceComponent(device_selection_follows_track_selection=True)
        self.set_device_component(self._device)
        self._mixer = MixerComponent(8)
        self._session = SessionComponent(8, 1)
        self._session.set_mixer(self._mixer)
        self.set_highlighting_session_component(self._session)

    def _make_touch_listener(self, slot):
        def listener(value):
            if self._wrapper_active:
                return
            if self._mode == MODE_NIGHTCAP:
                self._nightcap.touch(slot, value > 0)
            else:
                self._mixer_touch = slot if value else (-1 if self._mixer_touch == slot else self._mixer_touch)
                self.request_hud(bool(value))
        return listener

    def _make_page_listener(self, page):
        def listener(value):
            if value and self._mode == MODE_NIGHTCAP and not self._wrapper_active:
                self._nightcap.set_page(page)
                self._update_button_leds()
        return listener

    # ------------------------------------------------------------------ modes
    def _update_mode(self):
        nightcap = self._mode == MODE_NIGHTCAP and not self._wrapper_active
        mixer = self._mode == MODE_MIXER and not self._wrapper_active
        for i in range(8):
            strip = self._mixer.channel_strip(i)
            strip.set_volume_control(self._encoders[i] if mixer else None)
            strip.set_mute_button(self._top_buttons[i] if mixer else None)
        self._session.set_track_bank_buttons(self._page_up if mixer else None,
                                             self._page_down if mixer else None)
        self._nightcap.set_active(nightcap)
        self._update_crossfader()
        self._update_button_leds()
        if self._wrapper_active:
            self.show_message('kNocTurn: NightCap wrapper in control (press Mixer to return)')
        self.request_hud(True)
        self.request_rebuild_midi_map()

    def _update_crossfader(self):
        param = self._nightcap.crossfader_param() if not self._wrapper_active else None
        learning = self._nightcap.learn_armed and self._mode == MODE_NIGHTCAP
        if param is not None:
            self._mixer.set_crossfader_control(None)
            self._crossfader.connect_to(param)
        else:
            self._crossfader.release_parameter()
            self._mixer.set_crossfader_control(None if (learning or self._wrapper_active) else self._crossfader)

    def _update_button_leds(self):
        if self._wrapper_active:
            return
        nightcap = self._mode == MODE_NIGHTCAP
        if nightcap:
            for i, btn in enumerate(self._top_buttons):
                btn.send_value(127 if i == self._nightcap.page else 0, True)
            self._page_down.send_value(127 if self._nightcap.page > 0 else 0, True)
            self._page_up.send_value(127 if self._nightcap.page < self._nightcap.num_pages - 1 else 0, True)
        self._learn.send_value(127 if (nightcap and self._nightcap.learn_armed) else 0, True)
        self._inst.send_value(127 if self._nightcap.locked else 0, True)
        self._mixer_button.send_value(127 if self._mode == MODE_MIXER else 0, True)

    def nightcap_state_changed(self):
        """Called by NightCapEngine after learn finishes / pages change."""
        with self.component_guard():
            self._update_crossfader()
            self._update_button_leds()
            self.request_rebuild_midi_map()

    # ------------------------------------------------------------------ button handlers
    def _on_mixer_button(self, value):
        if not value:
            return
        if self._wrapper_active:
            self._set_bridge_mode(0)
            self._set_wrapper(False)
            return
        self._mode = MODE_MIXER if self._mode == MODE_NIGHTCAP else MODE_NIGHTCAP
        self.show_message('kNocTurn: %s mode' % ('Mixer' if self._mode == MODE_MIXER else 'NightCap'))
        self._update_mode()

    def _on_learn(self, value):
        if self._mode != MODE_NIGHTCAP or self._wrapper_active:
            return
        if value:
            self._learn_pressed_at = time.time()
            return
        held = self._learn_pressed_at is not None and time.time() - self._learn_pressed_at >= LONG_PRESS
        self._learn_pressed_at = None
        if held:
            if self._nightcap.learn_armed:
                self._nightcap.toggle_learn()
            self._nightcap.reset_mapping()
        else:
            self._nightcap.toggle_learn()
        self.nightcap_state_changed()

    def _on_view(self, value):
        if value and self._mode == MODE_NIGHTCAP and not self._wrapper_active:
            self._nightcap.reset_last()

    def _on_page_step(self, value, step):
        if value and self._mode == MODE_NIGHTCAP and not self._wrapper_active:
            self._nightcap.set_page(self._nightcap.page + step)
            self._update_button_leds()

    def _on_inst(self, value):
        if value and not self._wrapper_active:
            self._nightcap.toggle_lock()
            self._update_button_leds()

    def _on_speed_dial(self, value):
        if self._wrapper_active:
            return
        delta = _relative(value)
        if not delta:
            return
        if self._mode == MODE_NIGHTCAP:
            self._nightcap.fine_adjust(delta)
            return
        song = self.song()
        tracks = list(song.visible_tracks) + list(song.return_tracks) + [song.master_track]
        try:
            idx = tracks.index(song.view.selected_track)
        except ValueError:
            idx = 0
        song.view.selected_track = tracks[max(0, min(len(tracks) - 1, idx + (1 if delta > 0 else -1)))]

    def _on_crossfader(self, value):
        if self._mode == MODE_NIGHTCAP and not self._wrapper_active and self._nightcap.learn_armed:
            self._nightcap.crossfader_moved()

    def _on_device_step(self, value, step, button):
        button.send_value(127 if value else 0, True)
        if not value or self._wrapper_active:
            return
        song = self.song()
        track = song.view.selected_track
        devices = list(track.devices)
        if not devices:
            return
        current = track.view.selected_device
        idx = devices.index(current) if current in devices else 0
        song.view.select_device(devices[max(0, min(len(devices) - 1, idx + step))])

    # ------------------------------------------------------------------ HUD
    def request_hud(self, pop=False):
        self._hud_dirty = True
        self._hud_pop = self._hud_pop or pop

    def _on_mixer_bank(self, value):
        if self._mode == MODE_MIXER:
            # the session component moves the red box on this press; refresh after it
            self.schedule_message(1, lambda: self.request_hud(True))

    def _mixer_hud_state(self):
        song = self.song()
        try:
            offset = self._session.track_offset()
        except Exception:
            offset = 0
        tracks = list(song.visible_tracks)[offset:offset + 8]
        slots = []
        for t in tracks:
            vol = t.mixer_device.volume
            span = (vol.max - vol.min) or 1.0
            slots.append([t.name, str(vol) + ('  (muted)' if t.mute else ''), round((vol.value - vol.min) / span, 4)])
        slots += [None] * (8 - len(slots))
        return {'t': 'Mixer', 'p': 'Tracks %d - %d' % (offset + 1, offset + 8),
                'sub': 'Encoders: volume   Buttons: mute   Page -/+: move tracks   Mixer: back to NightCap',
                's': slots, 'x': None, 'touch': self._mixer_touch, 'learn': False, 'lt': -1}

    def _send_hud(self):
        if self._wrapper_active:
            state = {'hide': True}
        elif self._mode == MODE_MIXER:
            state = self._mixer_hud_state()
        else:
            state = self._nightcap.hud_state()
        pop = self._hud_pop
        self._hud_dirty = self._hud_pop = False
        key = json.dumps(state, sort_keys=True)
        if key == self._last_hud and not pop:
            return
        self._last_hud = key
        state['pop'] = pop
        data = json.dumps(state, ensure_ascii=True, separators=(',', ':')).encode('ascii')
        chunks = [data[i:i + HUD_CHUNK] for i in range(0, len(data), HUD_CHUNK)][:127]
        for i, chunk in enumerate(chunks):
            self._send_midi(SYSEX_HEADER + (SYSEX_HUD, i, len(chunks)) + tuple(bytearray(chunk)) + (0xF7,))
        if not self._hud_logged:
            self._hud_logged = True
            debug('first HUD sent: %d bytes in %d SysEx messages: %s' % (len(data), len(chunks), data[:120]))
            self.log_message('kNocTurn: HUD data sent to NightCapBridge (%d bytes in %d SysEx messages)' % (len(data), len(chunks)))

    def update_display(self):
        ControlSurface.update_display(self)
        if not getattr(self, '_ud_logged', False):
            self._ud_logged = True
            debug('update_display is being called by Live (hud_dirty=%s)' % getattr(self, '_hud_dirty', None))
        if self._mode == MODE_MIXER and self._mixer_touch >= 0:
            self._hud_dirty = True          # live volume readout while a knob is touched
        if self._hud_dirty:
            try:
                self._send_hud()
            except Exception as e:
                self._hud_dirty = False
                debug('HUD ERROR:\n' + traceback.format_exc())
                self.log_message('kNocTurn HUD error: %s' % e)

    # ------------------------------------------------------------------ device following / wrapper handoff
    def _on_appointed_device_changed(self):
        # Live forbids changing parameters/mappings inside a notification: defer one tick
        self.schedule_message(1, self._handle_appointed_device)

    @staticmethod
    def _focus_param(device):
        try:
            if device is None or device.class_name not in ('PluginDevice', 'AuPluginDevice'):
                return None
            for p in device.parameters:
                if p.name == NIGHTCAP_FOCUS_PARAM:
                    return p
        except Exception:
            pass
        return None

    def _handle_appointed_device(self):
        device = self.song().appointed_device
        focus = self._focus_param(device)
        if focus is not None:
            song = self.song()
            for track in list(song.tracks) + list(song.return_tracks) + [song.master_track]:
                for other in track.devices:
                    p = self._focus_param(other)
                    if p is not None:
                        target = p.max if other == device else p.min
                        if p.value != target:
                            p.value = target
            self._set_bridge_mode(1)
            self._set_wrapper(True)
            return
        if self._wrapper_active:
            self._set_bridge_mode(0)
            self._set_wrapper(False)
        with self.component_guard():
            self._nightcap.appointed_device_changed(device)
            self._update_crossfader()
            self._update_button_leds()

    def _set_wrapper(self, active):
        if active != self._wrapper_active:
            self._wrapper_active = active
            with self.component_guard():
                self._update_mode()

    def _set_bridge_mode(self, mode):
        self._send_midi(SYSEX_HEADER + (SYSEX_SET_MODE, mode, 0xF7))

    def handle_sysex(self, midi_bytes):
        b = tuple(midi_bytes)
        if len(b) == 8 and b[:5] == SYSEX_HEADER and b[5] == SYSEX_MODE_IS:
            self._set_wrapper(b[6] == 1)
            return
        ControlSurface.handle_sysex(self, midi_bytes)

    # ------------------------------------------------------------------ lifecycle
    def disconnect(self):
        try:
            self.song().remove_appointed_device_listener(self._on_appointed_device_changed)
        except Exception:
            pass
        self._nightcap.disconnect()
        self._set_bridge_mode(0)
        for btn in self._top_buttons + (self._learn, self._view, self._page_down, self._page_up,
                                        self._user, self._fx, self._inst, self._mixer_button):
            btn.send_value(0, True)
        for enc in self._encoders:
            enc.send_value(0, True)
        ControlSurface.disconnect(self)
