add_library(bar INTERFACE IMPORTED)

add_library(foo INTERFACE)
target_link_libraries(foo INTERFACE bar)

install(TARGETS foo EXPORT foo DESTINATION .)
export(EXPORT foo PACKAGE_INFO foo)
