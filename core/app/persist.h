/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/persist.h — the three disk-write commits behind the cfg_commit
 * gate: settings (config_save), the event log (evlog_flush) and the
 * On-The-Go live list (otg_store_commit). Moved verbatim out of
 * kernel/main.c.
 */

#ifndef CORE_APP_PERSIST_H
#define CORE_APP_PERSIST_H

#include "../kernel/cfg_commit.h"

extern cfg_commit_t g_cfg_commit;

void settings_touch(void);
void settings_commit(int mode);
void evlog_commit(int mode);
void otg_commit(int mode);

#endif /* CORE_APP_PERSIST_H */
