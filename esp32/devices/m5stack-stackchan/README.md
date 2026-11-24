# M5Stack StackChan

[StackChan](https://docs.m5stack.com/en/StackChan): a CoreS3 (`../m5stack-cores3`,
this device's base) on M5Stack's robot body, whose head turns and tilts on two
Feetech SCS0009 servos, with a touch strip on top and 12 RGB LEDs.

    tools/impo/board.sh build stackchan && tools/impo/board.sh flash stackchan

Impo's build replaces M5Stack's firmware. Back that up first
(`esptool.py read-flash 0 0x1000000 stackchan.bin`) if you may want it back.

| File | What it is |
| --- | --- |
| `vendor/m5stack/` | M5Stack's StackChan code, unchanged: servo, IO expander and touch drivers, and the robot (motion with spring easing and stall protection, LEDs, keyframe animations, dances) |
| `port/` | what that code expects from the rest of M5Stack's firmware |
| `stackchan.cpp` | Impo on top: the body's commands and its touch event |
| `manifest.py` | the product manifest the agent reads; `impo.py manifest push` sends it |

## Commands and events

The CoreS3's `imu.read` and `camera.capture`, then the body's `head.move`
(yaw, pitch, speed; answers once settled, with the measured angles),
`head.play` (frames the agent composes: nods, shakes, dances), `head.stop`,
`head.status`, `lights.set` and `lights.off` (CAPABILITIES.md). Events:
`head_touched` (pat or stroke) and `shaken`.

A touch or a shake is only its event: the board does not move, light or
change its face by itself (CAPABILITIES.md, What a board does). Pick-up
detection is off, as M5Stack leaves it: the motion sensor is in the head,
which moves. When both servos stop answering during a move, the board
power-cycles them and moves again once (`servos_revived`).

## Bench checks over USB

    >run=head.move {"yaw":30,"pitch":20}
    >run=head.play {"frames":[{"yaw":25,"ms":300},{"yaw":-25,"ms":300},{"yaw":0,"ms":400}]}
    >run=imu.read
    >snap                      (tools/impo/photo.py saves the JPEG)
