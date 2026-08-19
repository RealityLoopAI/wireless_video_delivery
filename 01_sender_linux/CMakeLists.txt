find_package(PkgConfig REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs highgui)
find_package(JPEG REQUIRED)
find_package(ZLIB REQUIRED)

pkg_check_modules(JSONCPP REQUIRED jsoncpp)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)

set(_ORBBEC_SDK_V2_ROOT
    "${CMAKE_SOURCE_DIR}/11_third_party/orbbec/linux_arm64/OrbbecSDK_v2.8.6")
set(_ORBBEC_SDK_V1_ROOT
    "${CMAKE_SOURCE_DIR}/11_third_party/orbbec/linux_arm64/OrbbecSDK_C_C++_v1.10.27_20250925_0549823_linux_arm64_release/OrbbecSDK_v1.10.27/SDK")
if(EXISTS "${_ORBBEC_SDK_V2_ROOT}/lib/libOrbbecSDK.so")
    set(_ORBBEC_SDK_DEFAULT_ROOT "${_ORBBEC_SDK_V2_ROOT}")
else()
    set(_ORBBEC_SDK_DEFAULT_ROOT "${_ORBBEC_SDK_V1_ROOT}")
endif()
if(DEFINED ENV{ORBBEC_SDK_ROOT} AND NOT "$ENV{ORBBEC_SDK_ROOT}" STREQUAL "")
    set(_ORBBEC_SDK_DEFAULT_ROOT "$ENV{ORBBEC_SDK_ROOT}")
endif()

set(ORBBEC_SDK_ROOT "${_ORBBEC_SDK_DEFAULT_ROOT}" CACHE PATH "Orbbec SDK root")

set(ORBBEC_SDK_INCLUDE_DIR "${ORBBEC_SDK_ROOT}/include")
set(ORBBEC_SDK_LIBRARY_DIR "${ORBBEC_SDK_ROOT}/lib")

include(CheckCXXSourceCompiles)
set(_GWV3_REQUIRED_INCLUDES_SAVED "${CMAKE_REQUIRED_INCLUDES}")
set(CMAKE_REQUIRED_INCLUDES "${ORBBEC_SDK_INCLUDE_DIR}")
unset(GWV3_ORBBEC_HAS_COLOR_AE_MAX_GAIN CACHE)
unset(GWV3_ORBBEC_HAS_COLOR_MAXIMAL_GAIN CACHE)
check_cxx_source_compiles(
    "#include <libobsensor/ObSensor.hpp>\nint main() { return static_cast<int>(OB_PROP_COLOR_AE_MAX_GAIN_INT); }"
    GWV3_ORBBEC_HAS_COLOR_AE_MAX_GAIN)
check_cxx_source_compiles(
    "#include <libobsensor/ObSensor.hpp>\nint main() { return static_cast<int>(OB_PROP_COLOR_MAXIMAL_GAIN_INT); }"
    GWV3_ORBBEC_HAS_COLOR_MAXIMAL_GAIN)
set(CMAKE_REQUIRED_INCLUDES "${_GWV3_REQUIRED_INCLUDES_SAVED}")
if(GWV3_ORBBEC_HAS_COLOR_AE_MAX_GAIN)
    set(GWV3_ORBBEC_MAX_GAIN_PROPERTY OB_PROP_COLOR_AE_MAX_GAIN_INT)
elseif(GWV3_ORBBEC_HAS_COLOR_MAXIMAL_GAIN)
    set(GWV3_ORBBEC_MAX_GAIN_PROPERTY OB_PROP_COLOR_MAXIMAL_GAIN_INT)
else()
    message(FATAL_ERROR "Orbbec SDK does not expose a supported color AE max gain property")
endif()

set(ORBBEC_SDK_CONFIG "")
foreach(_ORBBEC_SDK_CONFIG_CANDIDATE
        "${CMAKE_SOURCE_DIR}/OrbbecSDKConfig_v1.0.xml"
        "${ORBBEC_SDK_ROOT}/config/OrbbecSDKConfig_v1.0.xml"
        "${ORBBEC_SDK_ROOT}/lib/OrbbecSDKConfig.xml"
        "${ORBBEC_SDK_ROOT}/OrbbecSDKConfig.xml")
    if(EXISTS "${_ORBBEC_SDK_CONFIG_CANDIDATE}")
        set(ORBBEC_SDK_CONFIG "${_ORBBEC_SDK_CONFIG_CANDIDATE}")
        get_filename_component(ORBBEC_SDK_CONFIG_NAME "${ORBBEC_SDK_CONFIG}" NAME)
        break()
    endif()
endforeach()

add_executable(gemini_sender
    src/adaptive_exposure_controller.cpp
    src/clock_sync_client.cpp
    src/config.cpp
    src/gst_h264_encoder.cpp
    src/logger.cpp
    src/main.cpp
    src/network_utils.cpp
    src/transport.cpp
)

