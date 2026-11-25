# The hardware-coupled application layer, split out of kernel/main.c one
# module at a time (state, layout, util first; screens and services after).
#
# These are NOT a static_library: a move must not change how the code is
# compiled, so app_sources is compiled exactly as main.c is. On hw it joins
# kernel_sources in core_elf's own source list (project arguments only, no
# -DCORE_FREESTANDING; include dirs from hal_dep and codec_inc); on the host
# it joins headless_fw_src in tests/meson.build. A new app/*.c is added here
# and nowhere else.
app_sources = files('state.c', 'util.c', 'marquee.c', 'bootscreen.c', 'clockui.c', 'persist.c', 'nav.c', 'library.c', 'battery_ui.c', 'playctx.c', 'statusbar.c', 'playlists.c', 'albums.c', 'nowplaying.c')
