<!--
Copyright (c) Impo contributors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Gadget capabilities

What a gadget can do for the agent, and the words it uses to say so. This is
the contract between the three layers of the project:

1. **Boards** (`esp32/components/impo/boards/`, `linux/`, `arcs/boards/`): one file per piece of
   hardware, adapting its screen, speaker, buttons and extras to the board
   interface. A board implements whichever capabilities its hardware has.
2. **Capabilities** (this document): a fixed vocabulary of commands with their
   parameters and results, grouped by namespace. A gadget registers the ones it
   implements; the agent and the app rely on the contract, not on the board.
3. **Ecosystem** (`skills/`): other people's devices on the home network,
   reached through a gadget's tunnel and controlled by the agent following a
   skill. Nothing in the firmware knows about them.

Anything a gadget needs that isn't here goes in as a board command first
(`impo_board_t.commands`); once a second board wants it, it comes here with a
contract.

## How a gadget says what it has

A gadget's firmware says only **who it is** and **what it can execute**.
Everything the agent is told about it (what the device is for, what its
events mean, how to use its commands) is the product's manifest on the Impo
developer platform, not the firmware. The contract is
`contracts/gadget-identity.md` in the impoai/impo repository.

`link.register` carries:

- `commands_v2`: every command the gadget runs, by name only: `{"<name>": {}}`,
  plus `timeout_ms` on a command that can take longer than 30 seconds. Its
  description and parameter schema are in the manifest. The gateway checks a
  bound gadget's manifest against these names: a manifest command the
  firmware does not register makes the gadget not set up. A gadget that is
  not set up is offered only these names. A result is
  `{"ok": true, "payload": {...}}` or `{"ok": false, "error": "..."}`.
- Who it is: an official unit's `unit_id` and `attest` (below), or a custom
  gadget's `product_id` (below), or neither.
- `capabilities`: a summary for apps and prompts, without listing commands:

```json
{
  "screen": {"width": 240, "height": 240, "color": true, "round": false, "touch": false},
  "avatar": true,
  "speaker": true, "microphone": true, "push_to_talk": true,
  "camera": true, "battery": true, "buttons": true,
  "features": ["motion_sensor", "touch_pads", "locomotion", "lights"]
}
```

`features` names what the fixed keys don't: a board lists its own in
`impo_board_t.features`. Known values: `motion_sensor` (`imu.*`), `locomotion`
(`chassis.*`), `head` (`head.*`), `head_touch` (sends `head_touched`), `lights`
(lights the agent can set: `chassis.set_light` or `lights.*`), `touch_pads`,
`environment_sensors` (`sensors.read`).

The Impo server shows `capabilities` with each gadget in
`impo_list_gadgets`.

### Official gadgets: `unit_id` and `attest`

A unit of a product that Impo registered carries a per-unit key in eFuse
`BLOCK_KEY5` (purpose `HMAC_UP`). Its `link.register` adds two fields:

- `unit_id`: the chip's factory MAC, 12 lowercase hex digits
  (`"70af096dc5a8"`).
- `attest`: base64url without padding of the 32-byte HMAC-SHA256 that the
  chip's HMAC peripheral computes with that key over
  `"impo-gadget/attest/v1"`, one 0x00 byte, and the session's 32-byte Noise
  handshake hash. The hash differs on every connection, so a recorded proof
  is useless elsewhere.

Firmware sends both or neither: neither when the block has no such key, on
the classic ESP32 (no HMAC peripheral), and when the peripheral fails, so
that gadget registers as custom. When the gateway accepts the proof, the
agent gets the manifest of the unit's product. `esp32/main/gadget_attest.c`
implements it.

### Custom gadgets: `product_id`

A gadget without a unit key can name a product its owner created on the Impo
developer platform: set `CONFIG_GADGET_PRODUCT_ID` (`idf.py menuconfig` >
ESP32 Device SDK) to the product's ID: the lowercase UUID Impo assigned when
the product was created. The build fails on any other value. `link.register`
then carries `"product_id": "6f1c2b9e-4a7d-4e2b-9c1a-3d5e7f9a1b2c"`, only when the unit did not attest: an official
unit sends `unit_id` and `attest` and never a `product_id`. Nothing is
proven: the binding holds only when the product's owner is the account the
gadget is paired to, and the agent then gets the product's `dev` manifest.
Left empty (the default), nothing is sent and the gadget is not set up: its
commands work, but the agent gets no instructions or event meanings for it.

