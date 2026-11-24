/*
 * Copyright (c) Impo contributors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdbool.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Commands a gadget with the full UI offers its account, beyond what Home Link
 * registers itself. link.register names each one; what it does and its
 * parameters are described in the product's manifest on the Impo platform
 * (contracts/gadget-identity.md in impoai/impo). It runs when a link.invoke
 * names it.
 *
 * A board adds its own with impo_board_t.commands. `run` checks its own
 * parameters and returns a result object it owns no longer: {"ok":true,...} or
 * impo_command_error(). It runs on the Link session task, so it must not block.
 *
 * A result says what happened, never what is about to: a command whose work
 * takes time (a sound playing, a head moving) is an impo_later_command_t. It
 * checks its parameters, starts the work and returns impo_command_later(); when
 * the work has finished or failed it calls impo_command_reply() once with the
 * outcome. `timeout_ms` is registered when the work can take over 30 s. A
 * board adds its own with impo_board_t.later_commands.
 */
typedef struct impo_reply impo_reply_t;
struct impo_reply {
    /* Sends `result` (taken) as the command's answer and frees the reply. */
    void (*send)(impo_reply_t *reply, cJSON *result);
};

typedef struct {
    const char *name;
    cJSON *(*run)(const cJSON *params);
} impo_command_t;

typedef struct {
    const char *name;
    cJSON *(*run_later)(const cJSON *params, impo_reply_t *reply);
    int timeout_ms;
} impo_later_command_t;

/* What run_later returns once it has taken the reply. */
cJSON *impo_command_later(void);
/* True for impo_command_later()'s result: the answer comes through the reply. */
bool impo_command_is_later(const cJSON *result);
/* Answers a run_later command; `result` is taken and wrapped like run's. */
void impo_command_reply(impo_reply_t *reply, cJSON *result);

/* Names every command of this gadget in the link.register "commands_v2" object. */
void impo_commands_describe(cJSON *commands);

/* Adds the link.register "capabilities" object (what this gadget has, as
 * CAPABILITIES.md defines it, from the board and the build). */
void impo_commands_capabilities(cJSON *params);

/* Runs `name` if it is one of these commands; NULL means it is not. A
 * run_later command takes `reply` and returns impo_command_later(); otherwise
 * the caller still owns `reply`. Without a reply, a run_later command is
 * refused. */
cJSON *impo_commands_run(const char *name, const cJSON *params, impo_reply_t *reply);

cJSON *impo_command_ok(void);
cJSON *impo_command_error(const char *code, const char *message);

#ifdef __cplusplus
}
#endif
