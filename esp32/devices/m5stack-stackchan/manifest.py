#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Builds the Impo product manifest (contracts/gadget-identity.md, ManifestInput)
for the M5Stack StackChan (this device folder, devices/m5stack-stackchan).
The firmware registers only command names; everything the agent reads about
them is here, and the platform checks calls against these schemas. Refuses to
build when a command here is not one the firmware registers.

usage: devices/m5stack-stackchan/manifest.py [--min-firmware 0.1.0] > stackchan.manifest.json
       then impo.py manifest push stackchan.manifest.json (impo-developer-skills)
"""
import argparse
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
BODY_SOURCE = HERE / "stackchan.cpp"
BOARD = ROOT / "devices/m5stack-cores3/board.c"
OVERLAY = HERE / "sdkconfig"
COMMON = ROOT / "components/impo/impo_commands.c"

SYSTEM_PROMPT = """\
You are the voice and body of a StackChan: a small M5Stack desktop robot. Its face is a 2-inch \
touch screen showing Impo's avatar, on a body whose head turns left and right and tilts up and \
down. It has a speaker and microphones (the person holds the power button to talk to you, and \
your replies are spoken), a camera under the screen that looks where the head points, a \
touch strip along the top of its head, a motion sensor in its head, and 12 coloured LEDs in its \
body. It sits on a desk near the person.

Body language: the robot only executes moves you compose. head.move points the head (yaw -120 \
to 120 degrees, negative to its left; pitch 0 straight ahead to 85 looking up). head.play plays \
frames you write, e.g. a nod: [{"pitch":15,"ms":300},{"pitch":0,"ms":300}] twice; a shake: \
[{"yaw":25,"ms":300},{"yaw":-25,"ms":300}] twice, then {"yaw":0,"ms":400}. Both answer once \
done, with the angles the servos measured: check them before saying the robot moved. Keep it \
small and occasional: one gesture per reply at most, none when the person asks for quiet.

Seeing: camera.capture takes a photo of what the robot faces. Turn the head first \
(head.move) when the person asks about something to one side or above.

