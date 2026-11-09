# Protocol notes (Novation Nocturn controller)

## USB
| | |
|---|---|
| Vendor / product | `0x1235` / `0x000A` |
| Configuration | first (value 1) |
| Interface | first interface; pipe 1 = interrupt IN, pipe 2 = interrupt OUT |

**Initialisation** (sent after opening the interface):
```
B0 00 00
28 00 2B 4A 2C 00 2E 35
(wait 50 ms)
2A 02 2C 72 2E 30
```

**Output (host → Nocturn)**: data is sent as `(control, value)` byte pairs with no status byte, at most 4 pairs (8 bytes) per USB
packet. Values that haven't changed are skipped.

**Input (Nocturn → host)**: also `(control, value)` pairs. A leading `B0` is a running-status byte and is skipped. The crossfader
arrives as a pair: `48 hi` then `49 lo`; the 8-bit value is `hi*2 + (lo >> 6)`.

## Controls
| control | number (hex / dec) | value |
|---|---|---|
| Encoders 1–8 | 40–47 / 64–71 | relative: 1–63 = clockwise, 65–127 = anticlockwise (7-bit two's complement) |
| Crossfader | 48 / 72 | absolute (sent as 0–127 by NightCapBridge) |
| Speed dial | 4A / 74 | relative |
| Speed dial touch / push | 51 / 81, 52 / 82 | |
| Crossfader touch | 53 / 83 | |
| Encoder touch 1–8 | 60–67 / 96–103 | 127 touch, 0 release |
| Buttons | 70–7F / 112–127 | 127 press, 0 release. 112–119 top row; 120 Learn, 121 View, 122 Page −, 123 Page +, 124 User, 125 FX, 126 Inst, 127 Mixer |

**LED outputs**: rings 40–47 (position 0–127), ring display style 48–4F, button LEDs 70–7F (0 off, anything else on).

**Encoder acceleration** (matching the original driver): consecutive moves in the same direction less than 16 ms apart
count ×3; less than 26 ms apart count ×2.

## MIDI (NightCapBridge ↔ host)
NightCapBridge creates two virtual port pairs: **NightCap** (DAW / Live script) and **NightCap Wrapper** (wrapper plug-ins).
Controls are sent as CC on channel 1 using the numbers above (decimal). CCs sent back light the LEDs. Only one pair owns the
hardware at a time, and each pair's LED state is remembered and restored when it takes over again.

### SysEx (header `F0 7D 4B 4E 43`)
`7D` is the MIDI manufacturer ID reserved for non-commercial use, followed by "KNC".
| message | bytes | direction |
|---|---|---|
| Route hardware | `… 01 <0 = NightCap, 1 = NightCap Wrapper> F7` | host → bridge |
| Routing changed | `… 02 <0/1> F7` | bridge → both ports |
| HUD update | `… 10 <chunk index> <chunk count> <ASCII JSON …> F7` | Live script → bridge |

HUD JSON fields: `t` title, `p` page text, `sub` subtitle, `s` 8 × `[name, value text, 0..1]` or null, `x` crossfader slot,
`touch` touched knob (−1 if none), `learn` (bool), `lt` learn target, `pop` (show now), `hide` (hide the HUD).
The JSON is ASCII-escaped and split into chunks of up to 96 bytes.
