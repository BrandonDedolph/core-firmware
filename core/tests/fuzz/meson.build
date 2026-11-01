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
  'tag_flac':  {'src': ['fuzz_tag_flac.c', '../../codecs/dr_flac/flac.c',
                        '../../codecs/dr_flac/tag_flac.c'],
                'vendored': true},
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

# dr_flac is vendored; silence its noise exactly as codecs/meson.build does.
fuzz_vendored_args = vendored_codec_warning_suppress

fuzz_flags = ['-fsanitize=fuzzer,address,undefined',
              '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer']

fuzz_targets = []
foreach name, h : fuzz_harnesses
  extra = h.get('vendored', false) ? fuzz_vendored_args : []
  corpus  = meson.current_source_dir() / 'corpus' / name
  regress = meson.current_source_dir() / 'regress' / name
  inputs  = [corpus]
  if fs_mod.is_dir('regress' / name)
    inputs += [regress]
  endif

  replay = executable('fuzz_replay_' + name,
    sources: h['src'] + ['fuzz_replay.c'],
    include_directories: fuzz_inc,
    c_args: extra,
    override_options: test_opts)
  test('fuzz-replay-' + name, replay, args: inputs, suite: 'unit')

  if fuzz_with_libfuzzer
    fuzz_targets += executable('fuzz_' + name,
      sources: h['src'],
      include_directories: fuzz_inc,
      c_args: extra + fuzz_flags + ['-g'],
      link_args: fuzz_flags,
      override_options: test_opts)
  endif
endforeach

if fuzz_with_libfuzzer
  alias_target('fuzz', fuzz_targets)
endif
