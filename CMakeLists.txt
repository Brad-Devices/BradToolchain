cmake_minimum_required(VERSION 3.28)
project(brad-toolchain VERSION 0.1.0 LANGUAGES C)

# Curated BradISA V2 toolchain. The working repository's root CMakeLists also
# builds the driver and BVLib targets; this one does not, because the driver
# layer contains the deep moat. See README.md and ci/boundary.sh.

set(CMAKE_C_STANDARD 23)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

enable_testing()

add_library(brad-hw INTERFACE)
target_include_directories(brad-hw INTERFACE include)

# bradc-mini and Brad-CVT both lower through the assembler / BradVector encoder,
# so the order matters: asm -> bradc-mini, bradvector -> brad-cvt.
add_subdirectory(asm)
add_subdirectory(bradc-mini)
add_subdirectory(bradvector)
add_subdirectory(brad-cvt)
add_subdirectory(emu)
add_subdirectory(tests)
