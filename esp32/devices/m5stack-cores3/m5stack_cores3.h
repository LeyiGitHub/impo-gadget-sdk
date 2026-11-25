/*
 * Copyright (c) Impo contributors.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "impo_commands.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A body the CoreS3 sits in, such as the StackChan's (devices/m5stack-stackchan).
 * Its device folder selects M5STACK_CORES3_BODY and defines m5stack_cores3_body;
 * board.c switches on the 5 V output for it, starts it once the PMU is up and
 * adds its commands and features to the board's.
 */
typedef struct {
    const char *name;                   /* the device's name, "M5Stack StackChan" */
    esp_err_t (*start)(i2c_master_bus_handle_t bus);
    const impo_command_t *commands;
    int command_count;
    const impo_later_command_t *later_commands;
    int later_command_count;
    const char *features;               /* impo_board_t.features */
    const char *description;            /* impo_board_t.description: the whole device's */
} m5stack_cores3_body_t;

extern const m5stack_cores3_body_t m5stack_cores3_body;

#ifdef __cplusplus
}
#endif
