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
message(STATUS "Doors ${POCKETOS_VERSION} build ${POCKETOS_BUILD_ID}")

find_package(PkgConfig REQUIRED)

add_executable(pocketos-shell
    shell.c
    shell_ipc.c
    shell_kb_state.c
    settings.c
    brightness.c
    platform_${POCKETOS_DISPLAY}.c
    ../pocketui/pocketui.c
    ../pocketui/pos_display.c
    shell_display.c
    chrome.c
    orientation.c
    kbd_presence.c
    art.c
    art_format.c
    home.c
    home_layout.c
    shell_lock.c
    controls.c
    ../pocketui/pos_glyphs.c
    ../pocketui/pos_input.c
    ../pocketui/pos_keyboard.c
    ../pocketui/pos_keymap.c
    kbd_tca8418.c
    shell_kbd.c
    ../pocketui/pos_theme.c
    ../pocketui/pos_styles.c
    ../pocketui/pos_brand_mark.c
    ../pocketui/pos_app_icons.c
    ${REPO_DIR}/apps/radio/radio_app.c
    ${REPO_DIR}/apps/system/system_app.c
    ${REPO_DIR}/apps/system/system_view.c
    ${REPO_DIR}/apps/settings/settings_app.c
    ${REPO_DIR}/apps/settings/settings_view.c
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
    ${REPO_DIR}/apps/clock/clock_app.c
    ${REPO_DIR}/apps/clock/clock_engine.c
    ${REPO_DIR}/apps/clock/clock_store.c
    ${REPO_DIR}/apps/clock/clock_time.c
    ${REPO_DIR}/apps/clock/clock_alert.c
    ${REPO_DIR}/apps/clock/clock_runtime.c
    shell_alarm.c
    ${REPO_DIR}/apps/calendar/cal_app.c
    ${REPO_DIR}/apps/calendar/cal_date.c
    ${REPO_DIR}/apps/calendar/cal_view.c
    ${REPO_DIR}/apps/calculator/calc_app.c
    ${REPO_DIR}/apps/calculator/calc_engine.c
    ${REPO_DIR}/apps/calculator/calc_view.c
    ${REPO_DIR}/apps/notes/notes_app.c
    ${REPO_DIR}/apps/notes/notes_store.c
    ${REPO_DIR}/apps/notes/notes_view.c
    ${REPO_DIR}/apps/files/files_app.c
    ${REPO_DIR}/apps/files/files_fs.c
    ${REPO_DIR}/apps/files/files_job.c
    ${REPO_DIR}/apps/files/files_view.c
    ${REPO_DIR}/apps/wave/wave_app.c
    ${REPO_DIR}/apps/wave/wave_view.c
    ${REPO_DIR}/apps/wave/wave_session.c
    ${REPO_DIR}/apps/wave/wave_text.c
    ${REPO_DIR}/apps/camera/camera_app.c
    ${REPO_DIR}/apps/camera/camera_state.c
    ${REPO_DIR}/apps/camera/camera_layout.c
    ${REPO_DIR}/apps/camera/camera_session.c
    ${REPO_DIR}/apps/rift/rift_app.c
    ${REPO_DIR}/apps/rift/rift_model.c
    ${REPO_DIR}/apps/rift/rift_clock.c
    ${REPO_DIR}/apps/rift/rift_messages.c
    ${REPO_DIR}/apps/rift/rift_channels.c
    ${REPO_DIR}/apps/rift/rift_actions.c
    ${REPO_DIR}/apps/rift/rift_order.c
    ${REPO_DIR}/apps/rift/rift_format.c
    ${REPO_DIR}/apps/rift/rift_format_msg.c
    ${REPO_DIR}/apps/rift/rift_ipc.c
    ${REPO_DIR}/apps/rift/ui/rift_widgets.c
    ${REPO_DIR}/apps/rift/ui/rift_activity.c
    ${REPO_DIR}/apps/rift/ui/rift_nodes.c
    ${REPO_DIR}/apps/rift/ui/rift_detail.c
    ${REPO_DIR}/apps/rift/ui/rift_comms.c
    ${REPO_DIR}/apps/rift/ui/rift_thread.c
    ${REPO_DIR}/apps/timber/timber_app.c
    ${REPO_DIR}/apps/timber/timber_store.c
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
    ${REPO_DIR}/apps/timber/ui/timber_art.c
    ${REPO_DIR}/core/pocketipc/pocketipc.c
    ${REPO_DIR}/core/pocketipc/server.c
    ${REPO_DIR}/core/pocketlog/pocketlog.c
    ${POCKETUI_FONTS})
target_include_directories(pocketos-shell PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../pocketui" "${REPO_DIR}/core"
    "${REPO_DIR}/apps/rift" "${REPO_DIR}/apps/rift/ui")
target_compile_options(pocketos-shell PRIVATE -Wall -Wextra)
target_compile_definitions(pocketos-shell PRIVATE
    POCKETOS_DISPLAY_NAME="${POCKETOS_DISPLAY}"
    POCKETOS_VERSION="${POCKETOS_VERSION}"
    POCKETOS_BUILD_ID="${POCKETOS_BUILD_ID}"
    LV_LVGL_H_INCLUDE_SIMPLE)
target_link_libraries(pocketos-shell PRIVATE cjson m)
# Files runs copy, move and delete on a worker thread (apps/files/files_job.h).
find_package(Threads REQUIRED)
target_link_libraries(pocketos-shell PRIVATE Threads::Threads)

# Where the shell reads the DOORS art (ui/shell/art.h). The device reads the
# installed copy; the simulator reads the committed files in place, so a
# screenshot test needs no install step. POCKETOS_ART_DIR overrides either.
if(POCKETOS_DISPLAY STREQUAL "sdl")
    get_filename_component(POCKETOS_ART_SRC_DIR "${REPO_DIR}/ui/assets/doors" ABSOLUTE)
    target_compile_definitions(pocketos-shell PRIVATE POCKETOS_ART_DIR_DEFAULT="${POCKETOS_ART_SRC_DIR}")
    # The simulator has no camera: Camera's helper runs the fake backend
    # unless POCKETOS_CAMERA_BACKEND says otherwise, and the screen says
    # SIMULATED (docs/apps/CAMERA.md). The device keeps the real default.
    target_compile_definitions(pocketos-shell PRIVATE CAMERA_BACKEND_DEFAULT="fake")
