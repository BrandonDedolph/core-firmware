/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/layout.h — the list screens' vertical geometry, shared by every
 * module that paints the status strip, the header or list rows. Moved
 * verbatim out of kernel/main.c.
 */

#ifndef CORE_APP_LAYOUT_H
#define CORE_APP_LAYOUT_H

/* Vertical layout (menus.jsx): a status strip up top, a titled header with a
 * divider, then the scrolling list. */
#define STATUS_Y0  0                      /* status strip band (battery/track)    */
#define STATUS_H   15                     /* strip height                         */
#define HDR_BASE   30                     /* header title text baseline           */
#define HDR_DIV_Y  38                     /* header divider row                    */
#define LIST_Y0    42                     /* first list row top                   */
#define ROW_H      24                     /* px per single-line list row          */
#define LIST_ROWS  8                       /* visible rows: (240-42)/24 ~= 8       */
/* Two-line rows (album/song list: title + artist sub) get a taller row so the
 * bold title (bold_13, 19px ink) and the artist line (regular_9, 14px) don't
 * vertically overlap — 32px clears both so the marquee is cleanly title-only
 * (no artist repaint, no clipping of descenders). Fewer fit on screen. */
#define ROW_H2     32
#define LIST_ROWS2 6                       /* (240-42)/32 ~= 6                     */

#endif /* CORE_APP_LAYOUT_H */
