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
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SRTP2 REQUIRED libsrtp2)

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
  ${SRTP2_INCLUDE_DIRS}
)

target_link_libraries(sfu_core PUBLIC ${URING_LIB} OpenSSL::SSL OpenSSL::Crypto ${SRTP2_LIBRARIES} pthread m)
target_link_directories(sfu_core PUBLIC ${SRTP2_LIBRARY_DIRS})

# --- main server binary -------------------------------------------------
add_executable(mezon-sfu src/runtime/main.c)
target_link_libraries(mezon-sfu PRIVATE sfu_core)

# --- tests / benchmarks ---------------------------------------------------
enable_testing()
add_subdirectory(tests)
add_subdirectory(benchmark)
