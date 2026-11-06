# Six suites, exactly the ones that test the curated components.
#
# -UNDEBUG is not optional: this builds in Release, where CMake defines NDEBUG
# and turns assert() into a no-op. A test that only asserts then passes without
# testing anything. The libraries keep their own NDEBUG.

add_compile_options(-UNDEBUG)

add_executable(test_brad_as test_brad_as.c)
target_link_libraries(test_brad_as brad-asm)
add_test(NAME brad_as COMMAND test_brad_as)

# Compiles a C subset, assembles the emitted text, then executes the resulting
# machine code on the V2 emulator. This is the receipt that the compiler runs
# its own output, not just that it emits plausible-looking assembly.
add_executable(test_bradcmini test_bradcmini.c)
target_link_libraries(test_bradcmini brad-cmini brad-emu-v2)
add_test(NAME bradcmini COMMAND test_bradcmini)

add_executable(test_bradvector test_bradvector.c)
target_link_libraries(test_bradvector brad-vector m)
add_test(NAME bradvector COMMAND test_bradvector)

add_executable(test_bradlib test_bradlib.c)
target_link_libraries(test_bradlib brad-vector m)
add_test(NAME bradlib COMMAND test_bradlib)

add_executable(test_bradcvt test_bradcvt.c)
target_link_libraries(test_bradcvt brad-cvt m)
add_test(NAME bradcvt COMMAND test_bradcvt)

add_executable(test_core_emu test_core_emu.c)
target_link_libraries(test_core_emu brad-emu)
add_test(NAME core_emu COMMAND test_core_emu)
