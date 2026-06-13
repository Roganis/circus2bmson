# Tests / CI gates.

# M0.5: per-channel stems must sum back to the full mix on real modules.
# Observed residual is ~-123 dB (pure float-summation noise); the gates below
# keep a wide margin for cross-platform float differences while still catching
# any real breakage (a dropped/duplicated channel lands near 0 dB).
if(TARGET stem_sum_check)
  add_test(NAME stem_sum_reggae
    COMMAND stem_sum_check
      ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/10k_reggae_dub.mod
      --max-peak 1e-3 --max-rms-db -100)
  add_test(NAME stem_sum_castle
    COMMAND stem_sum_check
      ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/8bit_castle.mod
      --max-peak 1e-3 --max-rms-db -100)
  add_test(NAME stem_sum_xm
    COMMAND stem_sum_check
      ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/neurosys.xm
      --max-peak 1e-3 --max-rms-db -100)
  add_test(NAME stem_sum_it
    COMMAND stem_sum_check
      ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/chip_ultimatum.it
      --max-peak 1e-3 --max-rms-db -100)
endif()

set(C2B_FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

# M1 unit tests (no audio): parser, timeline, bmson emission.
add_executable(test_mod_parse test_mod_parse.cpp)
target_link_libraries(test_mod_parse PRIVATE circus2bmson::circus2bmson)
target_compile_definitions(test_mod_parse PRIVATE C2B_FIXTURE_DIR="${C2B_FIXTURE_DIR}")
add_test(NAME mod_parse COMMAND test_mod_parse)

add_executable(test_timeline test_timeline.cpp)
target_link_libraries(test_timeline PRIVATE circus2bmson::circus2bmson)
target_compile_definitions(test_timeline PRIVATE C2B_FIXTURE_DIR="${C2B_FIXTURE_DIR}")
add_test(NAME timeline COMMAND test_timeline)

# Cross-check timeline timing vs libopenmpt and validate the emitted bmson.
add_executable(test_convert_vs_libopenmpt test_convert_vs_libopenmpt.cpp)
target_link_libraries(test_convert_vs_libopenmpt
  PRIVATE circus2bmson::circus2bmson PkgConfig::LIBOPENMPT)
target_compile_definitions(test_convert_vs_libopenmpt PRIVATE
  C2B_FIXTURE_DIR="${C2B_FIXTURE_DIR}"
  C2B_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_out")
add_test(NAME convert_vs_libopenmpt COMMAND test_convert_vs_libopenmpt)

# M2: keysounds, placed back at their onsets, reconstruct the full mix.
add_executable(test_keysound_fidelity test_keysound_fidelity.cpp)
target_link_libraries(test_keysound_fidelity PRIVATE circus2bmson::circus2bmson)
target_compile_definitions(test_keysound_fidelity PRIVATE
  C2B_FIXTURE_DIR="${C2B_FIXTURE_DIR}")
add_test(NAME keysound_fidelity COMMAND test_keysound_fidelity)

# M2: render path -- naming schemes, WAV/OGG containers, every note bound.
add_executable(test_keysound_render test_keysound_render.cpp)
target_link_libraries(test_keysound_render PRIVATE circus2bmson::circus2bmson)
target_compile_definitions(test_keysound_render PRIVATE
  C2B_FIXTURE_DIR="${C2B_FIXTURE_DIR}"
  C2B_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_out")
add_test(NAME keysound_render COMMAND test_keysound_render)

# bmson emission must tolerate non-UTF-8 text (e.g. Shift-JIS MIDI titles).
add_executable(test_bmson_utf8 test_bmson_utf8.cpp)
target_link_libraries(test_bmson_utf8 PRIVATE circus2bmson::circus2bmson)
add_test(NAME bmson_utf8 COMMAND test_bmson_utf8)

# MIDI: Standard MIDI File parser.
add_executable(test_midi_parse test_midi_parse.cpp)
target_link_libraries(test_midi_parse PRIVATE circus2bmson::circus2bmson)
target_compile_definitions(test_midi_parse PRIVATE
  C2B_FIXTURE_DIR="${C2B_FIXTURE_DIR}")
add_test(NAME midi_parse COMMAND test_midi_parse)

# MIDI end-to-end: parse + FluidSynth render -> bmson (skips without a SoundFont).
add_executable(test_midi_convert test_midi_convert.cpp)
target_link_libraries(test_midi_convert PRIVATE circus2bmson::circus2bmson)
target_compile_definitions(test_midi_convert PRIVATE
  C2B_FIXTURE_DIR="${C2B_FIXTURE_DIR}"
  C2B_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_out")
add_test(NAME midi_convert COMMAND test_midi_convert)

# Audio-domain onset detection (no libgme needed).
add_executable(test_onset test_onset.cpp)
target_link_libraries(test_onset PRIVATE circus2bmson::circus2bmson)
add_test(NAME onset COMMAND test_onset)

# game-music-emu chip backend: load a VGM, render per-voice, detect onsets.
# Self-skips when the build has no libgme.
add_executable(test_chip test_chip.cpp)
target_link_libraries(test_chip PRIVATE circus2bmson::circus2bmson)
add_test(NAME chip COMMAND test_chip)

# Input format advertising: libopenmpt extensions + MIDI, sorted/unique.
add_executable(test_supported_formats test_supported_formats.cpp)
target_link_libraries(test_supported_formats PRIVATE circus2bmson::circus2bmson)
add_test(NAME supported_formats COMMAND test_supported_formats)

# Sub-row precision: note-delay (EDx/SDx) places notes off the row grid.
add_executable(test_subrow_delay test_subrow_delay.cpp)
target_link_libraries(test_subrow_delay PRIVATE circus2bmson::circus2bmson)
target_compile_definitions(test_subrow_delay PRIVATE
  C2B_FIXTURE_DIR="${C2B_FIXTURE_DIR}")
add_test(NAME subrow_delay COMMAND test_subrow_delay)

# MIDI player engine: headless render(), levels, per-channel gain separation.
add_executable(test_midi_player test_midi_player.cpp)
target_link_libraries(test_midi_player PRIVATE circus2bmson::circus2bmson)
target_compile_definitions(test_midi_player PRIVATE
  C2B_FIXTURE_DIR="${C2B_FIXTURE_DIR}")
add_test(NAME midi_player COMMAND test_midi_player)

# Oracle: generic libopenmpt-driven score == independent MOD sequencer.
add_executable(test_score_vs_timeline test_score_vs_timeline.cpp)
target_link_libraries(test_score_vs_timeline PRIVATE circus2bmson::circus2bmson)
target_compile_definitions(test_score_vs_timeline PRIVATE
  C2B_FIXTURE_DIR="${C2B_FIXTURE_DIR}")
add_test(NAME score_vs_timeline COMMAND test_score_vs_timeline)
