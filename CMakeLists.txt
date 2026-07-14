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

find_library(URING_LIB uring REQUIRED)
find_path(URING_INCLUDE_DIR liburing.h REQUIRED)

# --- BoringSSL Setup ------------------------------------------------------
# Explicitly target the paths provided to avoid picking up system OpenSSL
set(BORINGSSL_INCLUDE_DIR "/usr/local/include/boringssl")
find_library(BORINGSSL_SSL_LIB NAMES ssl PATHS "/usr/local/lib/boringssl" NO_DEFAULT_PATH REQUIRED)
find_library(BORINGSSL_CRYPTO_LIB NAMES crypto PATHS "/usr/local/lib/boringssl" NO_DEFAULT_PATH REQUIRED)

# --- libsrtp Setup --------------------------------------------------------
# Set libsrtp include path and locate the library
set(SRTP_INCLUDE_DIR "/usr/local/include/srtp3")
find_library(SRTP_LIB NAMES srtp2 srtp REQUIRED)

# --- core static lib: runtime + net + memory + util -------------------------
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
  ${BORINGSSL_INCLUDE_DIR}
  ${SRTP_INCLUDE_DIR}
)

# Link directly using resolved absolute library paths
target_link_libraries(sfu_core PUBLIC
  ${URING_LIB}
  ${BORINGSSL_SSL_LIB}
  ${BORINGSSL_CRYPTO_LIB}
  ${SRTP_LIB}
  pthread
  m
)

# --- main server binary -------------------------------------------------
add_executable(mezon-sfu src/runtime/main.c)
target_link_libraries(mezon-sfu PRIVATE sfu_core)
set_target_properties(mezon-sfu PROPERTIES LINKER_LANGUAGE CXX)

# --- tests / benchmarks ---------------------------------------------------
enable_testing()
add_subdirectory(tests)
add_subdirectory(benchmark)
