add_executable(udp_flood udp_flood.c)
target_link_libraries(udp_flood PRIVATE mimalloc pthread)

add_executable(bench_sfu_core bench_sfu_core.c)
target_link_libraries(bench_sfu_core PRIVATE sfu_core)

add_test(NAME bench_sfu_core_smoke COMMAND bench_sfu_core --quick)
set_tests_properties(bench_sfu_core_smoke PROPERTIES LABELS benchmark)

add_test(NAME bench_sfu_core_media_fanout_smoke COMMAND bench_sfu_core media_fanout --quick --workers 4 --subscribers 3)
set_tests_properties(bench_sfu_core_media_fanout_smoke PROPERTIES LABELS benchmark)