endif()

# PocketTimber art (docs/apps/POCKETTIMBER_ART.md): the sprites Blender
# rendered into docs/design/timber-art/rendered are converted to LVGL image
# arrays here, at configure time, into the build tree; nothing generated is
# committed. Without python3 or without the renders the app builds with its
# placeholder blocks (POCKETTIMBER_ART 0). In the Buildroot package the
# renders and the converter are exported beside the sources by
# apply_to_sdk.sh (docs/ is otherwise left out) and python3 is Buildroot's
# own host-python3 (pocketos.mk), so the image gets the same sprites.
set(TIMBER_ART_RENDERED "${REPO_DIR}/docs/design/timber-art/rendered")
set(TIMBER_ART_OUT "${CMAKE_BINARY_DIR}/timber_art")
set(POCKETTIMBER_ART 0)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND EXISTS "${TIMBER_ART_RENDERED}/anchors.json")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${REPO_DIR}/docs/design/timber-art/tools/png2lvgl.py"
                "${TIMBER_ART_RENDERED}" "${TIMBER_ART_OUT}"
        RESULT_VARIABLE TIMBER_ART_RC OUTPUT_VARIABLE TIMBER_ART_LOG ERROR_VARIABLE TIMBER_ART_LOG)
    if(TIMBER_ART_RC EQUAL 0)
        file(GLOB TIMBER_ART_SOURCES "${TIMBER_ART_OUT}/*.c")
        target_sources(pocketos-shell PRIVATE ${TIMBER_ART_SOURCES})
        set(POCKETTIMBER_ART 1)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     "${TIMBER_ART_RENDERED}/anchors.json")
        string(STRIP "${TIMBER_ART_LOG}" TIMBER_ART_LOG)
        message(STATUS "PocketTimber art: ${TIMBER_ART_LOG}")
    else()
        message(WARNING "PocketTimber art conversion failed, building the placeholder: ${TIMBER_ART_LOG}")
    endif()
else()
    message(STATUS "PocketTimber art: no python3 or no renders, building the placeholder")
endif()
target_compile_definitions(pocketos-shell PRIVATE POCKETTIMBER_ART=${POCKETTIMBER_ART})
target_include_directories(pocketos-shell PRIVATE "${REPO_DIR}/apps/timber/ui"
                                                  "${REPO_DIR}/apps/notes"
                                                  "${REPO_DIR}/apps/files"
                                                  "${REPO_DIR}/apps/clock"
                                                  "${REPO_DIR}/apps/calendar"
                                                  "${REPO_DIR}/apps/calculator"
                                                  "${REPO_DIR}/apps/settings"
                                                  "${REPO_DIR}/apps/wave"
                                                  "${REPO_DIR}/apps/camera")

if(POCKETOS_LVGL_MODE STREQUAL "source")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.defaults" LV_DEFAULTS_BASE)
    if(POCKETOS_DISPLAY STREQUAL "sdl")
        # The evdev driver too, on a Linux host: tests/timber_input_test.c feeds
        # it the touch controller's event grammar through a pipe.
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set(LV_DEFAULTS_EXTRA "LV_USE_SDL 1\nLV_USE_EVDEV 1\nLV_USE_LODEPNG 1\nLV_USE_SNAPSHOT 1\n")
        else()
            set(LV_DEFAULTS_EXTRA "LV_USE_SDL 1\nLV_USE_LODEPNG 1\nLV_USE_SNAPSHOT 1\n")
        endif()
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
    # The simulator is never shipped, so it may honour test hooks: the
    # brightness probe reads POCKETOS_TEST_SYSFS_ROOT when set
    # (tests/brightness_shell_test.sh). The drm build, which is what goes on
    # the card, is compiled without it and always reads /sys.
    target_compile_definitions(pocketos-shell PRIVATE POCKETOS_SHELL_TEST_HOOKS=1)
    # No keyboard base board on a PC. The stub reports absence the way the
    # real bus reports a controller that does not answer, so the simulator
    # takes the same path as a board with the base detached.
    target_sources(pocketos-shell PRIVATE kbd_bus_absent.c)
else()
    pkg_check_modules(DRM REQUIRED libdrm)
    pkg_check_modules(EVDEV REQUIRED libevdev)
    # The physical keyboard's bit-banged bus: libgpiod v2 for the lines and
    # /dev/mem for the pin mux (docs/hardware/
    # KEYBOARD_DRIVER_DESIGN_2026-09-12.md §4). libgpiod2 is already a
    # dependency of the Buildroot package, for radiod's SX1262 HAL.
    pkg_check_modules(GPIOD REQUIRED libgpiod)
    target_sources(pocketos-shell PRIVATE kbd_bus_k230.c)
    target_include_directories(pocketos-shell PRIVATE ${GPIOD_INCLUDE_DIRS})
    target_link_libraries(pocketos-shell PRIVATE ${GPIOD_LIBRARIES})
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

