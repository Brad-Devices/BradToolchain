add_library(brad-emu STATIC
    brad_core_emu.c
    brad_cache_emu.c
)

target_link_libraries(brad-emu PUBLIC brad-hw)
target_include_directories(brad-emu PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)

# V2 single-cycle emulator: used by bradc-mini for execution verification and
# by any V2 code that needs an in-process runner.  V1 emulator stays separate.
add_library(brad-emu-v2 STATIC brad_core_v2_emu.c)
target_link_libraries(brad-emu-v2 PUBLIC brad-hw)
target_include_directories(brad-emu-v2 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