target_include_directories(gemini_sender PRIVATE
    include
    "${CMAKE_SOURCE_DIR}/03_common_core/include"
    "${ORBBEC_SDK_INCLUDE_DIR}"
    ${JSONCPP_INCLUDE_DIRS}
    ${GSTREAMER_INCLUDE_DIRS}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_directories(gemini_sender PRIVATE
    "${ORBBEC_SDK_LIBRARY_DIR}"
    ${JSONCPP_LIBRARY_DIRS}
    ${GSTREAMER_LIBRARY_DIRS}
)

target_link_libraries(gemini_sender PRIVATE
    OrbbecSDK
    ${JSONCPP_LIBRARIES}
    ${GSTREAMER_LIBRARIES}
    ${OpenCV_LIBS}
    JPEG::JPEG
    ZLIB::ZLIB
    ${CMAKE_DL_LIBS}
    pthread
)

target_compile_options(gemini_sender PRIVATE
    ${JSONCPP_CFLAGS_OTHER}
    ${GSTREAMER_CFLAGS_OTHER}
    -Wall
    -Wextra
    -Wpedantic
)

target_compile_definitions(gemini_sender PRIVATE
    GWV3_ORBBEC_MAX_GAIN_PROPERTY=${GWV3_ORBBEC_MAX_GAIN_PROPERTY}
)

set_target_properties(gemini_sender PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    BUILD_RPATH "${ORBBEC_SDK_LIBRARY_DIR}"
    INSTALL_RPATH "${ORBBEC_SDK_LIBRARY_DIR}"
)

if(EXISTS "${ORBBEC_SDK_CONFIG}")
    add_custom_command(TARGET gemini_sender POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ORBBEC_SDK_CONFIG}"
                "$<TARGET_FILE_DIR:gemini_sender>/${ORBBEC_SDK_CONFIG_NAME}"
    )
endif()

add_executable(orbbec_depth_probe
    "${CMAKE_SOURCE_DIR}/05_tools/orbbec_depth_probe.cpp"
)

target_include_directories(orbbec_depth_probe PRIVATE
    "${ORBBEC_SDK_INCLUDE_DIR}"
)

target_link_directories(orbbec_depth_probe PRIVATE
    "${ORBBEC_SDK_LIBRARY_DIR}"
)

target_link_libraries(orbbec_depth_probe PRIVATE
    OrbbecSDK
)

target_compile_options(orbbec_depth_probe PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_compile_definitions(orbbec_depth_probe PRIVATE
    GWV3_ORBBEC_MAX_GAIN_PROPERTY=${GWV3_ORBBEC_MAX_GAIN_PROPERTY}
)

set_target_properties(orbbec_depth_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    BUILD_RPATH "${ORBBEC_SDK_LIBRARY_DIR}"
    INSTALL_RPATH "${ORBBEC_SDK_LIBRARY_DIR}"
)

add_executable(orbbec_fps_probe
    "${CMAKE_SOURCE_DIR}/05_tools/orbbec_fps_probe.cpp"
)

target_include_directories(orbbec_fps_probe PRIVATE
    "${ORBBEC_SDK_INCLUDE_DIR}"
)

target_link_directories(orbbec_fps_probe PRIVATE
    "${ORBBEC_SDK_LIBRARY_DIR}"
)

target_link_libraries(orbbec_fps_probe PRIVATE
    OrbbecSDK
)

target_compile_options(orbbec_fps_probe PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

set_target_properties(orbbec_fps_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    BUILD_RPATH "${ORBBEC_SDK_LIBRARY_DIR}"
    INSTALL_RPATH "${ORBBEC_SDK_LIBRARY_DIR}"
)

add_executable(depth_compression_bench
    "${CMAKE_SOURCE_DIR}/05_tools/depth_compression_bench.cpp"
)

target_link_libraries(depth_compression_bench PRIVATE
    ZLIB::ZLIB
    ${CMAKE_DL_LIBS}
    pthread
)

target_compile_options(depth_compression_bench PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

set_target_properties(depth_compression_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    add_executable(adaptive_exposure_controller_test
        "${CMAKE_SOURCE_DIR}/10_tests/test_adaptive_exposure_controller.cpp"
        src/adaptive_exposure_controller.cpp
    )
    target_include_directories(adaptive_exposure_controller_test PRIVATE include)
    target_compile_options(adaptive_exposure_controller_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME adaptive_exposure_controller COMMAND adaptive_exposure_controller_test)

    add_executable(rgb_transport_recovery_test
        "${CMAKE_SOURCE_DIR}/10_tests/test_rgb_transport_recovery.cpp"
    )
    target_include_directories(rgb_transport_recovery_test PRIVATE include)
    target_compile_options(rgb_transport_recovery_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME rgb_transport_recovery COMMAND rgb_transport_recovery_test)

    add_executable(scheduled_keyframe_test
        "${CMAKE_SOURCE_DIR}/10_tests/test_scheduled_keyframe.cpp"
    )
    target_include_directories(scheduled_keyframe_test PRIVATE include)
    target_compile_options(scheduled_keyframe_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME scheduled_keyframe COMMAND scheduled_keyframe_test)

    add_test(
        NAME sender_config_validation
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/10_tests/test_sender_config.py"
                --sender "$<TARGET_FILE:gemini_sender>"
                --source-root "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(sender_config_validation PROPERTIES TIMEOUT 120)
endif()
