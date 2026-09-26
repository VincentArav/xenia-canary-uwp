add_library(xenia-xefu STATIC)
xe_platform_sources(xenia-xefu ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xenia-xefu PUBLIC sha256 xenia-base xenia-vfs)
xe_target_defaults(xenia-xefu)
