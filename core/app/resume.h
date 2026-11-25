/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/resume.h — capture and restore of the playback position across a
 * power cycle (resume.c has the rationale). Moved verbatim out of
 * kernel/main.c.
 *
 * The four g_resume_* stamps and RESUME_SAMPLE_US are exported only for the
 * capture edge detector in run_ui.
 */

#ifndef CORE_APP_RESUME_H
#define CORE_APP_RESUME_H

#include <stdint.h>

#include "../fs/fat32.h"

/* Longest a single track may play before we re-stamp the position. 5 minutes:
 * a full-album track rarely outlives it, so in practice the track-change edge
 * is what fires and this is only the backstop for long mixes and podcasts. */
#define RESUME_SAMPLE_US   300000000u          /* 300 s */

extern uint32_t g_resume_open_seq;      /* player_open_seq() when last captured */
extern uint32_t g_resume_last_us;       /* when we last considered capturing     */
extern uint32_t g_resume_end_seq;       /* last seen player_end_seq()            */
extern int      g_resume_was_paused;    /* pause state when last captured       */

void resume_capture(void);
void resume_restore(fat32_t *fs);

#endif /* CORE_APP_RESUME_H */