if(POCKETOS_DISPLAY STREQUAL "sdl")
    # PocketRadar in the running app, a finger on the scope (run by
    # tests/radar_shell_test.sh). The shell is not linked; the test defines the
    # app.h entry points the game uses and pocketlog's one entry point itself,
    # and points POCKETOS_STATE_DIR at a directory of its own.
    add_executable(radar_app_test
        ${REPO_DIR}/tests/radar_app_test.c
        chrome.c
        ${REPO_DIR}/apps/radar/radar_app.c
        ${REPO_DIR}/apps/radar/radar_store.c
        ${REPO_DIR}/apps/radar/engine/radar_rng.c
        ${REPO_DIR}/apps/radar/engine/radar_rules.c
        ${REPO_DIR}/apps/radar/engine/radar_score.c
        ${REPO_DIR}/apps/radar/engine/radar_types.c
        ${REPO_DIR}/apps/radar/ui/radar_scope.c
        ${REPO_DIR}/apps/radar/ui/radar_screens.c
        ../pocketui/pos_app_icons.c
        ../pocketui/pocketui.c
        ../pocketui/pos_display.c
        ../pocketui/pos_input.c
        ../pocketui/pos_keyboard.c
        ../pocketui/pos_keymap.c
        ../pocketui/pos_theme.c
        ../pocketui/pos_styles.c
        ${POCKETUI_FONTS})
    target_include_directories(radar_app_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../pocketui"
        "${REPO_DIR}/core" "${REPO_DIR}/apps/radar")
    target_compile_options(radar_app_test PRIVATE -Wall -Wextra)
    target_compile_definitions(radar_app_test PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
    if(POCKETOS_LVGL_MODE STREQUAL "source")
        target_include_directories(radar_app_test PRIVATE "${LVGL_DIR}")
        target_compile_definitions(radar_app_test PRIVATE LV_CONF_PATH="${CMAKE_BINARY_DIR}/lv_conf.h")
    endif()
    target_link_libraries(radar_app_test PRIVATE lvgl ${SDL2_LIBRARIES} m)

    # The scope on its own: its geometry and its tap conversion, without the
    # app around it. The target build does not need either of these.
    add_executable(radar_scope_test
        ${REPO_DIR}/tests/radar_scope_test.c
        ${REPO_DIR}/apps/radar/ui/radar_scope.c
        ${REPO_DIR}/apps/radar/engine/radar_types.c
        ${REPO_DIR}/apps/radar/engine/radar_rng.c
        ${REPO_DIR}/apps/radar/engine/radar_rules.c
        ${REPO_DIR}/apps/radar/engine/radar_score.c
        ../pocketui/pocketui.c
        ../pocketui/pos_display.c
        ../pocketui/pos_input.c
        ../pocketui/pos_keyboard.c
        ../pocketui/pos_keymap.c
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

    # The Timber app under a real LVGL pointer device (run by
    # tests/timber_shell_test.sh). Built with the placeholder blocks: the
    # input path is the same with or without the sprites.
    add_executable(timber_input_test
        ${REPO_DIR}/tests/timber_input_test.c
        ${REPO_DIR}/apps/timber/timber_app.c
        ${REPO_DIR}/apps/timber/timber_store.c
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
        ${REPO_DIR}/apps/timber/ui/timber_art.c
        chrome.c
        ../pocketui/pos_app_icons.c
        ${REPO_DIR}/core/pocketlog/pocketlog.c
        ${REPO_DIR}/core/pocketpaths.c
        ../pocketui/pocketui.c
        ../pocketui/pos_display.c
        ../pocketui/pos_input.c
        ../pocketui/pos_keyboard.c
        ../pocketui/pos_keymap.c
        ../pocketui/pos_theme.c
        ../pocketui/pos_styles.c
        ${POCKETUI_FONTS})
    target_include_directories(timber_input_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../pocketui"
        "${REPO_DIR}/core" "${REPO_DIR}/apps/timber" "${REPO_DIR}/apps/timber/ui")
    target_compile_options(timber_input_test PRIVATE -Wall -Wextra)
    target_compile_definitions(timber_input_test PRIVATE
        POCKETOS_VERSION="${POCKETOS_VERSION}"
        POCKETOS_BUILD_ID="${POCKETOS_BUILD_ID}"
        POCKETTIMBER_ART=0
        LV_LVGL_H_INCLUDE_SIMPLE)
    if(POCKETOS_LVGL_MODE STREQUAL "source")
        target_include_directories(timber_input_test PRIVATE "${LVGL_DIR}")
        target_compile_definitions(timber_input_test PRIVATE LV_CONF_PATH="${CMAKE_BINARY_DIR}/lv_conf.h")
    endif()
    target_link_libraries(timber_input_test PRIVATE lvgl ${SDL2_LIBRARIES} m)

    # The logical key stream and the text field under real LVGL devices
    # (run by tests/pos_input_test.sh). DS v0.1 section 17.
    add_executable(pos_input_test
        ${REPO_DIR}/tests/pos_input_test.c
        ../pocketui/pocketui.c
        ../pocketui/pos_display.c
        ../pocketui/pos_input.c
        ../pocketui/pos_keyboard.c
        ../pocketui/pos_keymap.c
        ../pocketui/pos_theme.c
        ../pocketui/pos_styles.c
        ${POCKETUI_FONTS})
    target_include_directories(pos_input_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../pocketui"
        "${REPO_DIR}/core")
    target_compile_options(pos_input_test PRIVATE -Wall -Wextra)
    target_compile_definitions(pos_input_test PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
    if(POCKETOS_LVGL_MODE STREQUAL "source")
        target_include_directories(pos_input_test PRIVATE "${LVGL_DIR}")
        target_compile_definitions(pos_input_test PRIVATE LV_CONF_PATH="${CMAKE_BINARY_DIR}/lv_conf.h")
    endif()
    target_link_libraries(pos_input_test PRIVATE lvgl ${SDL2_LIBRARIES} m)

    # The touch keyboard typing into a real field under a real pointer
    # device (run by tests/pos_keyboard_test.sh). DS v0.1 section 17.3.
    add_executable(pos_keyboard_test
        ${REPO_DIR}/tests/pos_keyboard_test.c
        ../pocketui/pocketui.c
        ../pocketui/pos_display.c
        ../pocketui/pos_input.c
        ../pocketui/pos_keyboard.c
        ../pocketui/pos_keymap.c
        ../pocketui/pos_theme.c
        ../pocketui/pos_styles.c
        ${POCKETUI_FONTS})
    target_include_directories(pos_keyboard_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../pocketui"
        "${REPO_DIR}/core")
    target_compile_options(pos_keyboard_test PRIVATE -Wall -Wextra)
    target_compile_definitions(pos_keyboard_test PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
    if(POCKETOS_LVGL_MODE STREQUAL "source")
        target_include_directories(pos_keyboard_test PRIVATE "${LVGL_DIR}")
        target_compile_definitions(pos_keyboard_test PRIVATE LV_CONF_PATH="${CMAKE_BINARY_DIR}/lv_conf.h")
    endif()
    target_link_libraries(pos_keyboard_test PRIVATE lvgl ${SDL2_LIBRARIES} m)

    # The launcher tile with an app icon mask against the glyph tile, under a
    # real pointer device (run by tests/doors_shell_test.sh). DS 20.
    add_executable(pocketui_tile_test
        ${REPO_DIR}/tests/pocketui_tile_test.c
        ../pocketui/pos_app_icons.c
        ../pocketui/pocketui.c
        ../pocketui/pos_display.c
        ../pocketui/pos_input.c
        ../pocketui/pos_keyboard.c
        ../pocketui/pos_keymap.c
        ../pocketui/pos_theme.c
        ../pocketui/pos_styles.c
        ${POCKETUI_FONTS})
    target_include_directories(pocketui_tile_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../pocketui"
        "${REPO_DIR}/core")
    target_compile_options(pocketui_tile_test PRIVATE -Wall -Wextra)
    target_compile_definitions(pocketui_tile_test PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
    if(POCKETOS_LVGL_MODE STREQUAL "source")
        target_include_directories(pocketui_tile_test PRIVATE "${LVGL_DIR}")
        target_compile_definitions(pocketui_tile_test PRIVATE LV_CONF_PATH="${CMAKE_BINARY_DIR}/lv_conf.h")
    endif()
    target_link_libraries(pocketui_tile_test PRIVATE lvgl ${SDL2_LIBRARIES} m)

    # The DOORS lock screen under a real pointer and the shell's input stream
    # (DS §31.4; run by tests/doors_shell_test.sh).
    add_executable(shell_lock_test
        ${REPO_DIR}/tests/shell_lock_test.c
        shell_lock.c
        art.c
        art_format.c
        ../pocketui/pos_glyphs.c
        ../pocketui/pocketui.c
        ../pocketui/pos_display.c
        ../pocketui/pos_input.c
        ../pocketui/pos_keyboard.c
        ../pocketui/pos_keymap.c
        ../pocketui/pos_theme.c
        ../pocketui/pos_styles.c
        ${REPO_DIR}/core/pocketlog/pocketlog.c
        ${REPO_DIR}/core/pocketpaths.c
        ${POCKETUI_FONTS})
    target_include_directories(shell_lock_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../pocketui"
        "${REPO_DIR}/core")
    target_compile_options(shell_lock_test PRIVATE -Wall -Wextra)
    target_compile_definitions(shell_lock_test PRIVATE LV_LVGL_H_INCLUDE_SIMPLE
        POCKETOS_VERSION="${POCKETOS_VERSION}" POCKETOS_BUILD_ID="${POCKETOS_BUILD_ID}"
        POCKETOS_ART_DIR_DEFAULT="${POCKETOS_ART_SRC_DIR}")
    if(POCKETOS_LVGL_MODE STREQUAL "source")
        target_include_directories(shell_lock_test PRIVATE "${LVGL_DIR}")
        target_compile_definitions(shell_lock_test PRIVATE LV_CONF_PATH="${CMAKE_BINARY_DIR}/lv_conf.h")
    endif()
    target_link_libraries(shell_lock_test PRIVATE lvgl ${SDL2_LIBRARIES} m)

    # The shared responsive-layout guard every responsive app opens its layout
    # pass with (run by tests/display_geometry_shell_test.sh). DS 21.3, 22.2.
    add_executable(pocketui_layout_test
        ${REPO_DIR}/tests/pocketui_layout_test.c
        ../pocketui/pocketui.c
        ../pocketui/pos_display.c
        ../pocketui/pos_input.c
        ../pocketui/pos_keyboard.c
        ../pocketui/pos_keymap.c
        ../pocketui/pos_theme.c
        ../pocketui/pos_styles.c
        ${POCKETUI_FONTS})
    target_include_directories(pocketui_layout_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../pocketui"
        "${REPO_DIR}/core")
    target_compile_options(pocketui_layout_test PRIVATE -Wall -Wextra)
    target_compile_definitions(pocketui_layout_test PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
    if(POCKETOS_LVGL_MODE STREQUAL "source")
        target_include_directories(pocketui_layout_test PRIVATE "${LVGL_DIR}")
        target_compile_definitions(pocketui_layout_test PRIVATE LV_CONF_PATH="${CMAKE_BINARY_DIR}/lv_conf.h")
    endif()
    target_link_libraries(pocketui_layout_test PRIVATE lvgl ${SDL2_LIBRARIES} m)

    # Display and touch rotated together through LVGL's real evdev driver (run
    # by tests/display_geometry_shell_test.sh). Linux only: it needs the evdev
    # driver the SDL build enables there.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(display_touch_test
            ${REPO_DIR}/tests/display_touch_test.c
            ../pocketui/pos_display.c)
        target_include_directories(display_touch_test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../pocketui")
        target_compile_options(display_touch_test PRIVATE -Wall -Wextra)
        target_compile_definitions(display_touch_test PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
        if(POCKETOS_LVGL_MODE STREQUAL "source")
            target_include_directories(display_touch_test PRIVATE "${LVGL_DIR}")
            target_compile_definitions(display_touch_test PRIVATE LV_CONF_PATH="${CMAKE_BINARY_DIR}/lv_conf.h")
        endif()
        target_link_libraries(display_touch_test PRIVATE lvgl ${SDL2_LIBRARIES} m)
    endif()

    # PocketNotes in the running app, tapped on the real keyboard against a
    # real store (run by tests/notes_shell_test.sh).
    add_executable(notes_app_test
        ${REPO_DIR}/tests/notes_app_test.c
        chrome.c
        ${REPO_DIR}/apps/notes/notes_app.c
        ${REPO_DIR}/apps/notes/notes_store.c
        ${REPO_DIR}/apps/notes/notes_view.c
        ../pocketui/pos_app_icons.c
        ../pocketui/pocketui.c
        ../pocketui/pos_display.c
        ../pocketui/pos_input.c
        ../pocketui/pos_keyboard.c
        ../pocketui/pos_keymap.c
        ../pocketui/pos_theme.c
        ../pocketui/pos_styles.c
        ${POCKETUI_FONTS})
    target_include_directories(notes_app_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../pocketui"
        "${REPO_DIR}/core" "${REPO_DIR}/apps/notes")
    target_compile_options(notes_app_test PRIVATE -Wall -Wextra)
    target_compile_definitions(notes_app_test PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
    if(POCKETOS_LVGL_MODE STREQUAL "source")
        target_include_directories(notes_app_test PRIVATE "${LVGL_DIR}")
        target_compile_definitions(notes_app_test PRIVATE LV_CONF_PATH="${CMAKE_BINARY_DIR}/lv_conf.h")
    endif()
    target_link_libraries(notes_app_test PRIVATE lvgl ${SDL2_LIBRARIES} m)

    # Files in the running app, tapped on the panel and on the real keyboard,
    # against a real (temporary) directory tree (run by tests/files_shell_test.sh).
    add_executable(files_app_test
        ${REPO_DIR}/tests/files_app_test.c
        chrome.c
        ${REPO_DIR}/apps/files/files_app.c
        ${REPO_DIR}/apps/files/files_fs.c
        ${REPO_DIR}/apps/files/files_job.c
        ${REPO_DIR}/apps/files/files_view.c
        ${REPO_DIR}/core/pocketpaths.c
        ../pocketui/pos_app_icons.c
        ../pocketui/pocketui.c
        ../pocketui/pos_display.c
        ../pocketui/pos_input.c
        ../pocketui/pos_keyboard.c
        ../pocketui/pos_keymap.c
        ../pocketui/pos_theme.c
        ../pocketui/pos_styles.c
        ${POCKETUI_FONTS})
    target_include_directories(files_app_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../pocketui"
        "${REPO_DIR}/core" "${REPO_DIR}/apps/files")
    target_compile_options(files_app_test PRIVATE -Wall -Wextra)
    target_compile_definitions(files_app_test PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
    if(POCKETOS_LVGL_MODE STREQUAL "source")
        target_include_directories(files_app_test PRIVATE "${LVGL_DIR}")
        target_compile_definitions(files_app_test PRIVATE LV_CONF_PATH="${CMAKE_BINARY_DIR}/lv_conf.h")
    endif()
    target_link_libraries(files_app_test PRIVATE lvgl ${SDL2_LIBRARIES} m Threads::Threads)

    # Camera in the running app, tapped on the panel, with the real helper on
    # the fake backend behind it (run by tests/camera_shell_test.sh, which
    # hands it make's tests/pos-camera-testhooks).
    add_executable(camera_app_test
        ${REPO_DIR}/tests/camera_app_test.c
        chrome.c
        ${REPO_DIR}/apps/camera/camera_app.c
        ${REPO_DIR}/apps/camera/camera_state.c
        ${REPO_DIR}/apps/camera/camera_layout.c
        ${REPO_DIR}/apps/camera/camera_session.c
        ../pocketui/pos_app_icons.c
        ../pocketui/pocketui.c
        ../pocketui/pos_display.c
        ../pocketui/pos_input.c
        ../pocketui/pos_keyboard.c
        ../pocketui/pos_keymap.c
        ../pocketui/pos_theme.c
        ../pocketui/pos_styles.c
        ${POCKETUI_FONTS})
    target_include_directories(camera_app_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../pocketui"
        "${REPO_DIR}/core" "${REPO_DIR}/apps/camera")
    target_compile_options(camera_app_test PRIVATE -Wall -Wextra)
    target_compile_definitions(camera_app_test PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
    if(POCKETOS_LVGL_MODE STREQUAL "source")
        target_include_directories(camera_app_test PRIVATE "${LVGL_DIR}")
        target_compile_definitions(camera_app_test PRIVATE LV_CONF_PATH="${CMAKE_BINARY_DIR}/lv_conf.h")
    endif()
    target_link_libraries(camera_app_test PRIVATE lvgl ${SDL2_LIBRARIES} m)

    # PocketClock in the running app, tapped on the panel and on the real
    # keyboard, against a real store (run by tests/clock_shell_test.sh).
    add_executable(clock_app_test
        ${REPO_DIR}/tests/clock_app_test.c
        chrome.c
        ${REPO_DIR}/apps/clock/clock_app.c
        ${REPO_DIR}/apps/clock/clock_engine.c
        ${REPO_DIR}/apps/clock/clock_store.c
        ${REPO_DIR}/apps/clock/clock_time.c
        ${REPO_DIR}/apps/clock/clock_alert.c
        ${REPO_DIR}/apps/clock/clock_runtime.c
        ../pocketui/pos_app_icons.c
        shell_alarm.c
        shell_kb_state.c
        ../pocketui/pocketui.c
        ../pocketui/pos_display.c
        ../pocketui/pos_input.c
        ../pocketui/pos_keyboard.c
        ../pocketui/pos_keymap.c
        ../pocketui/pos_theme.c
        ../pocketui/pos_styles.c
        ${POCKETUI_FONTS})
    target_include_directories(clock_app_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../pocketui"
        "${REPO_DIR}/core" "${REPO_DIR}/apps/clock")
    target_compile_options(clock_app_test PRIVATE -Wall -Wextra)
    target_compile_definitions(clock_app_test PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
    if(POCKETOS_LVGL_MODE STREQUAL "source")
        target_include_directories(clock_app_test PRIVATE "${LVGL_DIR}")
        target_compile_definitions(clock_app_test PRIVATE LV_CONF_PATH="${CMAKE_BINARY_DIR}/lv_conf.h")
    endif()
    target_link_libraries(clock_app_test PRIVATE lvgl ${SDL2_LIBRARIES} m)

    # PocketFleet in the running app, a finger on the board (run by
    # tests/fleet_shell_test.sh). The shell is not linked; the test defines
    # the app.h entry points the game uses and pocketlog's one entry point
    # itself, and points POCKETOS_STATE_DIR at a directory of its own, so the
    # save is exercised without touching anything of the host's.
    add_executable(fleet_app_test
        ${REPO_DIR}/tests/fleet_app_test.c
        chrome.c
        ${REPO_DIR}/apps/fleet/fleet_app.c
        ${REPO_DIR}/apps/fleet/engine/fleet_ai.c
        ${REPO_DIR}/apps/fleet/engine/fleet_rng.c
        ${REPO_DIR}/apps/fleet/engine/fleet_rules.c
        ${REPO_DIR}/apps/fleet/engine/fleet_save.c
        ${REPO_DIR}/apps/fleet/engine/fleet_store.c
        ${REPO_DIR}/apps/fleet/engine/fleet_types.c
        ${REPO_DIR}/apps/fleet/ui/fleet_grid.c
        ${REPO_DIR}/apps/fleet/ui/fleet_screen_battle.c
        ${REPO_DIR}/apps/fleet/ui/fleet_screen_command.c
        ${REPO_DIR}/apps/fleet/ui/fleet_screen_deploy.c
        ${REPO_DIR}/apps/fleet/ui/fleet_screen_result.c
        ${REPO_DIR}/apps/fleet/ui/fleet_view.c
        ${REPO_DIR}/apps/fleet/ui/fleet_widgets.c
        ../pocketui/pos_app_icons.c
        ../pocketui/pocketui.c
        ../pocketui/pos_display.c
        ../pocketui/pos_input.c
        ../pocketui/pos_keyboard.c
        ../pocketui/pos_keymap.c
        ../pocketui/pos_theme.c
        ../pocketui/pos_styles.c
        ${POCKETUI_FONTS})
    target_include_directories(fleet_app_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../pocketui"
        "${REPO_DIR}/core" "${REPO_DIR}/apps/fleet")
    target_compile_options(fleet_app_test PRIVATE -Wall -Wextra)
    target_compile_definitions(fleet_app_test PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
    if(POCKETOS_LVGL_MODE STREQUAL "source")
        target_include_directories(fleet_app_test PRIVATE "${LVGL_DIR}")
        target_compile_definitions(fleet_app_test PRIVATE LV_CONF_PATH="${CMAKE_BINARY_DIR}/lv_conf.h")
    endif()
    target_link_libraries(fleet_app_test PRIVATE lvgl ${SDL2_LIBRARIES} m)

    # PocketCalendar in the running app, tapped on the panel (run by
    # tests/calendar_shell_test.sh). The shell is not linked, so the test
    # supplies pocketos_shell_system_day() itself: an unset clock, the moment
    # it is set and the moment it stops being valid are all reachable that way
    # without touching the host's clock. There is no store to point anywhere,
    # because the app has none.
    add_executable(cal_app_test
        ${REPO_DIR}/tests/cal_app_test.c
        chrome.c
        ${REPO_DIR}/apps/calendar/cal_app.c
        ${REPO_DIR}/apps/calendar/cal_date.c
        ${REPO_DIR}/apps/calendar/cal_view.c
        ../pocketui/pos_app_icons.c
        ../pocketui/pocketui.c
        ../pocketui/pos_display.c
        ../pocketui/pos_input.c
        ../pocketui/pos_keyboard.c
        ../pocketui/pos_keymap.c
        ../pocketui/pos_theme.c
        ../pocketui/pos_styles.c
        ${POCKETUI_FONTS})
    target_include_directories(cal_app_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../pocketui"
        "${REPO_DIR}/core" "${REPO_DIR}/apps/calendar")
    target_compile_options(cal_app_test PRIVATE -Wall -Wextra)
    target_compile_definitions(cal_app_test PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
    if(POCKETOS_LVGL_MODE STREQUAL "source")
        target_include_directories(cal_app_test PRIVATE "${LVGL_DIR}")
        target_compile_definitions(cal_app_test PRIVATE LV_CONF_PATH="${CMAKE_BINARY_DIR}/lv_conf.h")
    endif()
    target_link_libraries(cal_app_test PRIVATE lvgl ${SDL2_LIBRARIES} m)

    # PocketCalculator in the running app, tapped on the panel and typed into
    # through the real key stream (run by tests/calculator_shell_test.sh). The
    # shell is not linked; the test defines the app.h entry points itself, as
    # counters, and checks that the app calls none of them. There is no store,
    # and the test checks that nothing is written anywhere.
    add_executable(calc_app_test
        ${REPO_DIR}/tests/calc_app_test.c
        chrome.c
        ${REPO_DIR}/apps/calculator/calc_app.c
        ${REPO_DIR}/apps/calculator/calc_engine.c
        ${REPO_DIR}/apps/calculator/calc_view.c
        ../pocketui/pos_app_icons.c
        ../pocketui/pocketui.c
        ../pocketui/pos_display.c
        ../pocketui/pos_input.c
        ../pocketui/pos_keyboard.c
        ../pocketui/pos_keymap.c
        ../pocketui/pos_theme.c
        ../pocketui/pos_styles.c
        ${POCKETUI_FONTS})
    target_include_directories(calc_app_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../pocketui"
        "${REPO_DIR}/core" "${REPO_DIR}/apps/calculator")
    target_compile_options(calc_app_test PRIVATE -Wall -Wextra)
    target_compile_definitions(calc_app_test PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
    if(POCKETOS_LVGL_MODE STREQUAL "source")
        target_include_directories(calc_app_test PRIVATE "${LVGL_DIR}")
        target_compile_definitions(calc_app_test PRIVATE LV_CONF_PATH="${CMAKE_BINARY_DIR}/lv_conf.h")
    endif()
    target_link_libraries(calc_app_test PRIVATE lvgl ${SDL2_LIBRARIES} m)

    # Settings in the running app, tapped and typed into (run by
    # tests/settings_shell_test.sh). The test plays both services the app
    # talks to: it answers the app's netd calls with scripted JSON and records
    # what the app sent, and it supplies the shell's brightness entry points.
    add_executable(settings_app_test
        ${REPO_DIR}/tests/settings_app_test.c
        chrome.c
        ${REPO_DIR}/apps/settings/settings_app.c
        ${REPO_DIR}/apps/settings/settings_view.c
        ../pocketui/pos_app_icons.c
        ../pocketui/pocketui.c
        ../pocketui/pos_display.c
        ../pocketui/pos_input.c
        ../pocketui/pos_keyboard.c
        ../pocketui/pos_keymap.c
        ../pocketui/pos_theme.c
        ../pocketui/pos_styles.c
        ${POCKETUI_FONTS})
    target_include_directories(settings_app_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../pocketui"
        "${REPO_DIR}/core" "${REPO_DIR}/apps/settings")
    target_compile_options(settings_app_test PRIVATE -Wall -Wextra)
    target_compile_definitions(settings_app_test PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
    if(POCKETOS_LVGL_MODE STREQUAL "source")
        target_include_directories(settings_app_test PRIVATE "${LVGL_DIR}")
        target_compile_definitions(settings_app_test PRIVATE LV_CONF_PATH="${CMAKE_BINARY_DIR}/lv_conf.h")
    endif()
    target_link_libraries(settings_app_test PRIVATE lvgl ${SDL2_LIBRARIES} cjson m)

    # RIFT in the running app, in both orientations (run by
    # tests/rift_shell_test.sh). meshcored is not here: the app finds no
    # service, which is one of the states it has to draw, and the fixtures
    # go into its model through the same entry points its own client uses.
    # The shell is not here either; the app.h entry points are counters.
    #
    # The one RIFT source it does not take is rift_clock.c: the test links
    # tests/rift_test_clock.c in its place, a virtual monotonic clock that
    # moves only as the test pumps LVGL, so the same fixtures draw the same
    # ages however long a run takes (tests/rift_shell_test.sh, section 4).
    # The shell links the real clock; nothing of this reaches it.
    add_executable(rift_app_test
        ${REPO_DIR}/tests/rift_app_test.c
        ${REPO_DIR}/tests/rift_test_clock.c
        chrome.c
        ${REPO_DIR}/apps/rift/rift_app.c
        ${REPO_DIR}/apps/rift/rift_model.c
        ${REPO_DIR}/apps/rift/rift_messages.c
        ${REPO_DIR}/apps/rift/rift_channels.c
        ${REPO_DIR}/apps/rift/rift_actions.c
        ${REPO_DIR}/apps/rift/rift_order.c
        ${REPO_DIR}/apps/rift/rift_format.c
        ${REPO_DIR}/apps/rift/rift_format_msg.c
        ${REPO_DIR}/apps/rift/rift_ipc.c
        ${REPO_DIR}/apps/rift/ui/rift_widgets.c
        ${REPO_DIR}/apps/rift/ui/rift_activity.c
        ${REPO_DIR}/apps/rift/ui/rift_nodes.c
        ${REPO_DIR}/apps/rift/ui/rift_detail.c
        ${REPO_DIR}/apps/rift/ui/rift_comms.c
        ${REPO_DIR}/apps/rift/ui/rift_thread.c
        ${REPO_DIR}/core/pocketipc/pocketipc.c
        ${REPO_DIR}/core/pocketpaths.c
        ../pocketui/pos_app_icons.c
        ../pocketui/pocketui.c
        ../pocketui/pos_display.c
        ../pocketui/pos_input.c
        ../pocketui/pos_keyboard.c
        ../pocketui/pos_keymap.c
        ../pocketui/pos_theme.c
        ../pocketui/pos_styles.c
        ${POCKETUI_FONTS})
    target_include_directories(rift_app_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../pocketui"
        "${REPO_DIR}/core" "${REPO_DIR}/apps/rift" "${REPO_DIR}/apps/rift/ui")
    target_compile_options(rift_app_test PRIVATE -Wall -Wextra)
    target_compile_definitions(rift_app_test PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
    if(POCKETOS_LVGL_MODE STREQUAL "source")
        target_include_directories(rift_app_test PRIVATE "${LVGL_DIR}")
        target_compile_definitions(rift_app_test PRIVATE LV_CONF_PATH="${CMAKE_BINARY_DIR}/lv_conf.h")
    endif()
    target_link_libraries(rift_app_test PRIVATE lvgl ${SDL2_LIBRARIES} cjson m)

    # System in the running app, tapped and scrolled in portrait and in
    # landscape (run by tests/system_shell_test.sh). The test plays sysd: it
    # answers system.info and system.status with scripted JSON, answers or
    # refuses the two power actions without stopping anything, and records
    # every call. The shell's app.h entry points are counters.
    add_executable(system_app_test
        ${REPO_DIR}/tests/system_app_test.c
        chrome.c
        ${REPO_DIR}/apps/system/system_app.c
        ${REPO_DIR}/apps/system/system_view.c
        ../pocketui/pos_app_icons.c
        ../pocketui/pos_brand_mark.c
        ../pocketui/pocketui.c
        ../pocketui/pos_display.c
        ../pocketui/pos_input.c
        ../pocketui/pos_keyboard.c
        ../pocketui/pos_keymap.c
        ../pocketui/pos_theme.c
        ../pocketui/pos_styles.c
        ${POCKETUI_FONTS})
    target_include_directories(system_app_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../pocketui"
        "${REPO_DIR}/core" "${REPO_DIR}/apps/system")
    target_compile_options(system_app_test PRIVATE -Wall -Wextra)
    target_compile_definitions(system_app_test PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
    if(POCKETOS_LVGL_MODE STREQUAL "source")
        target_include_directories(system_app_test PRIVATE "${LVGL_DIR}")
        target_compile_definitions(system_app_test PRIVATE LV_CONF_PATH="${CMAKE_BINARY_DIR}/lv_conf.h")
    endif()
    target_link_libraries(system_app_test PRIVATE lvgl ${SDL2_LIBRARIES} cjson m)

    # Wave in the running app, tapped and typed into, driving a scripted fake
    # of its audio helper (run by tests/wave_shell_test.sh). The shell is not
    # linked; the test supplies app.h and records the status hint and the
    # keyboard requests. No audio device is involved at any point.
    add_executable(wave_app_test
        ${REPO_DIR}/tests/wave_app_test.c
        ${REPO_DIR}/apps/wave/wave_app.c
        ${REPO_DIR}/apps/wave/wave_view.c
        ${REPO_DIR}/apps/wave/wave_session.c
        ${REPO_DIR}/apps/wave/wave_text.c
        ../pocketui/pos_app_icons.c
        ../pocketui/pocketui.c
        ../pocketui/pos_display.c
        ../pocketui/pos_input.c
        ../pocketui/pos_keyboard.c
        ../pocketui/pos_keymap.c
        ../pocketui/pos_theme.c
        ../pocketui/pos_styles.c
        ${POCKETUI_FONTS})
    target_include_directories(wave_app_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../pocketui"
        "${REPO_DIR}/core" "${REPO_DIR}/apps/wave")
    target_compile_options(wave_app_test PRIVATE -Wall -Wextra)
    target_compile_definitions(wave_app_test PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
    if(POCKETOS_LVGL_MODE STREQUAL "source")
        target_include_directories(wave_app_test PRIVATE "${LVGL_DIR}")
        target_compile_definitions(wave_app_test PRIVATE LV_CONF_PATH="${CMAKE_BINARY_DIR}/lv_conf.h")
    endif()
    target_link_libraries(wave_app_test PRIVATE lvgl ${SDL2_LIBRARIES} m)

    # The system alert's keyboard isolation and focus ownership (run by
    # tests/alert_shell_test.sh). DS v0.1 section 18.5, 18.6 and 18.8: while
    # an alert holds the panel nothing typed may reach the app underneath.
    # It links shell_kb_state.c so the suppression rule under test is the
    # shell's own and not a copy of it.
    add_executable(shell_alarm_test
        ${REPO_DIR}/tests/shell_alarm_test.c
        shell_alarm.c
        shell_kb_state.c
        ${REPO_DIR}/apps/clock/clock_engine.c
        ${REPO_DIR}/apps/clock/clock_store.c
        ${REPO_DIR}/apps/clock/clock_time.c
        ${REPO_DIR}/apps/clock/clock_alert.c
        ${REPO_DIR}/apps/clock/clock_runtime.c
        ../pocketui/pocketui.c
        ../pocketui/pos_display.c
        ../pocketui/pos_input.c
        ../pocketui/pos_keyboard.c
        ../pocketui/pos_keymap.c
        ../pocketui/pos_theme.c
        ../pocketui/pos_styles.c
        ${REPO_DIR}/core/pocketpaths.c
        ${POCKETUI_FONTS})
    target_include_directories(shell_alarm_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../pocketui"
        "${REPO_DIR}/core" "${REPO_DIR}/apps/clock")
    target_compile_options(shell_alarm_test PRIVATE -Wall -Wextra)
    target_compile_definitions(shell_alarm_test PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
    if(POCKETOS_LVGL_MODE STREQUAL "source")
        target_include_directories(shell_alarm_test PRIVATE "${LVGL_DIR}")
        target_compile_definitions(shell_alarm_test PRIVATE LV_CONF_PATH="${CMAKE_BINARY_DIR}/lv_conf.h")
    endif()
    target_link_libraries(shell_alarm_test PRIVATE lvgl ${SDL2_LIBRARIES} m)

    # The physical keyboard's translation layer (run by
    # tests/pos_keymap_test.sh). Pure, but it speaks LVGL's key vocabulary,
    # so it is built here with the rest.
    add_executable(pos_keymap_test
        ${REPO_DIR}/tests/pos_keymap_test.c
        ../pocketui/pos_keymap.c
        ../pocketui/pos_input.c)
    target_include_directories(pos_keymap_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../pocketui"
        "${REPO_DIR}/core")
    target_compile_options(pos_keymap_test PRIVATE -Wall -Wextra)
    target_compile_definitions(pos_keymap_test PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
    if(POCKETOS_LVGL_MODE STREQUAL "source")
        target_include_directories(pos_keymap_test PRIVATE "${LVGL_DIR}")
        target_compile_definitions(pos_keymap_test PRIVATE LV_CONF_PATH="${CMAKE_BINARY_DIR}/lv_conf.h")
    endif()
    target_link_libraries(pos_keymap_test PRIVATE lvgl ${SDL2_LIBRARIES} m)

    # The physical keyboard end to end (run by tests/kbd_shell_test.sh): a
    # fake controller on a fake bus, the real poll timer, the real key map
    # and the real stream, into a real text field. The bus backend linked
    # here is the absent stub, so the test supplies its own bus through
    # shell_kbd_attach().
    add_executable(shell_kbd_test
        ${REPO_DIR}/tests/shell_kbd_test.c
        shell_kbd.c
        kbd_tca8418.c
        kbd_presence.c
        kbd_bus_absent.c
        ../pocketui/pocketui.c
        ../pocketui/pos_display.c
        ../pocketui/pos_input.c
        ../pocketui/pos_keyboard.c
        ../pocketui/pos_keymap.c
        ../pocketui/pos_theme.c
        ../pocketui/pos_styles.c
        ${REPO_DIR}/core/pocketlog/pocketlog.c
        ${REPO_DIR}/core/pocketpaths.c
        ${POCKETUI_FONTS})
    target_include_directories(shell_kbd_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../pocketui"
        "${REPO_DIR}/core")
    target_compile_options(shell_kbd_test PRIVATE -Wall -Wextra)
    target_compile_definitions(shell_kbd_test PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
    if(POCKETOS_LVGL_MODE STREQUAL "source")
        target_include_directories(shell_kbd_test PRIVATE "${LVGL_DIR}")
        target_compile_definitions(shell_kbd_test PRIVATE LV_CONF_PATH="${CMAKE_BINARY_DIR}/lv_conf.h")
    endif()
    target_link_libraries(shell_kbd_test PRIVATE lvgl ${SDL2_LIBRARIES} m)
endif()
