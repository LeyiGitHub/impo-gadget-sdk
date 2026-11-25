# Code from elsewhere

Copied unchanged; fix bugs upstream or in Impo's own files around it
(`../port/`, `../stackchan.cpp`), never here, so a newer copy can replace these
files as they are.

| Folder | Source | Version | Licence |
| --- | --- | --- | --- |
| `m5stack/hal/` (`hal_servo.cpp`, `hal_io_expander.cpp`, `hal_head_touch.cpp`, `drivers/FTServo_Arduino`, `drivers/PY32IOExpander_Class`, `drivers/Si12T`) | [m5stack/StackChan](https://github.com/m5stack/StackChan) `firmware/main/hal/` | firmware v1.6.2, commit `28b9f0c8bd6acd81cb3c70f1a93c3e373b39d400` | MIT (`m5stack/LICENSE`); FTServo_Arduino has its own MIT licence (`drivers/FTServo_Arduino/LICENSE`) |
| `m5stack/stackchan/` | the same, `firmware/main/stackchan/` | the same | MIT |
| `xiaozhi/settings.*` | [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) `main/settings.*`, which M5Stack's servo code keeps its zero positions with | v2.5.2, commit `49b4380b3faa0ceadd28a3e78aaf0f311c46c57e` | MIT (`xiaozhi/LICENSE`) |

What M5Stack's code expects from the rest of its firmware (its `Hal` and
`hal_bridge`) is in `../port/`: M5Stack's declarations, cut down, and Impo's
definitions of them. The libraries it is built on, smooth_ui_toolkit and
mooncake_log, come through the component manager at the versions M5Stack pins
(`../idf_component.yml`).

The face code in `m5stack/stackchan/avatar/` is not built yet: Impo draws its
own face.