To describe a gadget to the agent, create a product on the developer
platform and fill in its manifest: a system prompt, the commands the agent
may use (each a description and a JSON Schema for its parameters) and the
events with what each means on this device. A gadget's task answers one
thing at a time: while the agent works on one event, the next waits. So a
system prompt should ask for what one lookup can answer ("the subjects of the
two newest unread mails"), not for what takes a tour ("how many unread
mails"): a long turn leaves the gadget mute until it ends.

## What a board does

A board does what only hardware can: play bytes, move a servo to an angle,
light an LED, take a photo, read a sensor. Everything that needs understanding
or composing is the cloud's: speech is made in the cloud and played with
`speaker.play`, nods and dances are frames the agent writes for `head.play`,
and a touch or a shake is only an event, with no reaction of the board's own.

A command answers when its work is done, with what happened: a sound once it
has played (or was stopped, or failed), a head once it has settled, with the
angles its servos measured. "ok" never means "started". In firmware such a
command is an `impo_later_command_t` (`impo_commands.h`), answered through
`impo_command_reply()`; its `timeout_ms` covers the work.

A product's manifest gives each command a `result` schema, what an ok answer
holds, so the agent and the Impo app (which shows every command as a
prototype, `name(params) → answer`) know it.

## The vocabulary

Every command runs on the gadget and answers within its `timeout_ms` (30 s
unless the command says otherwise). Parameters are JSON; text is UTF-8 unless
a command says ASCII. "Any board with the full UI" means the boards that run
the avatar, captions and settings (`CONFIG_IMPO_ENABLED`).

### `display.*` — the screen

| Command | Who has it | Parameters | Result payload |
|---|---|---|---|
| `display.show_text` | full UI; Xteink X4 | `text` (1–200 chars, ASCII shown as is; others replaced) | — |
| `display.draw_url` | boards that draw images | `url` (public https image), `row`? | — |
| `display.get_info` | Xteink X4 canvas | — | dimensions, formats, colors, `busy` |
| `display.clear` | Xteink X4 canvas | `x`?, `y`?, `width`?, `height`?, `color`?, `refresh`? | `buffered`, `queued` |
| `display.draw_text` | Xteink X4 canvas | `text`, `x`?, `y`?, `width`?, `height`?, `scale`?, `color`?, `background`?, `refresh`? | `buffered`, `queued` |
| `display.refresh` | Xteink X4 canvas | — | `buffered`, `queued` |
| `display.set_power_off_image` | Xteink X4 canvas | `reset`? | `stored`, `persistent`, `source`, dimensions |
| `display.show_animation` | status-light boards | — | — |
| `avatar.cheer` | full UI | — | — |

On X4, `display.show_text` accepts ASCII only (1–200 characters), queues a monochrome page and returns
`{"ok":true,"payload":{"queued":true}}`. Use `display.draw_url` for rendered Chinese text.
`display.show_animation` restores its static connection-status page.

The X4 additionally declares a **480×800 canvas** in `capabilities.screen`.
The agent chooses its own layout; no image/text areas are reserved. Coordinates
are integer pixels, origin top left, x right and y down. `display.clear` fills
a rectangle (default whole screen, white). `display.draw_text` supports 1–512
ASCII characters, newline/wrap, scale 1–8 (default 3, a 18×24 pixel cell), black
or white ink and transparent/white/black background. Text clips to its rectangle;
pixels outside it remain unchanged. Missing rectangle fields default to the
remaining screen. Invalid or out-of-bounds parameters fail before drawing.

On X4, `display.draw_url` accepts **all four** `x,y,width,height` to preserve
other regions. Do not combine them with `row`. JPEGs fit and centre within the
rectangle using 1, 1/2, 1/4 or 1/8 scaling; raw RGB565 has exactly `width` pixels
per row and up to `height` rows. Render/resize on the agent for exact placement,
Chinese, other Unicode, mathematical notation or custom fonts. Clear the region
first if smaller images must have a blank margin. Omitting the rectangle keeps
the existing whole-page image behavior. HTTP(S) URLs must be reachable by the
device; plain HTTP uses less of the X4's limited memory.

JPEG input must be **8-bit baseline, three-component YCbCr**, including when
the intended image is black and white. Progressive, single-component grayscale
and CMYK JPEGs are unsupported, as are PNG, WebP and GIF. Prepare a compatible
file with Pillow: `im.convert('RGB').save('out.jpg', format='JPEG',
progressive=False, subsampling=2)`. Use a direct URL that returns HTTP 200 and
the actual image bytes; a share/viewer/login page is not an image URL. On
`invalid_image`, convert the source before retrying instead of retrying the
unchanged URL. The asset helper also emits compatible `.jpg` files.

Clear/text default to `refresh:false`; image downloads default to `refresh:true`.
For composition, buffer every operation with `refresh:false`, await each image
result, then call `display.refresh` once. It returns `queued:true`; poll
`display.get_info` until `busy:false` before the next edit. A busy drawing call
returns an error without applying it. The panel performs a full refresh even
when only a small region changed (about 4 seconds). Downloads are streamed, so
an error may leave partial buffered pixels; redraw that region. The canvas is
volatile and resets on reboot. The e-paper retains its last visible image when
powered off. By default, shutdown paints a goodbye page.

To customize that page, compose the desired canvas, await successful drawing
and `busy:false`, then call `display.set_power_off_image` with `{}`. It saves
the current complete canvas, takes no URL, and does not shut down. A successful
reply confirms flash write/readback (`stored:true, persistent:true`). Subsequent
drawing does not change the saved image; it survives reboot and is loaded
locally after the network stops at shutdown. `{"reset":true}` restores the
default page and returns `stored:false`. `display.get_info.power_off_image`
reports `supported`, `stored` and `persistent`. Saving a picture while it is
still downloading or refreshing returns `busy`.

See [the canvas example](esp32/devices/XTEINK_X4_CANVAS.md) for a complete
image/text composition and Unicode asset preparation.

Showing text wakes the screen. The caption stays until the next thing the
gadget shows.

### `speaker.*` — sound

| Command | Who has it | Parameters | Result payload |
|---|---|---|---|
| `speaker.play` | any speaker with PSRAM | `id`: a sound the gateway staged | `{"played_ms", "bytes"}` |
| `speaker.play_url` | any speaker with PSRAM | `url`: a public http(s) MP3 or 16-bit PCM WAV | `{"played_ms", "bytes"}` |
| `speaker.stop` | any speaker | — | — |
| `speaker.set_volume` | any speaker | `percent` 0–100 | `{"percent"}` |
| `speaker.status` | any speaker | — | `{"status": idle/fetching/playing/failed, "error"?, "last_url"?, "volume_percent", "speaker_on"}` |
| `speaker.say` | the gateway, for a board with `speaker.play` | `text` 1–300 chars, any language | `{"played_ms", "bytes"}` |

The board plays bytes; it never makes them. `play` and `play_url` answer once
the sound has played to its end; `stopped` (with `played_ms`) when
`speaker.stop` or a new sound ended it; `invalid_audio`, `not_found`,
`download_failed` (with the HTTP status), or `too_long` after 110 s. A new
sound stops the one playing. `play` fetches the staged bytes over Link's own
session, `GET /audio/<id>?offset&length`, 32 KB at a time while the player has
room; `play_url` follows up to 5 redirects.

`speaker.say` is not in the firmware: the gateway makes the speech (the Impo
API's text to speech, in Impo's voice), stages it and sends `speaker.play`,
and answers when that does. No speech service key is ever on a gadget.

### `camera.*` — pictures

| Command | Who has it | Parameters | Result payload |
|---|---|---|---|
| `camera.capture` | boards with a camera | — | `{"format": "jpeg-base64", "data_base64"}` |

The Impo server turns the result into a file in the agent's sandbox and hands
the agent its id, so the model sees the picture rather than its bytes.
`timeout_ms` is 30000.

### `imu.*` — motion

| Command | Who has it | Parameters | Result payload |
|---|---|---|---|
| `imu.read` | `motion_sensor` | — | `{"orientation", "moving", "accel_g": {x,y,z}, "gyro_dps": {x,y,z}}` |

`orientation` is one of `upright`, `upside_down`, `face_up`, `face_down`,
`on_left_side`, `on_right_side`, `tilted`. `moving` is true while the gadget is
being shaken or turned. The SparkBot and the M5Stack CoreS3 have it; on the
StackChan the CoreS3 is the head, so `orientation` is the head's.

### `chassis.*` — moving about

| Command | Who has it | Parameters | Result payload |
|---|---|---|---|
| `chassis.drive` | `locomotion` | `forward` −1..1, `turn` −1..1, `duration_ms` ≤ 10000 (500) | `{"base", "moving"}` |
| `chassis.move` | `locomotion` | `direction` forward/back/left/right/stop, `duration_ms`? | `{"base", "moving"}` |
| `chassis.stop` | `locomotion` | — | `{"base", "moving"}` |
| `chassis.dance` | `locomotion` | — | `{"base", "moving"}` |
| `chassis.set_light` | `lights` | `effect` on/blink/breathe_slow/breathe_fast/flowing/show/off | `{"base", "moving"}` |
| `chassis.status` | `locomotion` | — | `{"base", "moving"}` |

A drive stops by itself when its time is up, and whenever the gadget stops
hearing the base. `base` is `responding`, `silent` or `unknown`: whether the
base answered the gadget, which proves the link, not the movement.

On the Freenove 4WD car the base is its PCA9685 motor driver, `chassis.move`
also takes `speed`? 0.1–1 (0.5), and a drive also stops when the Link session
drops. Its results add `stopped` (why the last drive ended: `time_up`,
`stop`, `link_lost`, `base_error`), `battery_v` and, without a charged
battery, a `warning`: the wheels run only on its cells. Only its wheels are
wired so far, so it registers `chassis.drive`, `move`, `stop` and `status`.

### `head.*` — a head that turns

| Command | Who has it | Parameters | Result payload |
|---|---|---|---|
| `head.move` | `head` | `yaw`? −120..120°, `pitch`? 0..85°, `speed`? 100–1000 (150) | `{"yaw", "pitch", "servos_revived"?}` measured |
| `head.play` | `head` | `frames`: 1–40 of `{yaw?, pitch?, ms 80–3000, speed? (700)}`, 20 s in all | `{"duration_ms", "reached": [{yaw, pitch} or null], "rest": {yaw, pitch}, "servos_revived"?}` |
| `head.stop` | `head` | — | — |
| `head.status` | `head` | — | `{"yaw", "pitch", "moving", "holding"}` measured |

`head.move` answers once the head has settled, with the angles the servos
measured: `not_reached` when it stopped more than 5° short. `head.play` sends
the head to each frame in turn (nods, shakes and dances are frames the agent
composes) and answers once it rests: `reached` is what each frame got to as it
ended (null where the servos did not answer, common while moving), and
`not_reached` when the rest is more than 5° off the last frames. A new move, or
`head.stop`, ends the one under way, which answers `stopped`.

On the M5Stack StackChan both servos sometimes stop answering at once during
pitch moves, while the PY32 still holds their power on. A move then switches
their power off and on (about 2 s), moves again once and says
`servos_revived`; `servos_silent` when they stay silent. A reading counts only
when no servo reply came back short meanwhile: the vendor code otherwise
answers with its own animation's value. Yaw 0 and pitch 0 look straight ahead.

### `lights.*` — lights in one colour

| Command | Who has it | Parameters | Result payload |
|---|---|---|---|
| `lights.set` | `lights` | `color` (red, orange, yellow, green, cyan, blue, purple, pink, white, warm, off or `#rrggbb`), `brightness`? 1–100 (40) | — |
| `lights.off` | `lights` | — | — |

The StackChan's 12 LEDs in its body. They stay until changed; a head touch
glows them pink for a moment and then shows them again.

### `sensors.*` — the surroundings

| Command | Who has it | Parameters | Result payload |
|---|---|---|---|
| `sensors.read` | `environment_sensors` | — | CO₂, tVOC, temperature, humidity, each with its age; null when absent |

### `device.*` — the gadget itself

| Command | Who has it | Parameters | Result payload |
|---|---|---|---|
| `device.health` | every gadget | — | `{"overall", "metrics": {uptime, heap, battery_pct, battery_mv, charging, usb_power}, "network_ssid", "version"}` |
| `device.set_name` | every gadget | `name` (1–32 characters) | — ; the gadget stores the name, reconnects under it, and the account's gadget list shows it from then on |
| `device.discover` | gadgets with the tunnel | — | devices found on the home network |
| `device.ota` | gadgets with OTA on | `url`, `force`? | — (not offered to the agent) |

## Events

The gadget can call the agent too: `link.event` on the control stream, with
`{"event": "<name>", "data": {...}, "uptime_ms"}`. The gateway posts it to the
gadget's own standing task ("[Gadget event] Desk Bot: shaken (peak_g=2.3,
strength=hard)", plus what its manifest says it means), never to the person's
conversation, and the agent's reply is spoken by the gadget. Events are
for things worth a remark, debounced on the gadget; the gateway ignores a
repeat of the same event from the same gadget within three seconds.

