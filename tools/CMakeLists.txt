# Diagnostic / de-risking tools.

# M0.5 spike: prove that rendering each pattern channel in isolation
# (all-but-one muted, via libopenmpt's interactive interface) and summing the
# stems reproduces the full mix. The whole keysound-extraction approach for
# M2 depends on this holding.
add_executable(stem_sum_check stem_sum_check.cpp)
target_link_libraries(stem_sum_check
  PRIVATE
    circus2bmson::circus2bmson
    PkgConfig::LIBOPENMPT)
