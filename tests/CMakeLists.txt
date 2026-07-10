add_executable(test_memory_pool test_memory_pool.c)
target_link_libraries(test_memory_pool PRIVATE sfu_core)
add_test(NAME memory_pool COMMAND test_memory_pool)

add_executable(test_ringbuffer test_ringbuffer.c)
target_link_libraries(test_ringbuffer PRIVATE sfu_core)
add_test(NAME ringbuffer COMMAND test_ringbuffer)

add_executable(test_packet_pool test_packet_pool.c)
target_link_libraries(test_packet_pool PRIVATE sfu_core)
add_test(NAME packet_pool COMMAND test_packet_pool)
