"""
Native NightCap for Ableton Live - controls any device (Live devices, VST, AU) directly,
no wrapper plug-in needed.

 * Every parameter of the controlled device is laid out in pages of 8 knobs
   (Live's own curated banks first for Live devices, then everything else).
 * Learn: press Learn, touch a controller knob (or move the crossfader), then move a control
   in the plug-in's own window. The mapping is saved per plug-in and reused next time.
 * Speed dial fine-adjusts the last knob you touched. View resets it to its default.
 * Names / values are shown in Live's status bar; LED rings follow the parameter values.
"""
import json
import os
import re

try:
    from _Generic.Devices import parameter_banks
except Exception:  # pragma: no cover - older/newer Live layouts
    parameter_banks = None

SLOTS = 8
XFADER = SLOTS
MAPPING_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mappings')
PLUGIN_CLASSES = ('PluginDevice', 'AuPluginDevice')
RACK_CLASSES = ('InstrumentGroupDevice', 'AudioEffectGroupDevice', 'MidiEffectGroupDevice', 'DrumGroupDevice')
_GENERIC_NAME = re.compile(r'^Macro \d+$')
_DEBUG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'nightcap_debug.log')


def _debug(msg):
    try:
        import time
        with open(_DEBUG_FILE, 'a') as f:
            f.write('%s  %s\n' % (time.strftime('%H:%M:%S'), msg))
    except Exception:
        pass


def _safe_name(text):
    return ''.join(c if c.isalnum() or c in ' -_.' else '_' for c in text).strip() or 'device'


def _norm(param):
    span = param.max - param.min
    return 0.0 if span == 0 else (param.value - param.min) / span


