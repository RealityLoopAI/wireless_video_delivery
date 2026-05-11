find_package(PkgConfig REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs highgui)
find_package(ZLIB REQUIRED)

pkg_check_modules(JSONCPP REQUIRED jsoncpp)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0 gstreamer-app-1.0)

set(ORBBEC_SDK_ROOT
    "${CMAKE_SOURCE_DIR}/11_third_party/orbbec/linux_arm64/OrbbecSDK_C_C++_v1.10.27_20250925_0549823_linux_arm64_release/OrbbecSDK_v1.10.27/SDK"
    CACHE PATH "Orbbec SDK root")

set(ORBBEC_SDK_INCLUDE_DIR "${ORBBEC_SDK_ROOT}/include")
set(ORBBEC_SDK_LIBRARY_DIR "${ORBBEC_SDK_ROOT}/lib")
set(ORBBEC_SDK_CONFIG "${ORBBEC_SDK_ROOT}/config/OrbbecSDKConfig_v1.0.xml")

add_executable(gemini_sender
    src/config.cpp
    src/gst_h264_encoder.cpp
    src/logger.cpp
    src/main.cpp
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
    ZLIB::ZLIB
    pthread
)

target_compile_options(gemini_sender PRIVATE
    ${JSONCPP_CFLAGS_OTHER}
    ${GSTREAMER_CFLAGS_OTHER}
    -Wall
    -Wextra
    -Wpedantic
)

set_target_properties(gemini_sender PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/12_build/bin"
    BUILD_RPATH "${ORBBEC_SDK_LIBRARY_DIR}"
    INSTALL_RPATH "${ORBBEC_SDK_LIBRARY_DIR}"
)

add_custom_command(TARGET gemini_sender POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ORBBEC_SDK_CONFIG}"
            "$<TARGET_FILE_DIR:gemini_sender>/OrbbecSDKConfig_v1.0.xml"
)
