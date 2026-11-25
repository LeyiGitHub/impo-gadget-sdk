# SPDX-License-Identifier: Apache-2.0
# M5Stack StackChan (device.json): the body under the CoreS3. vendor/m5stack is
# M5Stack's code, unchanged (vendor/README.md); port/ is what that code expects
# from the rest of M5Stack's firmware; stackchan.cpp gives Impo the tools
# M5Stack's firmware gives its AI, done as it does them.
set(m5 "vendor/m5stack")
idf_component_register(
    SRCS "stackchan.cpp"
         "port/hal_impo.cpp"
         "${m5}/hal/hal_servo.cpp"
         "${m5}/hal/hal_io_expander.cpp"
         "${m5}/hal/hal_head_touch.cpp"
         "${m5}/hal/drivers/FTServo_Arduino/src/SCS.cpp"
         "${m5}/hal/drivers/FTServo_Arduino/src/SCSCL.cpp"
         "${m5}/hal/drivers/FTServo_Arduino/src/SCSerial.cpp"
         "${m5}/hal/drivers/PY32IOExpander_Class/PY32IOExpander_Class.cpp"
         "${m5}/hal/drivers/Si12T/Si12T.cpp"
         "${m5}/stackchan/stackchan.cpp"
         "${m5}/stackchan/motion/motion.cpp"
         "${m5}/stackchan/motion/servo.cpp"
         "${m5}/stackchan/animation/animation.cpp"
         "${m5}/stackchan/addons/neon_light/neon_light.cpp"
         "vendor/xiaozhi/settings.cc"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "port" "port/hal" "${m5}" "vendor/xiaozhi"
    EMBED_TXTFILES "description.json"   # what the StackChan is and does, sent in link.register
    REQUIRES m5stack-cores3
    PRIV_REQUIRES impo nvs_flash esp_driver_uart esp_driver_i2c esp_timer
    WHOLE_ARCHIVE   # m5stack_cores3_body and M5Stack's static objects
)

# M5Stack's code is built as M5Stack builds it, not under this project's
# warnings-as-errors.
file(GLOB_RECURSE vendored CONFIGURE_DEPENDS "${COMPONENT_DIR}/vendor/*.c" "${COMPONENT_DIR}/vendor/*.cc" "${COMPONENT_DIR}/vendor/*.cpp")
set_source_files_properties(${vendored} PROPERTIES COMPILE_OPTIONS "-Wno-error")
# M5Stack's headers, which stackchan.cpp includes, leave FeatureKeyframe's
# `size` unset (animation.h).
set_source_files_properties("stackchan.cpp" PROPERTIES COMPILE_OPTIONS "-Wno-error=uninitialized;-Wno-error=maybe-uninitialized")
