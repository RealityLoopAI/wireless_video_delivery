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
    "${CMAKE_SOURCE_DIR}/02_receiver_linux/*.cpp"
    "${CMAKE_SOURCE_DIR}/02_receiver_linux/*.h"
    "${CMAKE_SOURCE_DIR}/02_receiver_linux/*.hpp"
    "${CMAKE_SOURCE_DIR}/03_common_core/*.cpp"
    "${CMAKE_SOURCE_DIR}/03_common_core/*.h"
    "${CMAKE_SOURCE_DIR}/03_common_core/*.hpp"
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

add_compile_definitions(
    GWV3_GIT_COMMIT="${GWV3_GIT_COMMIT}"
    GWV3_GIT_DIRTY=${GWV3_GIT_DIRTY}
    GWV3_SOURCE_HASH="${GWV3_SOURCE_HASH}"
)

option(GWV3_BUILD_SENDER "Build ARM sender program" OFF)
option(GWV3_BUILD_RECEIVER "Build Linux receiver program" ON)

if(GWV3_BUILD_SENDER)
    add_subdirectory(01_sender_linux)
endif()

if(GWV3_BUILD_RECEIVER)
    add_subdirectory(02_receiver_linux)
endif()
