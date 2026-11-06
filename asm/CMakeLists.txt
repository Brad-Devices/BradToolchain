# The BradISA V2 assembler. Header-only dependency on src/include, so this
# builds before anything else and the test suite can link it.
add_library(brad-asm STATIC bradasm.c)
target_include_directories(brad-asm PUBLIC ${CMAKE_SOURCE_DIR}/include)
