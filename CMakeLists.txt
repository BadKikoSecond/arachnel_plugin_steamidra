cmake_minimum_required(VERSION 3.20)
project(arachnel_plugin_steamidra LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED ARACHNEL_SDK_DIR)
    if(DEFINED ENV{ARACHNEL_SDK_DIR})
        set(ARACHNEL_SDK_DIR "$ENV{ARACHNEL_SDK_DIR}")
    else()
        set(ARACHNEL_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Arachnel")
    endif()
endif()
# Prefer sibling Arachnel checkout used in this workspace layout.
if(NOT EXISTS "${ARACHNEL_SDK_DIR}/cmake/ArachnelPluginSdk.cmake"
   AND EXISTS "D:/Work/Arachnel/cmake/ArachnelPluginSdk.cmake")
    set(ARACHNEL_SDK_DIR "D:/Work/Arachnel")
endif()
cmake_path(ABSOLUTE_PATH ARACHNEL_SDK_DIR NORMALIZE)

find_package(Qt6 REQUIRED COMPONENTS Core Network)

include(${ARACHNEL_SDK_DIR}/cmake/ArachnelPluginSdk.cmake)
arachnel_plugin_sdk_init(${ARACHNEL_SDK_DIR})

# Native Steam CDN downloader (statically linked into the plugin).
if(NOT DEFINED DEPOTKIT_DIR)
    if(DEFINED ENV{DEPOTKIT_DIR})
        set(DEPOTKIT_DIR "$ENV{DEPOTKIT_DIR}")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../depotkit/CMakeLists.txt")
        set(DEPOTKIT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../depotkit")
    elseif(EXISTS "D:/PetWork/depotkit/CMakeLists.txt")
        set(DEPOTKIT_DIR "D:/PetWork/depotkit")
    endif()
endif()
if(NOT DEPOTKIT_DIR OR NOT EXISTS "${DEPOTKIT_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "depotkit not found. Set DEPOTKIT_DIR to the depotkit checkout.")
endif()
cmake_path(ABSOLUTE_PATH DEPOTKIT_DIR NORMALIZE)
set(DEPOTKIT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DEPOTKIT_BUILD_SHARED OFF CACHE BOOL "" FORCE)
add_subdirectory("${DEPOTKIT_DIR}" "${CMAKE_BINARY_DIR}/_deps/depotkit" EXCLUDE_FROM_ALL)

add_library(steamidra_plugin SHARED
    src/plugin_entry.cpp
    src/steamidra_plugin.cpp
    src/http_client.cpp
    src/plugin_settings.cpp
    src/catalog_sync.cpp
    src/online_fix_embed.cpp
    src/install_pipeline.cpp
)

target_include_directories(steamidra_plugin PRIVATE src)
target_link_libraries(steamidra_plugin PRIVATE arachnel_plugin_sdk Qt6::Core Qt6::Network depotkit)

set_target_properties(steamidra_plugin PROPERTIES
    OUTPUT_NAME "steamidra_plugin"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugin-bundle"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugin-bundle"
)
if(MINGW)
    set_target_properties(steamidra_plugin PROPERTIES PREFIX "")
elseif(MSVC)
    foreach(_cfg IN ITEMS Release RelWithDebInfo Debug MinSizeRel)
        set_target_properties(steamidra_plugin PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/plugin-bundle"
            LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/plugin-bundle"
        )
    endforeach()
endif()

set(_PLUGIN_BUNDLE_DIR "${CMAKE_BINARY_DIR}/plugin-bundle")
set(_ARACH "${CMAKE_BINARY_DIR}/dist/steamidra.arach")
set(_ARACH_STAGING "${CMAKE_BINARY_DIR}/_staging_steamidra_arach/steamidra")

if(WIN32)
    set(_TP_OS "windows")
else()
    set(_TP_OS "linux")
endif()
set(_TP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${_TP_OS}")

arachnel_stage_plugin_bundle(steamidra_plugin steamidra "${_PLUGIN_BUNDLE_DIR}")

add_custom_command(TARGET steamidra_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_PLUGIN_BUNDLE_DIR}/worker"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/config"
        "${_PLUGIN_BUNDLE_DIR}/config"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_PLUGIN_BUNDLE_DIR}/third_party"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_PLUGIN_BUNDLE_DIR}/third_party"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/README.md"
        "${_PLUGIN_BUNDLE_DIR}/third_party/README.md"
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${_TP_SRC}
        -DDEST_DIR=${_PLUGIN_BUNDLE_DIR}/third_party
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyThirdParty.cmake"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE.md"
        "${_PLUGIN_BUNDLE_DIR}/NOTICE.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        "${_PLUGIN_BUNDLE_DIR}/LICENSE"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_PLUGIN_BUNDLE_DIR}/online_fix_kit"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_PLUGIN_BUNDLE_DIR}/online_fix_kit"
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/online_fix_kit
        -DDEST_DIR=${_PLUGIN_BUNDLE_DIR}/online_fix_kit
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyOnlineFixKit.cmake"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${CMAKE_BINARY_DIR}/_staging_steamidra_arach"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_ARACH_STAGING}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${_PLUGIN_BUNDLE_DIR}" "${_ARACH_STAGING}"
    COMMENT "Stage steamidra config + OS third_party (${_TP_OS}) — depotkit linked in"
)

arachnel_package_plugin_arach(steamidra_plugin steamidra "${_ARACH_STAGING}" "${_ARACH}")

add_custom_target(steamidra_arach ALL DEPENDS steamidra_plugin)

# Dev smoke tool (catalog + relay + optional Meccha embed)
add_executable(steamidra_smoke
    src/http_client.cpp
    src/plugin_settings.cpp
    src/catalog_sync.cpp
    src/online_fix_embed.cpp
    tools/smoke_cpp_runtime.cpp
)
target_include_directories(steamidra_smoke PRIVATE src)
target_link_libraries(steamidra_smoke PRIVATE arachnel_plugin_sdk Qt6::Core Qt6::Network)
