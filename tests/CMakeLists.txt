add_executable(test_memory_pool test_memory_pool.c)
target_link_libraries(test_memory_pool PRIVATE sfu_core)
add_test(NAME memory_pool COMMAND test_memory_pool)

add_executable(test_ringbuffer test_ringbuffer.c)
target_link_libraries(test_ringbuffer PRIVATE sfu_core)
add_test(NAME ringbuffer COMMAND test_ringbuffer)

add_executable(test_packet_pool test_packet_pool.c)
target_link_libraries(test_packet_pool PRIVATE sfu_core)
add_test(NAME packet_pool COMMAND test_packet_pool)

add_executable(test_fanout_mesh test_fanout_mesh.c)
target_link_libraries(test_fanout_mesh PRIVATE sfu_core)
add_test(NAME fanout_mesh COMMAND test_fanout_mesh)

add_executable(test_room test_room.c)
target_link_libraries(test_room PRIVATE sfu_core)
add_test(NAME room COMMAND test_room)

add_executable(test_stun test_stun.c)
target_link_libraries(test_stun PRIVATE sfu_core)
add_test(NAME stun COMMAND test_stun)

add_executable(test_dtls_handshake test_dtls_handshake.c)
target_link_libraries(test_dtls_handshake PRIVATE sfu_core)
add_test(NAME dtls_handshake COMMAND test_dtls_handshake)

add_executable(test_srtp test_srtp.c)
target_link_libraries(test_srtp PRIVATE sfu_core)
add_test(NAME srtp COMMAND test_srtp)
