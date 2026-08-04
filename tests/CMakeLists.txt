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

add_executable(test_json_lite test_json_lite.c)
target_link_libraries(test_json_lite PRIVATE sfu_core)
add_test(NAME json_lite COMMAND test_json_lite)

add_executable(test_sdp test_sdp.c)
target_link_libraries(test_sdp PRIVATE sfu_core)
add_test(NAME sdp COMMAND test_sdp)

add_executable(test_ws_handshake test_ws_handshake.c)
target_link_libraries(test_ws_handshake PRIVATE sfu_core)
add_test(NAME ws_handshake COMMAND test_ws_handshake)

add_executable(test_session_table test_session_table.c)
target_link_libraries(test_session_table PRIVATE sfu_core)
add_test(NAME session_table COMMAND test_session_table)

add_executable(test_netbytes test_netbytes.c)
target_link_libraries(test_netbytes PRIVATE sfu_core)
add_test(NAME netbytes COMMAND test_netbytes)

add_executable(test_metrics test_metrics.c)
target_link_libraries(test_metrics PRIVATE sfu_core)
add_test(NAME metrics COMMAND test_metrics)

add_executable(test_rtx_capacity test_rtx_capacity.c)
target_link_libraries(test_rtx_capacity PRIVATE sfu_core)
add_test(NAME rtx_capacity COMMAND test_rtx_capacity)

add_executable(test_epoch_reclaimer test_epoch_reclaimer.c)
target_link_libraries(test_epoch_reclaimer PRIVATE sfu_core)
add_test(NAME epoch_reclaimer COMMAND test_epoch_reclaimer)

add_executable(test_rtcp_compound test_rtcp_compound.c)
target_link_libraries(test_rtcp_compound PRIVATE sfu_core)
add_test(NAME rtcp_compound COMMAND test_rtcp_compound)

add_executable(test_rtp_packet test_rtp_packet.c)
target_link_libraries(test_rtp_packet PRIVATE sfu_core)
add_test(NAME rtp_packet COMMAND test_rtp_packet)

add_executable(test_rtcp_fb test_rtcp_fb.c)
target_link_libraries(test_rtcp_fb PRIVATE sfu_core)
add_test(NAME rtcp_fb COMMAND test_rtcp_fb)

add_executable(test_valkey_queue test_valkey_queue.c)
target_link_libraries(test_valkey_queue PRIVATE sfu_core)
add_test(NAME valkey_queue COMMAND test_valkey_queue)

add_executable(test_rtx_build test_rtx_build.c)
target_link_libraries(test_rtx_build PRIVATE sfu_core)
add_test(NAME rtx_build COMMAND test_rtx_build)

add_executable(test_nack_parser test_nack_parser.c)
target_link_libraries(test_nack_parser PRIVATE sfu_core)
add_test(NAME nack_parser COMMAND test_nack_parser)

add_executable(test_rtp_ext test_rtp_ext.c)
target_link_libraries(test_rtp_ext PRIVATE sfu_core)
add_test(NAME rtp_ext COMMAND test_rtp_ext)

add_executable(test_twcc_parser test_twcc_parser.c)
target_link_libraries(test_twcc_parser PRIVATE sfu_core)
add_test(NAME twcc_parser COMMAND test_twcc_parser)

add_executable(test_pacer test_pacer.c)
target_link_libraries(test_pacer PRIVATE sfu_core)
add_test(NAME pacer COMMAND test_pacer)

add_executable(test_gcc test_gcc.c)
target_link_libraries(test_gcc PRIVATE sfu_core)
add_test(NAME gcc COMMAND test_gcc)

add_executable(test_layer_selector test_layer_selector.c)
target_link_libraries(test_layer_selector PRIVATE sfu_core)
add_test(NAME layer_selector COMMAND test_layer_selector)

add_executable(test_worker_protocol test_worker_protocol.c)
target_link_libraries(test_worker_protocol PRIVATE sfu_core)
add_test(NAME worker_protocol COMMAND test_worker_protocol)

add_executable(test_vp9_parser test_vp9_parser.c)
target_link_libraries(test_vp9_parser PRIVATE sfu_core)
add_test(NAME vp9_parser COMMAND test_vp9_parser)

