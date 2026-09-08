# PocketOS shell.
#   Host simulator : -DPOCKETOS_DISPLAY=sdl -DPOCKETOS_LVGL_MODE=source (defaults)
#   K230 (Buildroot): -DPOCKETOS_DISPLAY=drm -DPOCKETOS_LVGL_MODE=sysroot
# In sysroot mode LVGL, its lv_conf.h, libdrm and libevdev come from the
# Buildroot staging tree (the vendor LVGL package), so the shell shares one
# LVGL build with the rest of the image.
cmake_minimum_required(VERSION 3.16)
project(pocketos_shell C)

set(POCKETOS_DISPLAY "sdl" CACHE STRING "Display backend: sdl or drm")
set(POCKETOS_LVGL_MODE "source" CACHE STRING "LVGL from: source (add_subdirectory) or sysroot")
set(LVGL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/lvgl" CACHE PATH "LVGL source tree (pinned commit)")
set(REPO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")
file(GLOB POCKETUI_FONTS "${REPO_DIR}/ui/pocketui/fonts/pos_font_*.c")

# Build identity, matching the root Makefile: BUILD_ID beside VERSION when the
# tree was exported into the Buildroot package, git in a working checkout,
# "unknown" when there is neither. The shell carried no version at all before.
file(STRINGS "${REPO_DIR}/VERSION" POCKETOS_VERSION LIMIT_COUNT 1)
if(EXISTS "${REPO_DIR}/BUILD_ID")
    file(STRINGS "${REPO_DIR}/BUILD_ID" POCKETOS_BUILD_ID LIMIT_COUNT 1)
else()
    execute_process(COMMAND git -C "${REPO_DIR}" rev-parse --short HEAD
                    OUTPUT_VARIABLE POCKETOS_BUILD_ID
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET RESULT_VARIABLE POCKETOS_GIT_RC)
    if(NOT POCKETOS_GIT_RC EQUAL 0 OR POCKETOS_BUILD_ID STREQUAL "")
        set(POCKETOS_BUILD_ID "unknown")
    endif()
endif()
message(STATUS "PocketOS ${POCKETOS_VERSION} build ${POCKETOS_BUILD_ID}")

find_package(PkgConfig REQUIRED)

add_executable(pocketos-shell
    shell.c
    shell_ipc.c
    settings.c
    platform_${POCKETOS_DISPLAY}.c
    ../pocketui/pocketui.c
    ../pocketui/pos_theme.c
    ../pocketui/pos_styles.c
    ${REPO_DIR}/apps/radio/radio_app.c
    ${REPO_DIR}/apps/system/system_app.c
    ${REPO_DIR}/apps/fleet/fleet_app.c
    ${REPO_DIR}/apps/fleet/engine/fleet_types.c
    ${REPO_DIR}/apps/fleet/engine/fleet_rng.c
    ${REPO_DIR}/apps/fleet/engine/fleet_ai.c
    ${REPO_DIR}/apps/fleet/engine/fleet_rules.c
    ${REPO_DIR}/apps/fleet/engine/fleet_save.c
    ${REPO_DIR}/apps/fleet/engine/fleet_store.c
    ${REPO_DIR}/apps/fleet/ui/fleet_view.c
    ${REPO_DIR}/apps/fleet/ui/fleet_widgets.c
    ${REPO_DIR}/apps/fleet/ui/fleet_grid.c
    ${REPO_DIR}/apps/fleet/ui/fleet_screen_command.c
    ${REPO_DIR}/apps/fleet/ui/fleet_screen_deploy.c
    ${REPO_DIR}/apps/fleet/ui/fleet_screen_battle.c
    ${REPO_DIR}/apps/fleet/ui/fleet_screen_result.c
    ${REPO_DIR}/apps/radar/radar_app.c
    ${REPO_DIR}/apps/radar/radar_store.c
    ${REPO_DIR}/apps/radar/engine/radar_types.c
    ${REPO_DIR}/apps/radar/engine/radar_rng.c
    ${REPO_DIR}/apps/radar/engine/radar_rules.c
    ${REPO_DIR}/apps/radar/engine/radar_score.c
    ${REPO_DIR}/apps/radar/ui/radar_scope.c
    ${REPO_DIR}/apps/radar/ui/radar_screens.c
    ${REPO_DIR}/core/pocketpaths.c
    ${REPO_DIR}/apps/timber/timber_app.c
    ${REPO_DIR}/apps/timber/engine/timber_types.c
    ${REPO_DIR}/apps/timber/engine/timber_rng.c
    ${REPO_DIR}/apps/timber/engine/timber_tower.c
    ${REPO_DIR}/apps/timber/engine/timber_pull.c
    ${REPO_DIR}/apps/timber/engine/timber_stability.c
    ${REPO_DIR}/apps/timber/engine/timber_score.c
    ${REPO_DIR}/apps/timber/engine/timber_collapse.c
    ${REPO_DIR}/apps/timber/engine/timber_rules.c
    ${REPO_DIR}/apps/timber/engine/timber_replay.c
    ${REPO_DIR}/apps/timber/ui/timber_view.c
    ${REPO_DIR}/apps/timber/ui/timber_table.c
    ${REPO_DIR}/apps/timber/ui/timber_screens.c
    ${REPO_DIR}/core/pocketipc/pocketipc.c
    ${REPO_DIR}/core/pocketipc/server.c
    ${REPO_DIR}/core/pocketlog/pocketlog.c
    ${POCKETUI_FONTS})
target_include_directories(pocketos-shell PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../pocketui" "${REPO_DIR}/core")
target_compile_options(pocketos-shell PRIVATE -Wall -Wextra)
target_compile_definitions(pocketos-shell PRIVATE
    POCKETOS_DISPLAY_NAME="${POCKETOS_DISPLAY}"
    POCKETOS_VERSION="${POCKETOS_VERSION}"
    POCKETOS_BUILD_ID="${POCKETOS_BUILD_ID}"
    LV_LVGL_H_INCLUDE_SIMPLE)
target_link_libraries(pocketos-shell PRIVATE cjson m)

if(POCKETOS_LVGL_MODE STREQUAL "source")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.defaults" LV_DEFAULTS_BASE)
    if(POCKETOS_DISPLAY STREQUAL "sdl")
        set(LV_DEFAULTS_EXTRA "LV_USE_SDL 1\nLV_USE_LODEPNG 1\nLV_USE_SNAPSHOT 1\n")
    elseif(POCKETOS_DISPLAY STREQUAL "drm")
        set(LV_DEFAULTS_EXTRA "LV_USE_LINUX_DRM 1\nLV_USE_EVDEV 1\nLV_USE_LODEPNG 1\nLV_USE_SNAPSHOT 1\n")
    else()
        message(FATAL_ERROR "POCKETOS_DISPLAY must be sdl or drm")
    endif()
    file(WRITE "${CMAKE_BINARY_DIR}/lv_conf.defaults" "${LV_DEFAULTS_BASE}\n${LV_DEFAULTS_EXTRA}")
    execute_process(
        COMMAND ${Python3_EXECUTABLE} "${LVGL_DIR}/scripts/generate_lv_conf.py"
                --template "${LVGL_DIR}/lv_conf_template.h"
                --defaults "${CMAKE_BINARY_DIR}/lv_conf.defaults"
                --config "${CMAKE_BINARY_DIR}/lv_conf.h"
        RESULT_VARIABLE conf_rc OUTPUT_VARIABLE conf_out ERROR_VARIABLE conf_out)
    if(NOT conf_rc EQUAL 0)
        message(FATAL_ERROR "lv_conf.h generation failed: ${conf_out}")
    endif()
    set(LV_BUILD_CONF_PATH "${CMAKE_BINARY_DIR}/lv_conf.h" CACHE PATH "" FORCE)
    set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
    set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
    add_subdirectory("${LVGL_DIR}" lvgl EXCLUDE_FROM_ALL)
    target_include_directories(pocketos-shell PRIVATE "${LVGL_DIR}")
    target_compile_definitions(pocketos-shell PRIVATE LV_CONF_PATH="${CMAKE_BINARY_DIR}/lv_conf.h")
    target_link_libraries(pocketos-shell PRIVATE lvgl)
    set(LVGL_INC_TARGET lvgl)
elseif(POCKETOS_LVGL_MODE STREQUAL "sysroot")
    if(NOT CMAKE_SYSROOT)
        message(FATAL_ERROR "sysroot mode needs a toolchain file with CMAKE_SYSROOT (Buildroot)")
    endif()
    set(LVGL_SYSROOT_INC "${CMAKE_SYSROOT}/usr/include/lvgl")
    if(NOT EXISTS "${LVGL_SYSROOT_INC}/lv_conf.h")
        message(FATAL_ERROR "no LVGL in sysroot: ${LVGL_SYSROOT_INC}/lv_conf.h missing (build the lvgl package first)")
    endif()
    target_include_directories(pocketos-shell PRIVATE "${LVGL_SYSROOT_INC}" "${CMAKE_SYSROOT}/usr/include")
    target_compile_definitions(pocketos-shell PRIVATE LV_CONF_PATH="${LVGL_SYSROOT_INC}/lv_conf.h")
    target_link_directories(pocketos-shell PRIVATE "${CMAKE_SYSROOT}/usr/lib")
    target_link_libraries(pocketos-shell PRIVATE lvgl pthread)
    set(LVGL_INC_TARGET pocketos-shell)
else()
    message(FATAL_ERROR "POCKETOS_LVGL_MODE must be source or sysroot")
endif()

if(POCKETOS_DISPLAY STREQUAL "sdl")
    pkg_check_modules(SDL2 REQUIRED sdl2)
    target_include_directories(${LVGL_INC_TARGET} PUBLIC ${SDL2_INCLUDE_DIRS})
    target_link_libraries(pocketos-shell PRIVATE ${SDL2_LIBRARIES})
else()
    pkg_check_modules(DRM REQUIRED libdrm)
    pkg_check_modules(EVDEV REQUIRED libevdev)
    target_include_directories(${LVGL_INC_TARGET} PUBLIC ${DRM_INCLUDE_DIRS} ${EVDEV_INCLUDE_DIRS})
    target_link_libraries(pocketos-shell PRIVATE ${DRM_LIBRARIES} ${EVDEV_LIBRARIES})
    # The vendor LVGL package patches a plane-rotation call into the DRM
    # driver (LILYGO patch 0002). platform_drm.c uses it for the
    # POCKETOS_DRM_ROTATION bench override only when the header has it.
    if(POCKETOS_LVGL_MODE STREQUAL "sysroot")
        set(DRM_HEADER "${LVGL_SYSROOT_INC}/src/drivers/display/drm/lv_linux_drm.h")
    else()
        set(DRM_HEADER "${LVGL_DIR}/src/drivers/display/drm/lv_linux_drm.h")
    endif()
    file(STRINGS "${DRM_HEADER}" DRM_ROTATION_LINES REGEX "lv_linux_drm_set_rotation")
    if(DRM_ROTATION_LINES)
        target_compile_definitions(pocketos-shell PRIVATE POCKETOS_DRM_ROTATION_API)
        message(STATUS "LVGL DRM rotation API available (vendor patch)")
    else()
        message(STATUS "LVGL DRM rotation API not available; POCKETOS_DRM_ROTATION will be ignored")
    endif()
endif()

install(TARGETS pocketos-shell RUNTIME DESTINATION bin)

# Host-only LVGL-linked tests (run by tests/radar_shell_test.sh). The
# target build does not need them.
if(POCKETOS_DISPLAY STREQUAL "sdl")
    add_executable(radar_scope_test
        ${REPO_DIR}/tests/radar_scope_test.c
        ${REPO_DIR}/apps/radar/ui/radar_scope.c
        ${REPO_DIR}/apps/radar/engine/radar_types.c
        ${REPO_DIR}/apps/radar/engine/radar_rng.c
        ${REPO_DIR}/apps/radar/engine/radar_rules.c
        ${REPO_DIR}/apps/radar/engine/radar_score.c
        ../pocketui/pocketui.c
        ../pocketui/pos_theme.c
        ../pocketui/pos_styles.c
        ${POCKETUI_FONTS})
    target_include_directories(radar_scope_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../pocketui"
        "${REPO_DIR}/core" "${REPO_DIR}/apps/radar/ui")
    target_compile_options(radar_scope_test PRIVATE -Wall -Wextra)
    target_compile_definitions(radar_scope_test PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
    if(POCKETOS_LVGL_MODE STREQUAL "source")
        target_include_directories(radar_scope_test PRIVATE "${LVGL_DIR}")
        target_compile_definitions(radar_scope_test PRIVATE LV_CONF_PATH="${CMAKE_BINARY_DIR}/lv_conf.h")
    endif()
    target_link_libraries(radar_scope_test PRIVATE lvgl ${SDL2_LIBRARIES} m)
endif()
