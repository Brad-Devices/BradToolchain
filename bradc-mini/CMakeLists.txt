# bradc-mini: a TCC-class C-subset compiler for BradISA V2.
#
# Not the general compiler -- that is the LLVM backend (Layer 1).  This is
# Layer 2: a small, self-contained compiler for kernel bring-up and the
# in-browser compile receipt.  It targets BradISA V2 and emits assembly text
# that src/asm/bradasm.c assembles to machine code.
add_library(brad-cmini STATIC bradcmini.c)
target_link_libraries(brad-cmini PUBLIC brad-asm)
target_include_directories(brad-cmini PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/asm)