| Event | Who sends it | Data |
|---|---|---|
| `shaken` | `motion_sensor` | `peak_g`, `strength` gentle/hard |
| `picked_up` | `motion_sensor` | `orientation` it ended up in |
| `put_down` | `motion_sensor` | `orientation` (upright) |
| `head_touched` | `head_touch` | `how` pat/stroke, at most every 5 s |

A board sends one with `impo_link_send_event(name, data)`; a new event name
gets a row here first. What it means on a device goes in the product's
manifest.

What happens next is the agent's call, guided by the manifest's system
prompt. The board itself does nothing on an event: no sound, light or move of
its own (What a board does, above).

## Memory

One ESP32-S3 holds Wi-Fi, BLE, TLS, the UI, the Link session and the voice
session before any capability gets a byte, and what is left (about 40 KB of
internal RAM in pieces, a few MB of PSRAM) is shared by all of them. So a
capability's memory is decided up front, not taken when used:

| Capability | Memory | When |
|---|---|---|
| Replies spoken (`tts_data` → `decode`) | 512 KB PSRAM ring + decoder | at start-up, once |
| `speaker.play`, `speaker.play_url` (`impo_sound`) | 160 KB in + 64 KB out, PSRAM; a 32 KB decoder task | at start-up, once |
| `camera.capture` | 2 × 614 KB frames in PSRAM, 8 KB DMA pieces in internal RAM, a 614 KB copy and a 150 KB JPEG | for the second a photo takes, then given back |
| `camera.capture` on the M5Stack CoreS3 | a 32 KB DVP DMA buffer in internal RAM and a 150 KB frame in PSRAM, both held while the camera streams from start-up; a 150 KB copy and the JPEG per photo | at start-up; the copy for the photo |
| `imu.read`, the motion watcher | a 3 KB task (4 KB in PSRAM on the CoreS3) | at start-up |

Rules that keep it that way: stream, never buffer a whole file (a sound of
any length costs the same); release what a command took before it returns
(the camera stops after each photo); never let a worst-case constant size an
allocation; and tell memory failures apart from bad data in the error. The
numbers to watch are `heap_largest_internal` and `heap_largest_psram` in
`device.health`: the largest piece, not the total, is what a DMA buffer or
a frame needs.

## Adding to this

- A new board implements existing capabilities through `impo_board_t`
  (screen, audio, buttons, power, camera registration) and lists its extras in
  `features`; see `esp32/components/impo/boards/README.md`.
- A new command for one board goes in that board's `commands` table.
- A command two boards share moves to the common table in
  `esp32/components/impo/impo_commands.c` and gets a row here, with its
  parameters and result payload spelled out before anything depends on it.
- The Linux client (`linux/`) is the reference implementation of the common
  capabilities without hardware: a laptop's screen, speaker, microphone and
  camera.
