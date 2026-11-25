/*
 * Copyright (c) Impo contributors.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <driver/i2c_master.h>

/* The bus hal_bridge::board_get_i2c_bus() hands to vendor/m5stack. Set it
 * before GetHAL().init(). */
void hal_impo_set_i2c_bus(i2c_master_bus_handle_t bus);
