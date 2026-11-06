add_library(brad-vector STATIC
    bradc.c
    bvbc.c
    bvrt.c
    bradgdb.c
    bradtimeline.c
    bradlib.c
)

target_link_libraries(brad-vector PUBLIC brad-hw)
if(UNIX)
    target_link_libraries(brad-vector PUBLIC m)
endif()
target_include_directories(brad-vector PUBLIC ../include ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(brad-vector PRIVATE .)

# CLI tools
add_executable(bradc-cli tools/bradc-cli.c)
target_link_libraries(bradc-cli PRIVATE brad-vector)