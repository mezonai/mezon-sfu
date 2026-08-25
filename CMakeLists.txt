cmake_minimum_required(VERSION 3.16)
project(mezon-sfu C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SFU_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(SFU_ENABLE_TSAN "Build with ThreadSanitizer" OFF)
option(SFU_DIAG_LOG "Compile in diagnostic logging for the SRTP key-desync investigation (ON = verbose drops/dumps, OFF = silent)" ON)
option(USE_AF_XDP "Use AF_XDP instead of io_uring for media I/O" ON)
option(SFU_ENABLE_PRIVILEGED_TESTS "Register privileged network integration tests" OFF)

if(SFU_DIAG_LOG)
  add_compile_definitions(SFU_DIAG_LOG=1)
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(
  -Wall -Wextra -Wshadow -Wpointer-arith
  -Wno-unused-parameter
  -D_GNU_SOURCE
  -march=native
  $<$<AND:$<CONFIG:Release>,$<COMPILE_LANG_AND_ID:C,GNU,Clang>>:-O3>
  $<$<AND:$<CONFIG:Release>,$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>>:-O3>
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

set(SFU_NET_SOURCES)
set(SFU_NET_INCLUDE_DIRS)
set(SFU_NET_LIBRARIES)

if(USE_AF_XDP)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBXDP REQUIRED IMPORTED_TARGET libxdp)
  pkg_check_modules(LIBBPF REQUIRED IMPORTED_TARGET libbpf)
  find_program(CLANG_EXECUTABLE NAMES clang REQUIRED)

  set(SFU_XDP_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/sfu_xdp_kern.o")
  add_custom_command(
    OUTPUT "${SFU_XDP_OBJECT}"
    COMMAND "${CLANG_EXECUTABLE}" -O2 -g -target bpf -D__TARGET_ARCH_x86
            -I/usr/include/${CMAKE_LIBRARY_ARCHITECTURE}
            -c "${CMAKE_CURRENT_SOURCE_DIR}/src/net/xdp/sfu_xdp_kern.c"
            -o "${SFU_XDP_OBJECT}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/net/xdp/sfu_xdp_kern.c"
    VERBATIM
  )
  add_custom_target(sfu_xdp_program DEPENDS "${SFU_XDP_OBJECT}")
  add_compile_definitions(USE_AF_XDP=1 SFU_XDP_OBJECT_PATH="${SFU_XDP_OBJECT}")
  list(APPEND SFU_NET_SOURCES src/net/af_xdp.c src/net/af_xdp_frame.c)
  list(APPEND SFU_NET_LIBRARIES PkgConfig::LIBXDP PkgConfig::LIBBPF)
else()
  find_path(URING_INCLUDE_DIR liburing.h REQUIRED)
  find_library(URING_LIB uring REQUIRED)
  list(APPEND SFU_NET_SOURCES src/net/io_uring.c src/net/batch.c)
  list(APPEND SFU_NET_INCLUDE_DIRS ${URING_INCLUDE_DIR})
  list(APPEND SFU_NET_LIBRARIES ${URING_LIB})
endif()

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
  src/media/svc/layer_scheduler.c
  src/memory/pool.c
  src/memory/packet_pool.c
  src/memory/worker_packet_arena.c
  src/memory/refcount.c
  src/util/ringbuffer.c
  src/util/log.c
  src/util/metrics.c
  src/api/hook/producer.c
  src/net/socket.c
  ${SFU_NET_SOURCES}
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
  src/protocol/signaling/handshake.c
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
  src/congestion/bandwidth_allocator.c
  src/congestion/gcc.c
  src/congestion/pacer.c
  src/congestion/twcc_feedback.c
  src/congestion/twcc_parser.c
  src/rtp/rtp_packet.c
  src/rtp/rtp_seq_translate.c
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
  ${SFU_NET_INCLUDE_DIRS}
  ${BORINGSSL_INCLUDE_DIR}
  ${SRTP_INCLUDE_DIR}
  ${LIBUV_INCLUDE_DIR}
  ${NATS_INCLUDE_DIR}
)

# Link directly using resolved absolute library paths
target_link_libraries(sfu_core PUBLIC
  ${SFU_NET_LIBRARIES}
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
if(USE_AF_XDP)
  add_dependencies(sfu_core sfu_xdp_program)
endif()

add_executable(mezon-sfu src/runtime/main.c)
target_link_libraries(mezon-sfu PRIVATE sfu_core mimalloc)
set_target_properties(mezon-sfu PROPERTIES LINKER_LANGUAGE CXX ENABLE_EXPORTS ON)
target_link_options(mezon-sfu PRIVATE -rdynamic)

target_compile_options(mezon-sfu PRIVATE
    $<$<CONFIG:Debug>:-g3>
)

enable_testing()
add_subdirectory(tests)
add_subdirectory(benchmark)
