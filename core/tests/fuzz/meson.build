# Fuzz harnesses: one LLVMFuzzerTestOneInput() per parser that reads bytes
# off the user's disk. See tests/fuzz/fuzz.h for what each one counts as a
# finding, and docs/testing-fuzz.md for running them.
#
# Every harness is built TWICE from the same sources:
#
#   fuzz-replay-<name>  ALWAYS, in the ordinary host build, linked with
#                       fuzz_replay.c's main(). A `meson test` unit that runs
#                       the committed seed corpus and every regression input
#                       once — libFuzzer's -runs=0, without needing clang. In
#                       the CI sanitizer job it runs under gcc's ASan/UBSan.
#
#   fuzz_<name>         only with -Dfuzz=true (`make fuzz`), with clang and
#                       -fsanitize=fuzzer,address,undefined: the real fuzzer.
#
# The parser sources are compiled INTO each harness, the way the other host
# tests compile them (not through a library), for two reasons: they are the
# SAME .c files the ARM build links, and compiling them per target is what
# puts the fuzzer's coverage instrumentation on the code being fuzzed. Only
# the HAL is stubbed (the ATA write, inside the harness that needs it).

fs_mod = import('fs')

fuzz_inc = include_directories('.', '../../hal', '../../kernel', '../../fs',
                               '../../ui', '../../codecs', '../..')

fuzz_harnesses = {
  'fat32':     {'src': ['fuzz_fat32.c', '../../fs/fat32.c']},
  'm3u':       {'src': ['fuzz_m3u.c', 'fuzz_vol.c', '../../fs/m3u.c',
                        '../../fs/fat32.c']},
  'id3':       {'src': ['fuzz_id3.c', '../../codecs/pvmp3/id3_meta.c',
                        '../../codecs/pvmp3/mp3_frame.c',
                        '../../codecs/meta_text.c']},
  'flac_meta': {'src': ['fuzz_flac_meta.c', '../../codecs/flac_meta.c',
                        '../../codecs/meta_text.c']},
  # dr_flac itself (flac.c is its DR_FLAC_IMPLEMENTATION unit) is built
  # apart, as `vendored`, so it can carry its own flags — see below.
  'tag_flac':  {'src': ['fuzz_tag_flac.c', '../../codecs/dr_flac/tag_flac.c'],
                'vendored': ['../../codecs/dr_flac/flac.c']},
  'mp3_frame': {'src': ['fuzz_mp3_frame.c', '../../codecs/pvmp3/mp3_frame.c']},
  # settings.c for settings_defaults(); eq.c and datetime.c only because
  # settings.c's display helpers reference them, and the coverage
  # instrumentation keeps --gc-sections from dropping those helpers.
  'config':    {'src': ['fuzz_config.c', 'fuzz_vol.c', '../../kernel/config.c',
                        '../../ui/settings.c', '../../ui/eq.c',
                        '../../kernel/datetime.c', '../../fs/fat32.c']},
  'evlog':     {'src': ['fuzz_evlog.c', 'fuzz_vol.c', '../../kernel/evlog.c',
                        '../../kernel/cfg_commit.c', '../../fs/fat32.c']},
  'otg_store': {'src': ['fuzz_otg_store.c', 'fuzz_vol.c',
                        '../../kernel/otg_store.c', '../../kernel/cfg_commit.c',
                        '../../library/otg.c', '../../fs/fat32.c']},
  'idx':       {'src': ['fuzz_idx.c', 'fuzz_vol.c', '../../library/idx.c',
                        '../../fs/fat32.c']},
}

fuzz_with_libfuzzer = get_option('fuzz')
if fuzz_with_libfuzzer and cc.get_id() != 'clang'
  error('-Dfuzz=true needs clang (libFuzzer): CC=clang meson setup ... — ' +
        'or just `make fuzz`')
endif

# dr_flac is vendored and kept byte-identical to upstream (vendor.sh
# re-fetches it), so its findings are reported, not patched. Its noise is
# silenced exactly as codecs/meson.build does, plus ONE sanitizer check:
#
#   shift-base — drflac__le2host_32_ptr_unaligned() builds a little-endian
#   u32 as `*(p+3) << 24` on a promoted int, which is undefined for a byte
#   >= 0x80 (it shifts into the sign bit). Every compiler produces the
#   intended bits, it is reached only through the VORBIS_COMMENT walk that
#   runs when an onMeta callback is given (tag_flac.c; the device's flac.c
#   never passes one), and the fix belongs upstream: a (drflac_uint32) cast.
#   regress/tag_flac/xfail-drflac-le32-shift is the input; delete this flag
#   once a re-vendored dr_flac.h has the cast, and that input will hold it.
#
#   alignment — the CUESHEET reader (dr_flac.h, "Pass 2") packs its
#   drflac_cuesheet_track_index records at the on-disk 12-byte stride after
#   a 36-byte track header and then writes `pTrackIndex->offset` (a u64)
#   through a pointer that is only 4-byte aligned. Harmless on x86; a real
#   misaligned access on an ARM7TDMI — but behind the same onMeta gate, so
#   the device's decoder never parses a CUESHEET. Upstream's to fix (pack
#   to sizeof, or memcpy the u64). Input:
#   regress/tag_flac/xfail-drflac-cuesheet-misaligned.
#
# Same reasoning, and the same mechanism, as the pvmp3 precedent in
# codecs/meson.build. Everything else UBSan checks stays on for dr_flac.
fuzz_vendored_args = vendored_codec_warning_suppress + [
  '-fno-sanitize=shift-base', '-fno-sanitize=alignment']

fuzz_flags = ['-fsanitize=fuzzer,address,undefined',
              '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer']

fuzz_targets = []
foreach name, h : fuzz_harnesses
  vendored = h.get('vendored', [])
  corpus  = meson.current_source_dir() / 'corpus' / name
  regress = meson.current_source_dir() / 'regress' / name
  inputs  = [corpus]
  if fs_mod.is_dir('regress' / name)
    inputs += [regress]
  endif

  vlib = []
  if vendored.length() > 0
    vlib = static_library('fuzz_vendored_replay_' + name, vendored,
      include_directories: fuzz_inc,
      c_args: fuzz_vendored_args,
      override_options: test_opts)
  endif
  replay = executable('fuzz_replay_' + name,
    sources: h['src'] + ['fuzz_replay.c'],
    include_directories: fuzz_inc,
    link_with: vlib,
    override_options: test_opts)
  test('fuzz-replay-' + name, replay, args: inputs, suite: 'unit')

  if fuzz_with_libfuzzer
    vlib = []
    if vendored.length() > 0
      vlib = static_library('fuzz_vendored_' + name, vendored,
        include_directories: fuzz_inc,
        c_args: fuzz_flags + ['-g'] + fuzz_vendored_args,
        override_options: test_opts)
    endif
    fuzz_targets += executable('fuzz_' + name,
      sources: h['src'],
      include_directories: fuzz_inc,
      link_with: vlib,
      c_args: fuzz_flags + ['-g'],
      link_args: fuzz_flags,
      override_options: test_opts)
  endif
endforeach

if fuzz_with_libfuzzer
  alias_target('fuzz', fuzz_targets)
endif
