cmake_minimum_required(VERSION 3.16)

project(gemini_wireless_video_v3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(GWV3_BUILD_SENDER "Build ARM sender program" OFF)
option(GWV3_BUILD_RECEIVER "Build Linux receiver program" ON)

if(GWV3_BUILD_SENDER)
    add_subdirectory(01_sender_linux)
endif()

if(GWV3_BUILD_RECEIVER)
    add_subdirectory(02_receiver_linux)
endif()
