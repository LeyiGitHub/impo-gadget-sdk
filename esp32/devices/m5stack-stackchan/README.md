# M5Stack StackChan

[StackChan](https://docs.m5stack.com/en/StackChan): a CoreS3 (`../m5stack-cores3`,
this device's base) on M5Stack's robot body, whose head turns and tilts on two
Feetech SCS0009 servos, with a touch strip on top and RGB LEDs on its left and right.

    tools/impo/board.sh build stackchan && tools/impo/board.sh flash stackchan

Impo's build replaces M5Stack's firmware. Back that up first
(`esptool.py read-flash 0 0x1000000 stackchan.bin`) if you may want it back.

| File | What it is |
| --- | --- |
| `vendor/m5stack/` | M5Stack's StackChan code, unchanged: servo, IO expander and touch drivers, and the robot (motion with spring easing and stall protection, LEDs, keyframe animations, dances) |
| `port/` | what that code expects from the rest of M5Stack's firmware |
| `stackchan.cpp` | the body's commands: the tools M5Stack's firmware gives its AI, done as it does them |
| `description.json` | what the StackChan is and what each command does, in M5Stack's words; the firmware sends it as written, and `tests/test_m5stack_stackchan.py` checks it against the code |

## Commands

Each is a tool of M5Stack's StackChan firmware v1.6.2 (`hal_mcp.cpp`, and
xiaozhi-esp32 v2.5.2's `mcp_server.cc` under it), done as it does it:

| Command | M5Stack's tool |
| --- | --- |
| `head.status` | `self.robot.get_head_angles` |
| `head.move` | `self.robot.set_head_angles`: answers at once, as M5Stack's does |
| `lights.set` | `self.robot.set_led_color` |
| `speaker.set_volume` | `self.audio_speaker.set_volume` |
| `display.set_brightness` | `self.screen.set_brightness` (Impo's screen goes no darker than 10%) |
| `camera.capture` | `self.camera.take_photo`: the photo goes to Impo, which looks at it |
| `display.show_text`, `speaker.play` | its AI's caption and speech |
| `device.health` (the platform's) | `self.get_device_status`: volume and brightness as well |

A command M5Stack's firmware has no tool for is not offered, and the board
sends no events: M5Stack's firmware tells its AI of no touch or shake. The
CoreS3's motion sensor is left off (`shared_commands` in `stackchan.cpp`).
Captions show Chinese and Japanese (`CONFIG_IMPO_CJK_FONT`).

M5Stack's face, with its expressions, idle motion and reminders, is not built
yet: Impo's face is on the screen.

## Bench checks over USB

    >run=head.move {"yaw":30,"pitch":20}
    >run=head.status
    >run=lights.set {"red":0,"green":168,"blue":0}
    >snap                      (tools/impo/photo.py saves the JPEG)
