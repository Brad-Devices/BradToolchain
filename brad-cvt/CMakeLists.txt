# Brad-CVT reference front end.  The translator lowers PTX to a serialized
# .bvbc image; it uses the same encoder (BVEN) and container (bvbc_*) as
# bradc, so there is one instruction definition in the tree, not two.
add_library(brad-cvt STATIC
    bradcvt.c
    ptx_lex.c
    ptx_parse.c
    ptx_codegen.c
)

target_link_libraries(brad-cvt PUBLIC brad-vector)
target_include_directories(brad-cvt PUBLIC ../include)
target_include_directories(brad-cvt PRIVATE .)
