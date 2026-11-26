/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/resume_match.h — resume_find_song(), the resume locator
 * resolution (resume_match.c has the rationale). Moved verbatim out of
 * app/resume.c; pure over the g_songs table, so the host test links it.
 */

#ifndef CORE_APP_RESUME_MATCH_H
#define CORE_APP_RESUME_MATCH_H

#include <stdint.h>

int resume_find_song(uint32_t hash, uint32_t total_s);

#endif /* CORE_APP_RESUME_MATCH_H */
