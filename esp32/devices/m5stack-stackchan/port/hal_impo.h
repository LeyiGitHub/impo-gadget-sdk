/*
 * Copyright (c) Impo contributors.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <driver/i2c_master.h>

/* The bus hal_bridge::board_get_i2c_bus() hands to vendor/m5stack. Set it
 * before GetHAL().init(). */
void hal_impo_set_i2c_bus(i2c_master_bus_handle_t bus);

/* How many servo replies have failed to arrive whole since start-up. The
 * servos' getCurrentAngle() answers with the animation's own value when its
 * read fails, so a reading is the servo's only if this did not change. */
unsigned hal_impo_servo_short_reads(void);
