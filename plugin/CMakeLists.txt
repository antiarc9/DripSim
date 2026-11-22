cmake_minimum_required(VERSION 3.21)

project(DripSim VERSION 2.0.0 LANGUAGES CXX)
set(DRIPSIM_SWEAT_VERSION 1.3.0)  # the sweat mode has its own release line (1.2.2 was the last early-access build)

# CommonLibSSE-NG 7.2.0, the same release DazPoser builds against (copied into lib/).
option(DRIPSIM_BUILD_SWEAT "Also build the sweat mode (DripSimSweat.dll)" ON)
# The soak's draining sheet (rain/SoakFlow.*), free since 2026-11-13. OFF = DripSimSoak.dll without it (SoakFlow=1 in the INI then does nothing).
option(DRIPSIM_SOAK_FLOW "Build the new soak (draining sheet) into DripSimSoak.dll" ON)
set(BUILD_TESTS OFF CACHE BOOL "Disable CommonLibSSE tests" FORCE)
set(COMMONLIB_PREBUILT ON CACHE BOOL "Use the pinned CommonLibSSE release build when compatible" FORCE)
add_subdirectory(lib/CommonLibSSE-NG)
if(NOT COMMAND add_commonlibsse_plugin)
    include(lib/CommonLibSSE-NG/cmake/CommonLibSSE.cmake)
endif()

# The plugins: the framework (DripSim.dll: hooks, body, gravity field, film layers, API) and the modes, built against api/DripSimAPI.h
# (DripSimRain.dll, DripSimSoak.dll, DripSimSweat.dll). A mode never touches the engine hooks; it only talks to the framework through the API.
add_commonlibsse_plugin(DripSim
    NAME DripSim
    AUTHOR "DripSim"
    VERSION ${PROJECT_VERSION}
    SOURCES
        src/core/main.cpp
        src/core/Core.cpp
        src/core/World.cpp
        src/core/Spike.cpp
        src/core/Frame.cpp
        src/core/Texture.cpp
        src/core/Body.cpp
        src/core/Field.cpp
        src/core/Film.cpp
        src/core/WetDetail.cpp
        src/core/Gravity.cpp
        src/core/Params.cpp
        src/core/SystemInfo.cpp
)
add_commonlibsse_plugin(DripSimRain
    NAME DripSimRain
    AUTHOR "DripSim"
    VERSION ${PROJECT_VERSION}
    SOURCES
        src/rain/RainMain.cpp
        src/rain/RainMode.cpp
)
# DripSimSoak.dll: the same water code as the rain, built as the "soak" mode (water after a swim) with its own INI and layer (DSW_KIND, RainMode.cpp)
add_commonlibsse_plugin(DripSimSoak
    NAME DripSimSoak
    AUTHOR "DripSim"
    VERSION ${PROJECT_VERSION}
    SOURCES
        src/rain/RainMain.cpp
        src/rain/RainMode.cpp
)

if(DRIPSIM_BUILD_SWEAT)
    add_commonlibsse_plugin(DripSimSweat
        NAME DripSimSweat
        AUTHOR "DripSim"
        VERSION ${DRIPSIM_SWEAT_VERSION}
        SOURCES
            src/sweat/SweatMain.cpp
            src/sweat/SweatMode.cpp
    )
endif()



set(DRIPSIM_TARGETS DripSim DripSimRain DripSimSoak)
if(DRIPSIM_BUILD_SWEAT)
    list(APPEND DRIPSIM_TARGETS DripSimSweat)
endif()
foreach(target ${DRIPSIM_TARGETS})
    target_compile_features(${target} PRIVATE cxx_std_23)
    target_include_directories(${target} PRIVATE src/core ${CMAKE_CURRENT_SOURCE_DIR}/../api)
    target_precompile_headers(${target} PRIVATE src/core/PCH.h)
endforeach()
target_compile_definitions(DripSim PRIVATE DS_VERSION="${PROJECT_VERSION}")
# SystemInfo.cpp uses the real DXGI headers: without the precompiled header (CommonLib has its own D3D types)
set_source_files_properties(src/core/SystemInfo.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
target_compile_definitions(DripSimRain PRIVATE DSR_VERSION="${PROJECT_VERSION}" DSW_KIND=1 DRIPSIM_SOAK_FLOW=0)
target_compile_definitions(DripSimSoak PRIVATE DSR_VERSION="${PROJECT_VERSION}" DSW_KIND=2)
if(DRIPSIM_BUILD_SWEAT)
    target_compile_definitions(DripSimSweat PRIVATE DSS_VERSION="${DRIPSIM_SWEAT_VERSION}")
endif()

# DripSimPreview: runs a mode without the game and writes pictures (see tools/preview/Preview.cpp). Same film, field and mode sources as the plugins.
add_executable(DripSimPreview
    tools/preview/Preview.cpp
    src/core/Field.cpp
    src/core/Film.cpp
    src/core/WetDetail.cpp
    src/rain/RainMode.cpp
)
if(DRIPSIM_BUILD_SWEAT)
    target_sources(DripSimPreview PRIVATE src/sweat/SweatMode.cpp)
    target_compile_definitions(DripSimPreview PRIVATE DRIPSIM_WITH_SWEAT)
    target_include_directories(DripSimPreview PRIVATE src/sweat)
endif()
if(DRIPSIM_SOAK_FLOW)
    target_sources(DripSimSoak PRIVATE src/rain/SoakFlow.cpp)
    target_sources(DripSimPreview PRIVATE src/rain/SoakFlow.cpp)
    target_compile_definitions(DripSimSoak PRIVATE DRIPSIM_SOAK_FLOW=1)
    target_compile_definitions(DripSimPreview PRIVATE DRIPSIM_SOAK_FLOW=1)
else()
    target_compile_definitions(DripSimSoak PRIVATE DRIPSIM_SOAK_FLOW=0)
    target_compile_definitions(DripSimPreview PRIVATE DRIPSIM_SOAK_FLOW=0)
endif()
# HeavyMagic's skin flow (C:/Projects/chim/HeavyMagic): a DripSim mode without engine code, previewed here as --mode heavymagic
set(HEAVYMAGIC_SKIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../HeavyMagic/plugin/src/skin")
if(EXISTS "${HEAVYMAGIC_SKIN_DIR}/SkinMode.cpp")
    target_sources(DripSimPreview PRIVATE "${HEAVYMAGIC_SKIN_DIR}/SkinMode.cpp" "${HEAVYMAGIC_SKIN_DIR}/Fx.cpp" "${HEAVYMAGIC_SKIN_DIR}/Veins.cpp" "${HEAVYMAGIC_SKIN_DIR}/Baked.cpp")
    target_compile_definitions(DripSimPreview PRIVATE DRIPSIM_WITH_HEAVYMAGIC)
    target_include_directories(DripSimPreview PRIVATE "${HEAVYMAGIC_SKIN_DIR}")
endif()
target_compile_features(DripSimPreview PRIVATE cxx_std_23)
target_compile_definitions(DripSimPreview PRIVATE DRIPSIM_PREVIEW)
target_include_directories(DripSimPreview PRIVATE src/core src/rain ${CMAKE_CURRENT_SOURCE_DIR}/../api)
target_compile_options(DripSimPreview PRIVATE /FI${CMAKE_CURRENT_SOURCE_DIR}/tools/preview/PreviewShim.h)
