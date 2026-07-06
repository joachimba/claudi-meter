cmake_minimum_required(VERSION 3.15)
project(clawdmeter_sim CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Mirror the firmware's LVGL feature flags (see firmware/platformio.ini) and
# add the sim build flag the firmware headers gate on.
add_compile_definitions(
    LV_CONF_SKIP=1
    LV_COLOR_DEPTH=16
    LV_USE_LOG=0
    LV_USE_BAR=1
    LV_USE_LABEL=1
    LV_USE_ARC=1
    LV_USE_BTN=1
    LV_USE_LINE=1
    LV_USE_OBJ=1
    LV_USE_IMG=1
    LV_USE_IMAGE=1
    LV_USE_ANIMIMG=0
    LV_TICK_CUSTOM=1
    LV_USE_SNAPSHOT=1
    LV_USE_CANVAS=1
    CLAWDMETER_SIM_BUILD=1
)

# Skip LVGL's extras — we only need the core library.
set(LV_CONF_SKIP YES CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL YES CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES YES CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS YES CACHE BOOL "" FORCE)
add_subdirectory(lvgl)

# SDL2 — installed via Homebrew on macOS.
find_package(SDL2 REQUIRED)
if(NOT SDL2_INCLUDE_DIRS AND TARGET SDL2::SDL2)
    get_target_property(SDL2_INCLUDE_DIRS SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
endif()

set(FW_SRC "${CMAKE_SOURCE_DIR}/../firmware/src")

add_executable(clawdmeter-sim
    main.cpp
    stubs/sim_stubs.cpp
    ${FW_SRC}/ui.cpp
    ${FW_SRC}/splash.cpp
    ${FW_SRC}/settings.cpp
    ${FW_SRC}/pomodoro.cpp
    ${FW_SRC}/oled_protect.cpp
    ${FW_SRC}/usage_rate.cpp
    ${FW_SRC}/font_styrene_12.c
    ${FW_SRC}/font_styrene_14.c
    ${FW_SRC}/font_styrene_16.c
    ${FW_SRC}/font_styrene_20.c
    ${FW_SRC}/font_styrene_24.c
    ${FW_SRC}/font_styrene_28.c
    ${FW_SRC}/font_styrene_48.c
    ${FW_SRC}/font_tiempos_34.c
    ${FW_SRC}/font_tiempos_56.c
    ${FW_SRC}/font_mono_18.c
    ${FW_SRC}/font_mono_32.c
)

target_include_directories(clawdmeter-sim PRIVATE
    include
    ${FW_SRC}
    ${SDL2_INCLUDE_DIRS}
)

if(TARGET SDL2::SDL2)
    target_link_libraries(clawdmeter-sim PRIVATE lvgl SDL2::SDL2)
else()
    target_link_libraries(clawdmeter-sim PRIVATE lvgl ${SDL2_LIBRARIES})
endif()

# Suppress noisy warnings from LVGL fonts and Arduino-style code
target_compile_options(clawdmeter-sim PRIVATE
    -Wno-unused-parameter
    -Wno-deprecated-declarations
)
