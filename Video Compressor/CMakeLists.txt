cmake_minimum_required(VERSION 3.20)
project(video-compressor C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0)

add_compile_options(-Wall -Wextra $<$<CONFIG:Debug>:-Werror>)

add_library(vc_core STATIC src/compress.c src/modes.c)
target_include_directories(vc_core PUBLIC src)
target_link_libraries(vc_core PUBLIC PkgConfig::GLIB m)

add_executable(video-compressor src/main.c)
target_link_libraries(video-compressor PRIVATE vc_core PkgConfig::GTK4)

enable_testing()
add_executable(test_compress tests/test_compress.c)
target_link_libraries(test_compress PRIVATE vc_core)
add_test(NAME test_compress COMMAND test_compress)
add_executable(test_modes tests/test_modes.c)
target_link_libraries(test_modes PRIVATE vc_core)
add_test(NAME test_modes COMMAND test_modes)

# The .desktop name must match the GApplication id: GNOME drops notifications from apps it can't
# find. For a user install: cmake -B build --install-prefix ~/.local && cmake --install build
include(GNUInstallDirs)
install(TARGETS video-compressor)
configure_file(data/io.github.dasantha.VideoCompressor.desktop.in
               io.github.dasantha.VideoCompressor.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/io.github.dasantha.VideoCompressor.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