class NightCapEngine(object):

    def __init__(self, surface, encoders, crossfader):
        self._surface = surface
        self._encoders = encoders
        self._crossfader = crossfader
        self._device = None
        self._locked = False
        self._pages = []             # list of lists of DeviceParameter (len 8, may contain None)
        self._custom_path = None     # file the current custom mapping came from
        self._custom = None          # learned mapping: {'slots': [names...], 'crossfader': name}
        self._page = 0
        self._last_slot = 0
        self._learn = False
        self._learn_slot = None
        self._fine_accum = 0.0
        self._active = False
        self._watched = []           # params with ring listeners
        self._learn_watched = []     # params with learn listeners
        self._xfader_param = None
        self._listening_device = None
        self._touched = -1

    # ------------------------------------------------------------------ public
    def set_active(self, active):
        self._active = active
        self._connect()

    def appointed_device_changed(self, device):
        if self._locked and self._device_alive():
            return
        self._locked = False
        self._set_device(device)

    def toggle_lock(self):
        if self._device is None:
            return
        self._locked = not self._locked
        self._message('Locked to %s' % self._device.name if self._locked else 'Following the selected device')
        self._hud(True)

    @property
    def locked(self):
        return self._locked

    @property
    def page(self):
        return self._page

    @property
    def num_pages(self):
        return max(1, len(self._pages))

    @property
    def learn_armed(self):
        return self._learn

    def set_page(self, page):
        page = max(0, min(self.num_pages - 1 + (1 if self._custom is not None else 0), page))
        if page != self._page:
            self._page = page
            self._ensure_page_exists(page)
            self._connect()
        self._show_page()
        self._hud(True)

    def touch(self, slot, touched):
        if not touched:
            if self._touched == slot:
                self._touched = -1
                self._hud(False)
            return
        self._touched = slot
        self._hud(True)
        self._last_slot = slot
        if self._learn:
            self._learn_slot = slot
            self._message('Learn: now move a control in the plug-in for knob %d' % (slot + 1))
            self._hud(True)
            return
        param = self._param(slot)
        if param is not None:
            self._message('%d  %s: %s' % (slot + 1, param.name, self._value_text(param)))

    def crossfader_moved(self):
        if self._learn:
            self._learn_slot = XFADER
            self._hud(True)
            self._message('Learn: now move a control in the plug-in for the crossfader')

    def fine_adjust(self, delta):
        param = self._param(self._last_slot)
        if param is None or not param.is_enabled:
            return
        if param.is_quantized:
            self._fine_accum += delta
            steps = int(self._fine_accum / 3)
            if not steps:
                return
            self._fine_accum -= steps * 3
            param.value = max(param.min, min(param.max, param.value + steps))
        else:
            span = param.max - param.min
            param.value = max(param.min, min(param.max, param.value + delta * span / 1270.0))
        self._message('%s: %s' % (param.name, self._value_text(param)))
        self._hud(True)

    def reset_last(self):
        param = self._param(self._last_slot)
        if param is not None and param.is_enabled and not param.is_quantized:
            default = getattr(param, 'default_value', None)
            if default is not None:
                param.value = default
                self._message('%s reset to %s' % (param.name, self._value_text(param)))

    def toggle_learn(self):
        if self._device is None:
            return
        self._learn = not self._learn
        self._learn_slot = self._last_slot if self._learn else None
        self._watch_for_learn(self._learn)
        self._connect()
        if self._learn:
            self._message('LEARN: touch a controller knob (or move the crossfader), then move a control in the plug-in')
        else:
            self._show_page()
        self._hud(True)

    def reset_mapping(self):
        if self._device is None:
            return
        self._custom = None
        for path in (getattr(self, '_custom_path', None), self._mapping_file()):
            if path:
                try:
                    os.remove(path)
                except OSError:
                    pass
        self._custom_path = None
        self._rebuild_pages()
        self._message('Mapping reset to default for %s' % self._device.name)

    def refresh_leds(self):
        if not self._active:
            return
        for slot, enc in enumerate(self._encoders):
            param = self._param(slot)
            enc.send_value(int(round(_norm(param) * 127)) if param is not None else 0, True)

    def disconnect(self):
        self._watch_rings([])
        self._watch_for_learn(False)
        self._listen_device(None)

    # ------------------------------------------------------------------ device / pages
    def _device_alive(self):
        try:
            return self._device is not None and self._device.name is not None
        except Exception:
            return False

    def _set_device(self, device):
        if device == self._device and device is not None:
            return
        if self._learn:
            self._learn = False
            self._watch_for_learn(False)
        self._device = device
        self._listen_device(device)
        if device is not None:
            try:
                _debug('selected %s (%s, %d parameters) -> %s.json'
                       % (device.name, device.class_name, len(self._all_params()), _safe_name(self._device_key())))
            except Exception:
                pass
        self._custom = self._load_mapping()
        self._page = 0
        self._touched = -1
        self._rebuild_pages()
        self._hud(True)

    def _listen_device(self, device):
        old = self._listening_device
        if old is not None:
            try:
                old.remove_parameters_listener(self._on_parameters_changed)
            except Exception:
                pass
        self._listening_device = None
        if device is not None:
            try:
                device.add_parameters_listener(self._on_parameters_changed)
                self._listening_device = device
            except Exception:
                pass

    def _on_parameters_changed(self):
        # Plug-in parameters were (re)configured, or have just finished loading (common when
        # Live opens a set): re-check the saved mapping, then rebuild after the notification.
        self._surface.schedule_message(1, self._reload_after_parameters_change)

    def _reload_after_parameters_change(self):
        if self._device is None or self._learn:
            self._rebuild_pages()
            return
        self._custom = self._load_mapping()
        self._rebuild_pages()

    def _all_params(self):
        if self._device is None:
            return []
        try:
            # index 0 is "Device On"
            return [p for p in self._device.parameters[1:]]
        except Exception:
            return []

    def _rebuild_pages(self):
        params = self._all_params()
        by_name = {}
        for p in params:
            by_name.setdefault(p.name, p)
        pages = []
        self._xfader_param = None
        if self._custom is not None:
            names = self._custom.get('slots', [])
            slots = [by_name.get(n) if n else None for n in names]
            for i in range(0, max(len(slots), 1), SLOTS):
                page = slots[i:i + SLOTS]
                pages.append(page + [None] * (SLOTS - len(page)))
            self._xfader_param = by_name.get(self._custom.get('crossfader') or '')
        else:
            used = set()
            if parameter_banks is not None and self._device is not None:
                try:
                    for bank in parameter_banks(self._device):
                        page = [p for p in bank][:SLOTS]
                        if any(p is not None for p in page):
                            pages.append(page + [None] * (SLOTS - len(page)))
                            used.update(id(p) for p in page if p is not None)
                except Exception:
                    pages = []
                    used = set()
            rest = [p for p in params if id(p) not in used]
            for i in range(0, len(rest), SLOTS):
                page = rest[i:i + SLOTS]
                pages.append(page + [None] * (SLOTS - len(page)))
        self._pages = pages
        self._page = min(self._page, max(0, len(pages) - 1))
        self._connect()
        self._show_page()
        self._hud(False)

    def _ensure_page_exists(self, page):
        while len(self._pages) <= page:
            self._pages.append([None] * SLOTS)

    def _param(self, slot):
        if slot == XFADER:
            return self._xfader_param
        if 0 <= self._page < len(self._pages):
            return self._pages[self._page][slot]
        return None

    # ------------------------------------------------------------------ controls & LEDs
    def _connect(self):
        for slot, enc in enumerate(self._encoders):
            param = self._param(slot) if (self._active and not self._learn) else None
            if param is not None:
                enc.connect_to(param)
            else:
                enc.release_parameter()
        self._watch_rings([self._param(s) for s in range(SLOTS)] if self._active else [])
        self._surface.request_rebuild_midi_map()
        self.refresh_leds()

    def crossfader_param(self):
        """The learned crossfader parameter for this device, or None (= Live's crossfader)."""
        return self._xfader_param if (self._active and not self._learn) else None

    def _watch_rings(self, params):
        for p, cb in self._watched:
            try:
                p.remove_value_listener(cb)
            except Exception:
                pass
        self._watched = []
        for slot, p in enumerate(params):
            if p is None:
                continue
            cb = self._make_ring_listener(slot, p)
            try:
                p.add_value_listener(cb)
                self._watched.append((p, cb))
            except Exception:
                pass

    def _make_ring_listener(self, slot, param):
        def on_value():
            if self._active:
                self._encoders[slot].send_value(int(round(_norm(param) * 127)), True)
                self._hud(False)
        return on_value

    # ------------------------------------------------------------------ learn
    def _watch_for_learn(self, on):
        for p, cb in self._learn_watched:
            try:
                p.remove_value_listener(cb)
            except Exception:
                pass
        self._learn_watched = []
        if not on:
            return
        for p in self._all_params():
            cb = self._make_learn_listener(p)
            try:
                p.add_value_listener(cb)
                self._learn_watched.append((p, cb))
            except Exception:
                pass

    def _make_learn_listener(self, param):
        def on_value():
            if self._learn and self._learn_slot is not None:
                # can't change mappings inside a Live notification - defer
                self._surface.schedule_message(1, lambda: self._complete_learn(param))
        return on_value

    def _complete_learn(self, param):
        if not self._learn or self._learn_slot is None:
            return
        slot = self._learn_slot
        self._learn = False
        self._watch_for_learn(False)
        if self._custom is None:
            # start from the current layout so the other knobs keep their parameters
            self._custom = {'slots': [p.name if p is not None else '' for page in self._pages for p in page],
                            'crossfader': self._xfader_param.name if self._xfader_param is not None else ''}
        if slot == XFADER:
            self._custom['crossfader'] = param.name
        else:
            idx = self._page * SLOTS + slot
            slots = self._custom['slots']
            while len(slots) <= idx:
                slots.append('')
            slots[idx] = param.name
        self._save_mapping()
        self._rebuild_pages()
        self._message('Learned: %s -> %s' % (param.name, 'crossfader' if slot == XFADER else 'knob %d' % (slot + 1)))
        self._hud(True)
        self._surface.nightcap_state_changed()

    # ------------------------------------------------------------------ persistence
    def _device_key(self):
        """Mappings are saved per plug-in, per rack and per Live device type.

        Plug-ins and racks are keyed by their own name ("Serum 2", "Wooley Bass"), so two
        Instrument Racks no longer share one mapping. Other Live devices are keyed by type
        ("EQ Eight"), so a renamed EQ Eight keeps its mapping."""
        d = self._device
        kind = getattr(d, 'class_name', 'Device')
        if kind in PLUGIN_CLASSES or kind in RACK_CLASSES:
            name = d.name
        else:
            name = getattr(d, 'class_display_name', '')
        return '%s - %s' % (kind, name or d.name)

    def _mapping_file(self):
        return os.path.join(MAPPING_DIR, _safe_name(self._device_key()) + '.json')

    def _load_mapping(self):
        """Return the saved mapping for this device, or None for the default layout.

        A saved mapping is only used when nearly all of the parameter names it refers to exist on
        this device (generic names like "Macro 9" don't count, every rack has those). Otherwise it
        belongs to something else, or the device's parameters haven't loaded yet, and the default
        layout is better than a page of wrong or empty knobs."""
        self._custom_path = None
        if self._device is None:
            return None
        path = self._mapping_file()
        try:
            with open(path, 'r') as f:
                data = json.load(f)
        except Exception:
            return None
        if not isinstance(data, dict) or not isinstance(data.get('slots'), list):
            return None
        present = set(p.name for p in self._all_params())
        names = [n for n in data['slots'] if n and not _GENERIC_NAME.match(n)]
        if data.get('crossfader'):
            names.append(data['crossfader'])
        hits = sum(1 for n in names if n in present)
        if names and hits * 10 >= len(names) * 8:
            self._custom_path = path
            _debug('  mapping: using %s (%d/%d parameters found)' % (os.path.basename(path), hits, len(names)))
            return data
        _debug('  mapping: not using %s yet (%d/%d parameters found)' % (os.path.basename(path), hits, len(names)))
        return None

    def _save_mapping(self):
        try:
            if not os.path.isdir(MAPPING_DIR):
                os.makedirs(MAPPING_DIR)
            path = self._mapping_file()
            with open(path, 'w') as f:
                json.dump(self._custom, f, indent=1)
            self._custom_path = path
        except Exception as e:
            self._surface.log_message('NightCap: could not save mapping: %s' % e)

    # ------------------------------------------------------------------ HUD
    def _hud(self, pop):
        self._surface.request_hud(pop)

    def _slot_state(self, param):
        if param is None:
            return None
        return [param.name, self._value_text(param), round(_norm(param), 4)]

    def hud_state(self):
        if self._device is None:
            return {'t': 'No device selected', 'p': '', 'sub': 'Select a device or plug-in in Live',
                    's': [None] * SLOTS, 'x': None, 'touch': -1, 'learn': False, 'lt': -1}
        try:
            title = self._device.name
        except Exception:
            title = '?'
        if self._locked:
            title += '  [locked]'
        sub = 'Custom mapping' if self._custom is not None else 'NightCap'
        return {'t': title,
                'p': 'Page %d / %d' % (self._page + 1, self.num_pages),
                'sub': sub + '   -   Learn: press Learn, touch a knob, move a control in the plug-in',
                's': [self._slot_state(self._param(i)) for i in range(SLOTS)],
                'x': self._slot_state(self._xfader_param),
                'touch': self._touched,
                'learn': self._learn,
                'lt': self._learn_slot if (self._learn and self._learn_slot is not None) else -1}

    # ------------------------------------------------------------------ display
    @staticmethod
    def _value_text(param):
        try:
            return str(param)
        except Exception:
            return '%.2f' % param.value

    def _show_page(self):
        if not self._active or self._learn:
            return
        if self._device is None:
            self._message('No device selected')
            return
        names = []
        for slot in range(SLOTS):
            p = self._param(slot)
            names.append('%d %s' % (slot + 1, p.name if p is not None else '-'))
        self._message('%s%s  page %d/%d  |  %s' % ('[locked] ' if self._locked else '', self._device.name,
                                                 self._page + 1, self.num_pages, '  '.join(names)))

    def _message(self, text):
        self._surface.show_message('NightCap: ' + text)