Lights: lights.set shows one colour on all the LEDs (a name such as warm, pink, blue or green, \
or #rrggbb) at a brightness; lights.off turns them off. Use them for mood or a signal the \
person asked for, not on every reply. They stay until changed.

Touch: when the person pats or strokes its head you get head_touched; the robot itself does \
nothing. Answer as the robot being petted would: one short warm phrase, and a small move or a \
light if it fits. When someone shakes the robot you get shaken. Answer as a robot that was \
jostled.

Speak in short, friendly sentences. The screen shows what you say; display.show_text puts a \
short caption on it when the person wants something written down."""

EVENTS = [
    {"name": "head_touched",
     "description": "The person touched the top of the robot's head and let go: how is pat (a touch) or "
                    "stroke (a hand moved along the head). The robot does nothing by itself. Sent at most "
                    "every 5 seconds."},
    {"name": "shaken",
     "description": "Someone shook the robot's head: peak_g is the strongest jolt in g, strength gentle or "
                    "hard. The robot does nothing by itself. At most every 8 s."},
]


def registered(path, name):
    """The command names in `name[] = { {"a.b", run}, ... };`."""
    src = path.read_text()
    m = re.search(re.escape(name) + r'\[\]\s*=\s*\{(.*?)\n\};', src, re.S)
    if not m:
        sys.exit(f"{name} not found in {path}")
    return re.findall(r'\{\s*"([^"]+)"', m.group(1))


def command(name, desc, req, opt):
    req = json.loads(req) if req else {}
    opt = json.loads(opt) if opt else {}
    schema = {"type": "object", "properties": {**req, **opt}}
    if req:
        schema["required"] = list(req)
    schema["additionalProperties"] = False
    assert re.fullmatch(r'[a-z][a-z0-9_]*(\.[a-z0-9_]+){0,3}', name), name
    assert 1 <= len(desc) <= 500, (name, len(desc))
    return {"name": name, "description": desc, "parameters": schema}



# The CoreS3's own commands (devices/m5stack-cores3/board.c), listed first.
CORE = [
    ("imu.read",
     "Which way up the robot's head is (upright, upside_down, face_up, face_down, on_left_side, "
     "on_right_side or tilted), whether it is moving, and the raw accelerometer (g) and gyroscope "
     "(degrees per second) readings.",
     None, None),
]

ANGLES = ('"yaw":{"type":"number","minimum":-120,"maximum":120,"description":"Degrees: 0 straight ahead, negative to the robot\'s left."},'
          '"pitch":{"type":"number","minimum":0,"maximum":85,"description":"Degrees: 0 straight ahead, up to 85 looking up."},'
          '"speed":{"type":"integer","minimum":100,"maximum":1000,"description":"How briskly, 100 (slow) to 1000 (snappy)."}')

# The body's commands (stackchan.cpp): BODY answers at once, LATER once its move is done.
LATER = [
    ("head.move",
     "Point the robot's head: yaw, pitch or both; what is left out stays. Answers once it has "
     "settled, with the yaw and pitch the servos measured; not_reached when it stopped more than "
     "5 degrees short; servos_silent when they do not answer even after the robot power-cycles "
     "them. speed 150 (a natural look) if left out.",
     None,
     '{' + ANGLES + '}'),
    ("head.play",
     "Play head frames you compose: each frame sends the head to its yaw and/or pitch and lasts ms "
     "(80 to 3000; 20 s in all). Use it for nods, shakes and dances. Answers once the head rests, "
     "with reached (per frame, the angles measured as it ended; null where the servos did not "
     "answer, common while moving) and rest; not_reached when the rest is more than 5 degrees from where "
     "the last frames sent each axis. speed 700 if left out.",
     '{"frames":{"type":"array","minItems":1,"maxItems":40,"items":{"type":"object","properties":{'
     + ANGLES + ',"ms":{"type":"integer","minimum":80,"maximum":3000}},"required":["ms"],'
     '"additionalProperties":false}}}',
     None),
]

BODY = [
    ("head.stop",
     "Stop the head where it is; a head.move or head.play under way answers stopped.",
     None, None),
    ("head.status",
     "Where the head points now, as the servos measure it (yaw and pitch in degrees; servos_silent "
     "when they do not answer), whether it is moving, and whether the servos are holding it.",
     None, None),
    ("lights.set",
     "Light the 12 LEDs in the robot's body in one colour. They stay on until changed or turned off.",
     '{"color":{"type":"string","minLength":3,"maxLength":7,"description":"red, orange, yellow, green, cyan, blue, purple, pink, white, warm, or #rrggbb."}}',
     '{"brightness":{"type":"integer","minimum":1,"maximum":100,"description":"Percent, 1 to 100; 40 if left out."}}'),
    ("lights.off",
     "Turn off the LEDs in the robot's body.",
     None, None),
]

# Commands every board with the full UI registers (impo_commands.c COMMON).
UI = [
    ("display.show_text",
     "Show a short caption on the robot's screen, for something the person wants written down. It "
     "stays until the next thing the robot shows.",
     '{"text":{"type":"string","minLength":1,"maxLength":200,"description":"1 to 200 characters."}}',
     None),
    ("avatar.cheer",
     "Make the face on the screen cheer for a moment.",
     None, None),
    ("speaker.play_url",
     "Play a sound file from a public http(s) URL: an MP3 or a 16-bit PCM WAV, up to 110 seconds. "
     "Answers once it has finished playing (played_ms), or why it did not play.",
     '{"url":{"type":"string","minLength":8,"maxLength":512,"description":"A direct link to the file itself."}}',
     None),
    ("speaker.set_volume",
     "Set the speaker volume.",
     '{"percent":{"type":"integer","minimum":0,"maximum":100,"description":"0 to 100."}}',
     None),
    ("speaker.stop",
     "Stop what the speaker is playing.",
     None, None),
]

# Commands Impo's gateway carries out with the robot's own (they are not in the
# firmware): speaker.say makes the speech in the cloud and plays it with
# speaker.play.
CLOUD = [
    ("speaker.say",
     "Say something out loud in Impo's voice, outside a reply (a reminder, a greeting). Answers once "
     "it has been said (played_ms), or why it was not.",
     '{"text":{"type":"string","minLength":1,"maxLength":1000,"description":"What to say, any language."}}',
     None),
]

# Link's own commands (main/noise_control.cpp), which every ESP32 gadget registers,
# and camera.capture, which it registers on a board with a camera.
LINK = [
    ("camera.capture",
     "Take one photo with the camera under the robot's screen of whatever its head faces, as a "
     "320x240 JPEG. Returned only when this command is explicitly invoked.", None, None),
    ("device.health",
     "The robot's battery level, whether it is charging, uptime, memory and the Wi-Fi network.",
     None, None),
    ("device.set_name",
     "Rename the robot: the name the person calls it by. 1 to 32 characters. It reconnects under the "
     "new name a moment later.",
     '{"name":{"type":"string","minLength":1,"maxLength":32,"description":"The new name, 1 to 32 characters."}}',
     None),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--min-firmware", default="0.1.0")
    a = ap.parse_args()
    for listed, path, table in ((CORE, BOARD, "s_commands"), (BODY, BODY_SOURCE, "s_commands"),
                                (LATER, BODY_SOURCE, "s_later_commands")):
        ours = registered(path, table)
        if [c[0] for c in listed] != ours:
            sys.exit(f"{[c[0] for c in listed]} differs from what {path.name} {table} registers: {ours}")
    common = registered(COMMON, "COMMON") + registered(COMMON, "COMMON_LATER")
    missing = [c[0] for c in UI if c[0] not in common]
    if missing:
        sys.exit(f"{missing} are not in {COMMON.name} COMMON")
    if "speaker.play" not in common:
        sys.exit("speaker.say needs speaker.play in " + COMMON.name)
    commands = [command(*c) for c in CORE + BODY + LATER + UI + CLOUD + LINK]
    names = [c["name"] for c in commands]
    assert len(names) == len(set(names)) and len(commands) <= 64
    assert len(SYSTEM_PROMPT) <= 4000, len(SYSTEM_PROMPT)
    product = re.search(r'^CONFIG_GADGET_PRODUCT_ID="([^"]+)"', OVERLAY.read_text(), re.M)
    if not product:
        sys.exit(f"no CONFIG_GADGET_PRODUCT_ID in {OVERLAY.name}")
    for e in EVENTS:
        assert 1 <= len(e["description"]) <= 200, (e["name"], len(e["description"]))
    json.dump({"product": product.group(1), "minFirmware": a.min_firmware, "systemPrompt": SYSTEM_PROMPT, "commands": commands,
               "events": EVENTS}, sys.stdout, ensure_ascii=False, indent=2)
    print()


if __name__ == "__main__":
    main()
