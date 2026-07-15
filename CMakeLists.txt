cmake_minimum_required(VERSION 3.16)
project(mezon-sfu C)

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

find_library(URING_LIB uring REQUIRED)
find_path(URING_INCLUDE_DIR liburing.h REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SRTP2 REQUIRED libsrtp2)

find_library(MIMALLOC_LIB mimalloc REQUIRED)

option(SFU_USE_BORINGSSL "Link against a local BoringSSL build instead of system OpenSSL" OFF)
set(SFU_BORINGSSL_ROOT "" CACHE PATH
    "Root of a built BoringSSL tree: <root>/include/openssl/*.h and <root>/build/lib{ssl,crypto}.a. See third_party/README.md.")

if(SFU_USE_BORINGSSL)
  if(NOT SFU_BORINGSSL_ROOT)
    message(FATAL_ERROR "SFU_USE_BORINGSSL=ON requires -DSFU_BORINGSSL_ROOT=/path/to/boringssl (see third_party/README.md)")
  endif()

  add_library(boringssl_ssl STATIC IMPORTED)
  set_target_properties(boringssl_ssl PROPERTIES
    IMPORTED_LOCATION "${SFU_BORINGSSL_ROOT}/build/libssl.a")
  add_library(boringssl_crypto STATIC IMPORTED)
  set_target_properties(boringssl_crypto PROPERTIES
    IMPORTED_LOCATION "${SFU_BORINGSSL_ROOT}/build/libcrypto.a")

  set(SFU_SSL_INCLUDE_DIRS "${SFU_BORINGSSL_ROOT}/include")
  # BoringSSL is compiled as C++ internally; the final link needs the
  # C++ runtime even though every call site in this codebase is plain C.
  set(SFU_SSL_LIBS boringssl_ssl boringssl_crypto stdc++)
  message(STATUS "Using BoringSSL from ${SFU_BORINGSSL_ROOT}")
else()
  find_package(OpenSSL REQUIRED)
  set(SFU_SSL_INCLUDE_DIRS "")
  set(SFU_SSL_LIBS OpenSSL::SSL OpenSSL::Crypto)
endif()

add_library(sfu_core STATIC
  src/memory/pool.c
  src/memory/packet_pool.c
  src/memory/refcount.c
  src/util/ringbuffer.c
  src/util/log.c
  src/net/socket.c
  src/net/io_uring.c
  src/net/batch.c
  src/net/zerocopy.c
  src/transport/stun/stun.c
  src/transport/dtls/dtls.c
  src/transport/srtp/srtp.c
  src/peer/session.c
  src/room/room.c
  src/protocol/websocket/ws.c
  src/protocol/signaling/json_lite.c
  src/protocol/signaling/sdp.c
  src/protocol/signaling/signaling.c
  src/pipeline/dispatch.c
  src/runtime/cpu.c
  src/runtime/timer.c
  src/runtime/fanout.c
  src/runtime/worker.c
  src/runtime/scheduler.c
  src/runtime/signal.c
)

target_include_directories(sfu_core PUBLIC
  include
  src
  ${URING_INCLUDE_DIR}
  ${SRTP2_INCLUDE_DIRS}
  ${SFU_SSL_INCLUDE_DIRS}
)

target_link_libraries(sfu_core PUBLIC
  ${URING_LIB}
  ${SFU_SSL_LIBS}
  ${SRTP2_LIBRARIES}
  ${MIMALLOC_LIB}
  pthread
  m
)
target_link_directories(sfu_core PUBLIC ${SRTP2_LIBRARY_DIRS})

add_executable(mezon-sfu src/runtime/main.c)
target_link_libraries(mezon-sfu PRIVATE sfu_core)

enable_testing()
add_subdirectory(tests)
add_subdirectory(benchmark)
