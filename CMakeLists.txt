cmake_minimum_required(VERSION 3.16)

project(gemini_wireless_video_v3 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE GWV3_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(GWV3_GIT_COMMIT STREQUAL "")
    set(GWV3_GIT_COMMIT "unknown")
endif()
execute_process(
    COMMAND git status --porcelain
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE GWV3_GIT_STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(GWV3_GIT_STATUS STREQUAL "")
    set(GWV3_GIT_DIRTY 0)
else()
    set(GWV3_GIT_DIRTY 1)
endif()

file(GLOB_RECURSE GWV3_VERSION_SOURCE_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/01_sender_linux/*.cpp"
    "${CMAKE_SOURCE_DIR}/01_sender_linux/*.h"
    "${CMAKE_SOURCE_DIR}/01_sender_linux/*.hpp"
    "${CMAKE_SOURCE_DIR}/01_sender_linux/*.inl"
    "${CMAKE_SOURCE_DIR}/02_receiver_linux/*.cpp"
    "${CMAKE_SOURCE_DIR}/02_receiver_linux/*.h"
    "${CMAKE_SOURCE_DIR}/02_receiver_linux/*.hpp"
    "${CMAKE_SOURCE_DIR}/02_receiver_linux/*.inl"
    "${CMAKE_SOURCE_DIR}/03_common_core/*.cpp"
    "${CMAKE_SOURCE_DIR}/03_common_core/*.h"
    "${CMAKE_SOURCE_DIR}/03_common_core/*.hpp"
    "${CMAKE_SOURCE_DIR}/03_common_core/*.inl"
)
foreach(GWV3_VERSION_CMAKE_FILE IN ITEMS
        "${CMAKE_SOURCE_DIR}/CMakeLists.txt"
        "${CMAKE_SOURCE_DIR}/01_sender_linux/CMakeLists.txt"
        "${CMAKE_SOURCE_DIR}/02_receiver_linux/CMakeLists.txt")
    if(EXISTS "${GWV3_VERSION_CMAKE_FILE}")
        list(APPEND GWV3_VERSION_SOURCE_FILES "${GWV3_VERSION_CMAKE_FILE}")
    endif()
endforeach()
list(SORT GWV3_VERSION_SOURCE_FILES)
set(GWV3_SOURCE_HASH_MATERIAL "")
foreach(GWV3_SOURCE_FILE IN LISTS GWV3_VERSION_SOURCE_FILES)
    file(SHA256 "${GWV3_SOURCE_FILE}" GWV3_SOURCE_FILE_HASH)
    file(RELATIVE_PATH GWV3_SOURCE_RELATIVE "${CMAKE_SOURCE_DIR}" "${GWV3_SOURCE_FILE}")
    string(APPEND GWV3_SOURCE_HASH_MATERIAL "${GWV3_SOURCE_RELATIVE}:${GWV3_SOURCE_FILE_HASH}\n")
endforeach()
string(SHA256 GWV3_SOURCE_HASH_FULL "${GWV3_SOURCE_HASH_MATERIAL}")
string(SUBSTRING "${GWV3_SOURCE_HASH_FULL}" 0 16 GWV3_SOURCE_HASH)

function(gwv3_compute_component_hash output_variable)
    set(component_files ${ARGN})
    list(REMOVE_DUPLICATES component_files)
    list(SORT component_files)
    set(component_material "")
    foreach(component_file IN LISTS component_files)
        if(NOT EXISTS "${component_file}")
            continue()
        endif()
        file(SHA256 "${component_file}" component_file_hash)
        file(RELATIVE_PATH component_relative "${CMAKE_SOURCE_DIR}" "${component_file}")
        string(APPEND component_material "${component_relative}:${component_file_hash}\n")
    endforeach()
    string(SHA256 component_hash_full "${component_material}")
    string(SUBSTRING "${component_hash_full}" 0 16 component_hash)
    set(${output_variable} "${component_hash}" PARENT_SCOPE)
endfunction()

file(GLOB_RECURSE GWV3_COMMON_VERSION_SOURCE_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/03_common_core/*.cpp"
    "${CMAKE_SOURCE_DIR}/03_common_core/*.h"
    "${CMAKE_SOURCE_DIR}/03_common_core/*.hpp"
    "${CMAKE_SOURCE_DIR}/03_common_core/*.inl"
)
file(GLOB_RECURSE GWV3_SENDER_VERSION_SOURCE_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/01_sender_linux/*.cpp"
    "${CMAKE_SOURCE_DIR}/01_sender_linux/*.h"
    "${CMAKE_SOURCE_DIR}/01_sender_linux/*.hpp"
    "${CMAKE_SOURCE_DIR}/01_sender_linux/*.inl"
)
file(GLOB_RECURSE GWV3_RECEIVER_VERSION_SOURCE_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/02_receiver_linux/*.cpp"
    "${CMAKE_SOURCE_DIR}/02_receiver_linux/*.h"
    "${CMAKE_SOURCE_DIR}/02_receiver_linux/*.hpp"
    "${CMAKE_SOURCE_DIR}/02_receiver_linux/*.inl"
)
gwv3_compute_component_hash(GWV3_SENDER_SOURCE_HASH
    ${GWV3_COMMON_VERSION_SOURCE_FILES}
    ${GWV3_SENDER_VERSION_SOURCE_FILES}
    "${CMAKE_SOURCE_DIR}/CMakeLists.txt"
    "${CMAKE_SOURCE_DIR}/01_sender_linux/CMakeLists.txt"
)
gwv3_compute_component_hash(GWV3_RECEIVER_SOURCE_HASH
    ${GWV3_COMMON_VERSION_SOURCE_FILES}
    ${GWV3_RECEIVER_VERSION_SOURCE_FILES}
    "${CMAKE_SOURCE_DIR}/CMakeLists.txt"
    "${CMAKE_SOURCE_DIR}/02_receiver_linux/CMakeLists.txt"
)

add_compile_definitions(
    GWV3_GIT_COMMIT="${GWV3_GIT_COMMIT}"
    GWV3_GIT_DIRTY=${GWV3_GIT_DIRTY}
    GWV3_SOURCE_HASH="${GWV3_SOURCE_HASH}"
    GWV3_SENDER_SOURCE_HASH="${GWV3_SENDER_SOURCE_HASH}"
    GWV3_RECEIVER_SOURCE_HASH="${GWV3_RECEIVER_SOURCE_HASH}"
)

option(GWV3_BUILD_SENDER "Build ARM sender program" OFF)
option(GWV3_BUILD_RECEIVER "Build Linux receiver program" ON)

if(GWV3_BUILD_SENDER)
    add_subdirectory(01_sender_linux)
endif()

if(GWV3_BUILD_RECEIVER)
    add_subdirectory(02_receiver_linux)
endif()
