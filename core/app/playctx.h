/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/playctx.h — the play context: building the player's queue from
 * library songs (the Songs view, Shuffle Songs), and the queue-context
 * triple (kind, seed, playlist hash) every queue builder sets and the
 * resume capture reads. Moved verbatim out of kernel/main.c.
 */

#ifndef CORE_APP_PLAYCTX_H
#define CORE_APP_PLAYCTX_H

#include <stdint.h>

#include "../fs/fat32.h"
#include "../player/player.h"
#include "library.h"

extern int      g_queue_kind;
extern uint32_t g_queue_seed;
extern uint32_t g_queue_ctx_hash;

void queue_entry_from_song(browse_entry_t *e, const lib_song_t *s);
int library_play_song(fat32_t *fs, int songview_idx);
int shuffle_songs_build(fat32_t *fs, uint32_t seed, int start_si);
void shuffle_songs_play(fat32_t *fs);

#endif /* CORE_APP_PLAYCTX_H */
