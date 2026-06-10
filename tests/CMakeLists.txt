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
