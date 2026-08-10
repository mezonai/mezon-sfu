cmake_minimum_required(VERSION 3.16)
project(mezon-sfu C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SFU_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(SFU_ENABLE_TSAN "Build with ThreadSanitizer" OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(
  -Wall -Wextra -Wshadow -Wpointer-arith
  -Wno-unused-parameter
  -D_GNU_SOURCE
  -march=native
)

if(SFU_ENABLE_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

if(SFU_ENABLE_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -fno-pie)
  add_link_options(-fsanitize=thread -no-pie)
endif()

find_package(mimalloc REQUIRED)

find_path(URING_INCLUDE_DIR liburing.h REQUIRED)
find_library(URING_LIB uring REQUIRED)

# Explicitly target the paths provided to avoid picking up system OpenSSL
set(BORINGSSL_INCLUDE_DIR "/usr/local/include/boringssl")
find_library(BORINGSSL_SSL_LIB NAMES ssl PATHS "/usr/local/lib/boringssl" NO_DEFAULT_PATH REQUIRED)
find_library(BORINGSSL_CRYPTO_LIB NAMES crypto PATHS "/usr/local/lib/boringssl" NO_DEFAULT_PATH REQUIRED)

# Set libsrtp include path and locate the library
set(SRTP_INCLUDE_DIR "/usr/local/include/srtp2")
find_library(SRTP2_LIBRARY NAMES srtp2 PATHS "/usr/local/lib" NO_DEFAULT_PAT REQUIRED)

# Set libuv headers and library installed by libuv1-dev
find_path(LIBUV_INCLUDE_DIR NAMES uv.h)
find_library(LIBUV_LIBRARY NAMES uv)

if(NOT LIBUV_INCLUDE_DIR OR NOT LIBUV_LIBRARY)
  message(FATAL_ERROR "libuv1-dev not found! Install it using 'sudo apt install libuv1-dev'")
endif()

# Set nats library
find_path(NATS_INCLUDE_DIR NAMES nats/nats.h)
find_library(NATS_LIBRARY NAMES nats)

add_library(sfu_core STATIC
  src/media/svc/svc_parser.c
  src/media/svc/svc_descriptor.c
  src/memory/pool.c
  src/memory/packet_pool.c
  src/memory/refcount.c
  src/util/ringbuffer.c
  src/util/log.c
  src/util/metrics.c
  src/api/hook/producer.c
  src/net/socket.c
  src/net/io_uring.c
  src/net/batch.c
  src/net/zerocopy.c
  src/transport/stun/stun.c
  src/transport/dtls/dtls.c
  src/transport/srtp/srtp.c
  src/peer/session.c
  src/room/room.c
  src/room/room_media_graph.c
  src/room/room_registry.c
  src/protocol/websocket/ws.c
  src/protocol/signaling/json_lite.c
  src/protocol/signaling/sdp.c
  src/protocol/signaling/signaling.c
  src/pipeline/dispatch.c
  src/pipeline/ingress.c
  src/pipeline/router.c
  src/pipeline/egress.c
  src/pipeline/keyframe.c
  src/runtime/cpu.c
  src/runtime/fanout_job.c
  src/runtime/timer.c
  src/runtime/fanout.c
  src/runtime/worker.c
  src/runtime/epoch_reclaimer.c
  src/runtime/scheduler.c
  src/runtime/signal.c
  src/runtime/routing_context.c
  src/congestion/gcc.c
  src/congestion/pacer.c
  src/congestion/twcc_feedback.c
  src/congestion/twcc_parser.c
  src/rtp/rtp_packet.c
  src/rtp/rtp_ext.c
  src/rtp/rtx.c
  src/rtp/rtx_build.c
  src/rtcp/rtcp_kf.c
  src/rtcp/rtcp_compound.c
  src/rtcp/rtcp_fb.c
  src/config/config.c
)

target_include_directories(sfu_core PUBLIC
  include
  src
  ${URING_INCLUDE_DIR}
  ${BORINGSSL_INCLUDE_DIR}
  ${SRTP_INCLUDE_DIR}
  ${LIBUV_INCLUDE_DIR}
  ${NATS_INCLUDE_DIR}
)

# Link directly using resolved absolute library paths
target_link_libraries(sfu_core PUBLIC
  ${URING_LIB}
  ${BORINGSSL_SSL_LIB}
  ${BORINGSSL_CRYPTO_LIB}
  ${SRTP2_LIBRARY}
  ${LIBUV_LIBRARY}
  ${NATS_LIBRARY}
  mimalloc
  pthread
  m
  stdc++
)
set_target_properties(sfu_core PROPERTIES LINKER_LANGUAGE CXX)

add_executable(mezon-sfu src/runtime/main.c)
target_link_libraries(mezon-sfu PRIVATE sfu_core mimalloc)
set_target_properties(mezon-sfu PROPERTIES LINKER_LANGUAGE CXX)

target_compile_options(mezon-sfu PRIVATE
    $<$<CONFIG:Debug>:-g3>
    $<$<CONFIG:Release>:-O2>
)

enable_testing()
add_subdirectory(tests)
add_subdirectory(benchmark)
