cmake_minimum_required(VERSION 3.24)
project(HRVR CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(minhook CONFIG REQUIRED)
find_package(OpenXR CONFIG REQUIRED)

# Everything but the developer tools: shared by the release DLL and the maintainer's developer DLL.
add_library(hrvr_common OBJECT
    src/dllmain.cpp
    src/log.cpp
    src/crashreport.cpp
    src/hotkeys.cpp
    src/game.cpp
    src/sigdb.cpp
    src/stereo.cpp
    src/xr.cpp
    src/vrinput.cpp
    src/pacing.cpp
    src/videowatch.cpp
    src/harness.cpp
    src/engineobj.cpp
    src/heapscan.cpp
    src/bones.cpp
    src/player.cpp
    src/camdirector.cpp
    src/prompts.cpp
    src/fpcam.cpp
    src/fpbody.cpp
    src/movielink.cpp
    src/audio.cpp
    src/rendersize.cpp
    src/panel.cpp
    src/welcome_draw.cpp
    src/welcome.cpp
    src/ari_draw.cpp
    src/ari_copy_draw.cpp
    src/ari.cpp
    src/menus.cpp
    src/story.cpp
    src/summary.cpp
)
target_compile_definitions(hrvr_common PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(hrvr_common PUBLIC $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /Zi>)
target_include_directories(hrvr_common PUBLIC src)
target_link_libraries(hrvr_common PUBLIC minhook::minhook OpenXR::headers OpenXR::openxr_loader
    d3d11 dxgi bcrypt d3dcompiler gdi32 d2d1 dwrite windowscodecs)

# 0.23.0 (playbook 11, panel V6): the VR panel's font, Montserrat (SIL Open Font License 1.1, assets/fonts/OFL.txt),
# built into the DLL as RCDATA 3101 Bold, 3102 SemiBold, 3103 Regular (panel.cpp adds them for the game process only).
set(HRVR_FONTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts")
set(HRVR_FONTS_RC "${CMAKE_CURRENT_BINARY_DIR}/gen/fonts.rc")
file(WRITE "${HRVR_FONTS_RC}"
  "3101 RCDATA \"${HRVR_FONTS}/Montserrat-Bold.ttf\"\n3102 RCDATA \"${HRVR_FONTS}/Montserrat-SemiBold.ttf\"\n3103 RCDATA \"${HRVR_FONTS}/Montserrat-Regular.ttf\"\n")

# 0.28.0 (playbook 28.3): the default settings file is built into the DLL, which writes it next to HeavyRain.exe when
# there is none (the installer never ships one that could overwrite the player's). Source: hrvr_default.ini.
set(HRVR_DEFAULT_INI "${CMAKE_CURRENT_SOURCE_DIR}/hrvr_default.ini")
file(READ "${HRVR_DEFAULT_INI}" HRVR_INI_TEXT)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${HRVR_DEFAULT_INI}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/gen/default_ini.inc" "R\"HRVRINI(${HRVR_INI_TEXT})HRVRINI\"\n")
target_include_directories(hrvr_common PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/gen")

# The mod players get (build/<config>/dinput8.dll -> ../files): no developer tools (src/devtools_off.cpp, devtools.h).
add_library(hrvr SHARED src/devtools_off.cpp src/dinput8.def "${HRVR_FONTS_RC}")
target_link_libraries(hrvr PRIVATE hrvr_common)
set_target_properties(hrvr PROPERTIES OUTPUT_NAME "dinput8")
# /PDBALTPATH: the DLL names its symbols file "dinput8.pdb" instead of the build machine's full path.
target_link_options(hrvr PRIVATE /DEBUG /OPT:REF /PDBALTPATH:dinput8.pdb)

# The maintainer's own developer build (target hrvr_dev, build/dev/<config>/dinput8.dll), only where its folder
# exists: leave src/devtools out of a public source release.
set(_hr_has_devtools OFF)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/devtools/devtools.cmake")
    set(_hr_has_devtools ON)
    include("${CMAKE_CURRENT_SOURCE_DIR}/src/devtools/devtools.cmake")
    target_sources(hrvr_dev PRIVATE "${HRVR_FONTS_RC}")
endif()

# `cmake --build build --config Release --target deploy` copies the proxy next to HeavyRain.exe.
set(_hr_default_game "")
foreach(_dir "${CMAKE_CURRENT_SOURCE_DIR}/../.." "${CMAKE_CURRENT_SOURCE_DIR}/..")
    if(NOT _hr_default_game AND EXISTS "${_dir}/HeavyRain.exe")
        get_filename_component(_hr_default_game "${_dir}" ABSOLUTE)
    endif()
endforeach()
set(HRVR_GAME_DIR "${_hr_default_game}" CACHE PATH "Heavy Rain install dir")
if(HRVR_GAME_DIR)
    # The game folder gets the developer build where it exists (F7 / F8), else the release build.
    if(_hr_has_devtools)
        set(_hr_deploy_target hrvr_dev)
    else()
        set(_hr_deploy_target hrvr)
    endif()
    add_custom_target(deploy
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${_hr_deploy_target}> "${HRVR_GAME_DIR}/dinput8.dll"
        DEPENDS ${_hr_deploy_target})
endif()

# Desk tests: only where the tests folder exists (the public source leaves it out - several fixtures are rig and clip
# data read from the game files).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
enable_testing()
add_executable(pose_tests tests/pose_tests.cpp)
target_compile_options(pose_tests PRIVATE /W4 /permissive-)
add_test(NAME pose_tests COMMAND pose_tests)

add_executable(xr_input_tests tests/xr_input_tests.cpp)
target_compile_options(xr_input_tests PRIVATE /W4 /permissive-)
add_test(NAME xr_input_tests COMMAND xr_input_tests)

add_executable(xr_geometry_tests tests/xr_geometry_tests.cpp)
target_compile_options(xr_geometry_tests PRIVATE /W4 /permissive-)
add_test(NAME xr_geometry_tests COMMAND xr_geometry_tests)

add_executable(vrinput_tests tests/vrinput_tests.cpp)
target_include_directories(vrinput_tests PRIVATE src)
target_compile_options(vrinput_tests PRIVATE /W4 /permissive-)
add_test(NAME vrinput_tests COMMAND vrinput_tests)

add_executable(haptics_tests tests/haptics_tests.cpp)
target_include_directories(haptics_tests PRIVATE src)
target_compile_options(haptics_tests PRIVATE /W4 /permissive-)
add_test(NAME haptics_tests COMMAND haptics_tests)

# 1.0.1: XR swapchain colour format (washed-out double gamma on runtimes without RGBA sRGB).
add_executable(swap_format_tests tests/swap_format_tests.cpp)
target_include_directories(swap_format_tests PRIVATE src)
target_compile_options(swap_format_tests PRIVATE /W4 /permissive-)
add_test(NAME swap_format_tests COMMAND swap_format_tests)

# 0.34.0: the other-build search on the analysed Steam exe mapped as an image (skipped without the exe).
add_executable(sigdb_tests tests/sigdb_tests.cpp src/sigdb.cpp src/log.cpp)
target_include_directories(sigdb_tests PRIVATE src)
target_compile_definitions(sigdb_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(sigdb_tests PRIVATE /W4 /permissive-)
add_test(NAME sigdb_tests COMMAND sigdb_tests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/build/Release)

add_executable(xinput_tests tests/xinput_tests.cpp)
target_include_directories(xinput_tests PRIVATE src)
target_compile_options(xinput_tests PRIVATE /W4 /permissive-)
add_test(NAME xinput_tests COMMAND xinput_tests)

add_executable(movie_item_tests tests/movie_item_tests.cpp)
target_include_directories(movie_item_tests PRIVATE src tests)
target_compile_options(movie_item_tests PRIVATE /W4 /permissive-)
add_test(NAME movie_item_tests COMMAND movie_item_tests)

add_executable(item_seam_tests tests/item_seam_tests.cpp)
target_include_directories(item_seam_tests PRIVATE src)
target_compile_options(item_seam_tests PRIVATE /W4 /permissive-)
add_test(NAME item_seam_tests COMMAND item_seam_tests)

add_executable(steam_shadow_tests tests/steam_shadow_tests.cpp)
target_compile_options(steam_shadow_tests PRIVATE /W4 /permissive-)
add_test(NAME steam_shadow_tests COMMAND steam_shadow_tests)

add_executable(stereo_math_tests tests/stereo_math_tests.cpp)
target_include_directories(stereo_math_tests PRIVATE src)
target_compile_options(stereo_math_tests PRIVATE /W4 /permissive-)
add_test(NAME stereo_math_tests COMMAND stereo_math_tests)

add_executable(fp_tests tests/fp_tests.cpp)
target_include_directories(fp_tests PRIVATE src)
target_compile_options(fp_tests PRIVATE /W4 /permissive-)
add_test(NAME fp_tests COMMAND fp_tests)

add_executable(fp_math_tests tests/fp_math_tests.cpp)
target_compile_options(fp_math_tests PRIVATE /W4 /permissive-)
add_test(NAME fp_math_tests COMMAND fp_math_tests)

add_executable(help_menu_tests tests/help_menu_tests.cpp)
target_compile_options(help_menu_tests PRIVATE /W4 /permissive-)
add_test(NAME help_menu_tests COMMAND help_menu_tests)

add_executable(gamma_filter_tests tests/gamma_filter_tests.cpp)
target_include_directories(gamma_filter_tests PRIVATE src)
target_compile_options(gamma_filter_tests PRIVATE /W4 /permissive-)
add_test(NAME gamma_filter_tests COMMAND gamma_filter_tests)

add_executable(ari_gesture_tests tests/ari_gesture_tests.cpp)
target_include_directories(ari_gesture_tests PRIVATE src)
target_compile_options(ari_gesture_tests PRIVATE /W4 /permissive-)
add_test(NAME ari_gesture_tests COMMAND ari_gesture_tests)

add_executable(ari_copy_tests tests/ari_copy_tests.cpp)
target_include_directories(ari_copy_tests PRIVATE src)
target_compile_options(ari_copy_tests PRIVATE /W4 /permissive-)
add_test(NAME ari_copy_tests COMMAND ari_copy_tests)
add_executable(ari_mesh_load_tests tests/ari_mesh_load_tests.cpp)
target_include_directories(ari_mesh_load_tests PRIVATE src)
target_compile_options(ari_mesh_load_tests PRIVATE /W4 /permissive-)
add_test(NAME ari_mesh_load_tests COMMAND ari_mesh_load_tests)

add_executable(f3_comfort_tests tests/f3_comfort_tests.cpp)
target_include_directories(f3_comfort_tests PRIVATE src)
target_compile_options(f3_comfort_tests PRIVATE /W4 /permissive-)
add_test(NAME f3_comfort_tests COMMAND f3_comfort_tests)

add_executable(turn_gate_tests tests/turn_gate_tests.cpp)
target_include_directories(turn_gate_tests PRIVATE src)
target_compile_options(turn_gate_tests PRIVATE /W4 /permissive-)
add_test(NAME turn_gate_tests COMMAND turn_gate_tests)

add_executable(prompt_place_tests tests/prompt_place_tests.cpp)
target_include_directories(prompt_place_tests PRIVATE src)
target_compile_options(prompt_place_tests PRIVATE /W4 /permissive-)
add_test(NAME prompt_place_tests COMMAND prompt_place_tests)

add_executable(fpbody_tests tests/fpbody_tests.cpp)
target_include_directories(fpbody_tests PRIVATE src)
target_compile_options(fpbody_tests PRIVATE /W4 /permissive-)
add_test(NAME fpbody_tests COMMAND fpbody_tests)

# 0.16.0: the bone rules proven by the rig pictures, on a real rig from the game files (tools/rig_fixture.py).
add_executable(rigmap_tests tests/rigmap_tests.cpp)
target_include_directories(rigmap_tests PRIVATE src tests)
target_compile_options(rigmap_tests PRIVATE /W4 /permissive-)
add_test(NAME rigmap_tests COMMAND rigmap_tests)

# Playbook 19.8 point 11 Gate 2: the carry rules of rigmap.h as a desk DLL for tools/rig_gate2.py (not the mod).
add_library(carry_desk SHARED EXCLUDE_FROM_ALL tests/carry_desk.cpp)
target_include_directories(carry_desk PRIVATE src)
target_compile_options(carry_desk PRIVATE /W4 /permissive-)

# 0.22.0: finger rules (playbook 14.12) on the four playable rigs from the game files
add_executable(finger_tests tests/finger_tests.cpp)
target_include_directories(finger_tests PRIVATE src tests)
target_compile_options(finger_tests PRIVATE /W4 /permissive- /bigobj)
add_test(NAME finger_tests COMMAND finger_tests)

add_executable(vr_size_audio_tests tests/vr_size_audio_tests.cpp)
target_include_directories(vr_size_audio_tests PRIVATE src)
target_compile_options(vr_size_audio_tests PRIVATE /W4 /permissive-)
add_test(NAME vr_size_audio_tests COMMAND vr_size_audio_tests)

add_executable(panel_tests tests/panel_tests.cpp)
target_include_directories(panel_tests PRIVATE src)
target_compile_options(panel_tests PRIVATE /W4 /permissive-)
add_test(NAME panel_tests COMMAND panel_tests)

# Not a test: renders the real panel offline to a BMP (layout review without the game).
add_executable(panel_preview tests/panel_preview.cpp src/panel.cpp "${HRVR_FONTS_RC}")
target_compile_definitions(panel_preview PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(panel_preview PRIVATE gdi32)

# Not a test: the ARI glasses copy drawn by the mod's own renderer on WARP (tools/ari_copy.py ->
# docs/items/ari_copy/ARI_COPY_HEADSET.png). Built only on request: --target ari_copy_preview.
add_executable(ari_copy_preview EXCLUDE_FROM_ALL tests/ari_copy_preview.cpp src/ari_copy_draw.cpp)
target_include_directories(ari_copy_preview PRIVATE src)
target_compile_definitions(ari_copy_preview PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(ari_copy_preview PRIVATE d3d11 d3dcompiler)

# Not a test: every welcome window (playbook 11.4) drawn by the mod's own src/welcome_draw.cpp, saved as BMPs
# (tools/welcome_pictures.py -> docs/welcome/*.png). Built only on request: --target welcome_preview.
add_executable(welcome_preview EXCLUDE_FROM_ALL tests/welcome_preview.cpp src/welcome_draw.cpp "${HRVR_FONTS_RC}")
target_include_directories(welcome_preview PRIVATE src)
target_compile_definitions(welcome_preview PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(welcome_preview PRIVATE d2d1 dwrite windowscodecs)

# Not a test: the two ARI glasses hint pictures (1.6) from src/ari_draw.cpp -> docs/ari/*.png. On request only.
add_executable(ari_hint_preview EXCLUDE_FROM_ALL tests/ari_hint_preview.cpp src/ari_draw.cpp "${HRVR_FONTS_RC}")
target_include_directories(ari_hint_preview PRIVATE src)
target_compile_definitions(ari_hint_preview PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(ari_hint_preview PRIVATE d2d1 dwrite windowscodecs)

add_executable(harness_tests tests/harness_tests.cpp)
target_include_directories(harness_tests PRIVATE src)
target_compile_options(harness_tests PRIVATE /W4 /permissive-)
add_test(NAME harness_tests COMMAND harness_tests)

# Privacy: what players send (the log, crash reports) never names them (src/log.cpp LogScrub).
add_executable(log_tests tests/log_tests.cpp src/log.cpp)
target_compile_definitions(log_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(log_tests PRIVATE /W4 /permissive-)
add_test(NAME log_tests COMMAND log_tests)
endif()
