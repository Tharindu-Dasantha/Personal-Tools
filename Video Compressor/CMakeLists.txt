cmake_minimum_required(VERSION 3.20)
project(video-compressor C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0)

# The installed app is a release build (cmake --install --strip); Debug is the test build (docs/HANDOFF.md).
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra $<$<CONFIG:Debug>:-Werror>
                    -fstack-protector-strong $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=3>)

add_library(vc_core STATIC src/compress.c src/modes.c src/web_video.c src/frames.c src/images.c)
target_include_directories(vc_core PUBLIC src)
target_link_libraries(vc_core PUBLIC PkgConfig::GLIB m)

# The UI's own icons (globe, bolt, …), which Adwaita lacks, and the Inter font, built into the binary so
# the app runs offline on any machine. GtkApplication finds the icons by name; load_fonts() loads the font.
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)
file(GLOB VC_ICONS ${CMAKE_CURRENT_SOURCE_DIR}/data/icons/scalable/actions/*.svg ${CMAKE_CURRENT_SOURCE_DIR}/data/fonts/*)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/resources.c
  COMMAND ${GLIB_COMPILE_RESOURCES} --generate-source --target=${CMAKE_CURRENT_BINARY_DIR}/resources.c
          --sourcedir=${CMAKE_CURRENT_SOURCE_DIR}/data ${CMAKE_CURRENT_SOURCE_DIR}/data/resources.gresource.xml
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/data/resources.gresource.xml ${VC_ICONS})
# The GTK app: one file per area, sharing src/app.h.
add_executable(video-compressor src/main.c src/common.c src/row.c src/status.c src/source.c src/jobs.c
               src/handlers.c src/tools.c src/style.c src/widgets.c src/inspector.c
               ${CMAKE_CURRENT_BINARY_DIR}/resources.c)
target_link_libraries(video-compressor PRIVATE vc_core PkgConfig::GTK4)

enable_testing()
add_executable(test_compress tests/test_compress.c)
target_link_libraries(test_compress PRIVATE vc_core)
add_test(NAME test_compress COMMAND test_compress)
add_executable(test_modes tests/test_modes.c)
target_link_libraries(test_modes PRIVATE vc_core)
add_test(NAME test_modes COMMAND test_modes)
add_executable(test_web_video tests/test_web_video.c)
target_link_libraries(test_web_video PRIVATE vc_core)
add_test(NAME test_web_video COMMAND test_web_video)
add_executable(test_frames tests/test_frames.c)
target_link_libraries(test_frames PRIVATE vc_core)
add_test(NAME test_frames COMMAND test_frames)
add_executable(test_images tests/test_images.c)
target_link_libraries(test_images PRIVATE vc_core)
add_test(NAME test_images COMMAND test_images)

# The .desktop name must match the GApplication id: GNOME drops notifications from apps it can't
# find. For a user install: cmake -B build --install-prefix ~/.local && cmake --install build
include(GNUInstallDirs)
install(TARGETS video-compressor)
set(APP_ID io.github.Tharindu_Dasantha.MediaTools)
install(FILES data/${APP_ID}.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/${APP_ID}.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES data/icons/hicolor/scalable/apps/${APP_ID}.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES ../LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/${APP_ID})
