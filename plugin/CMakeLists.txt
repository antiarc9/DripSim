cmake_minimum_required(VERSION 3.21)

project(DripSim VERSION 1.2.0 LANGUAGES CXX)

# CommonLibSSE-NG 7.2.0, the same release DazPoser builds against (copied into lib/).
set(BUILD_TESTS OFF CACHE BOOL "Disable CommonLibSSE tests" FORCE)
set(COMMONLIB_PREBUILT ON CACHE BOOL "Use the pinned CommonLibSSE release build when compatible" FORCE)
add_subdirectory(lib/CommonLibSSE-NG)
if(NOT COMMAND add_commonlibsse_plugin)
    include(lib/CommonLibSSE-NG/cmake/CommonLibSSE.cmake)
endif()

# Two plugins: the framework (DripSim.dll: hooks, body, gravity field, film layers, API) and the rain mode, built against api/DripSimAPI.h
# (DripSimRain.dll). A mode never touches the engine hooks; it only talks to the framework through the API.
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



set(DRIPSIM_TARGETS DripSim DripSimRain)
foreach(target ${DRIPSIM_TARGETS})
    target_compile_features(${target} PRIVATE cxx_std_23)
    target_include_directories(${target} PRIVATE src/core ${CMAKE_CURRENT_SOURCE_DIR}/../api)
    target_precompile_headers(${target} PRIVATE src/core/PCH.h)
endforeach()
target_compile_definitions(DripSim PRIVATE DS_VERSION="${PROJECT_VERSION}")
# SystemInfo.cpp uses the real DXGI headers: without the precompiled header (CommonLib has its own D3D types)
set_source_files_properties(src/core/SystemInfo.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
target_compile_definitions(DripSimRain PRIVATE DSR_VERSION="${PROJECT_VERSION}")

# DripSimPreview: runs a mode without the game and writes pictures (see tools/preview/Preview.cpp). Same film, field and mode sources as the plugins.
add_executable(DripSimPreview
    tools/preview/Preview.cpp
    src/core/Field.cpp
    src/core/Film.cpp
    src/rain/RainMode.cpp
)
target_compile_features(DripSimPreview PRIVATE cxx_std_23)
target_compile_definitions(DripSimPreview PRIVATE DRIPSIM_PREVIEW)
target_include_directories(DripSimPreview PRIVATE src/core src/rain ${CMAKE_CURRENT_SOURCE_DIR}/../api)
target_compile_options(DripSimPreview PRIVATE /FI${CMAKE_CURRENT_SOURCE_DIR}/tools/preview/PreviewShim.h)
