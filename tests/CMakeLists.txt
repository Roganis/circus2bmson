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
