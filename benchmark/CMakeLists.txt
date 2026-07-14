add_executable(udp_flood udp_flood.c)
target_link_libraries(udp_flood PRIVATE mimalloc pthread)
