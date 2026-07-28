add_executable(gemini_receiver
    src/clock_sync_manager.cpp
    src/main.cpp
)

find_package(ZLIB REQUIRED)
find_path(JSONCPP_INCLUDE_DIR NAMES json/json.h PATH_SUFFIXES jsoncpp)
find_library(JSONCPP_LIBRARY NAMES jsoncpp)
if(NOT JSONCPP_INCLUDE_DIR OR NOT JSONCPP_LIBRARY)
    message(FATAL_ERROR "jsoncpp headers and library are required")
endif()

target_include_directories(gemini_receiver PRIVATE
    include
    "${CMAKE_SOURCE_DIR}/03_common_core/include"
    ${JSONCPP_INCLUDE_DIR}
)

target_compile_features(gemini_receiver PRIVATE cxx_std_17)

target_compile_options(gemini_receiver PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_link_libraries(gemini_receiver PRIVATE
    ZLIB::ZLIB
    ${JSONCPP_LIBRARY}
    ${CMAKE_DL_LIBS}
    pthread
)

set_target_properties(gemini_receiver PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(
        NAME receiver_hardening_integration
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/10_tests/test_receiver_hardening.py
                --receiver $<TARGET_FILE:gemini_receiver>
                --source-root ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(receiver_hardening_integration PROPERTIES TIMEOUT 90)
    add_test(
        NAME sync_input_guard_unit
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/10_tests/test_sync_input_guard.py
    )
    add_test(
        NAME recording_uploader_fault_injection
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/10_tests/test_recording_uploader.py
                --python ${Python3_EXECUTABLE}
                --uploader ${CMAKE_SOURCE_DIR}/05_tools/recording_uploader.py
    )
    set_tests_properties(recording_uploader_fault_injection PROPERTIES TIMEOUT 90)
    add_test(
        NAME receiver_staging_pipeline_integration
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/10_tests/test_receiver_staging_pipeline.py
                --receiver $<TARGET_FILE:gemini_receiver>
                --python ${Python3_EXECUTABLE}
                --uploader ${CMAKE_SOURCE_DIR}/05_tools/recording_uploader.py
    )
    set_tests_properties(receiver_staging_pipeline_integration PROPERTIES TIMEOUT 90)
    add_test(
        NAME photo_uploader_fault_injection
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/10_tests/test_photo_uploader.py
                --uploader ${CMAKE_SOURCE_DIR}/05_tools/photo_uploader.py
    )
    set_tests_properties(photo_uploader_fault_injection PROPERTIES TIMEOUT 30)
    add_test(
        NAME voice_photo_pipeline_integration
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/10_tests/test_voice_photo_pipeline.py
                --receiver $<TARGET_FILE:gemini_receiver>
    )
    set_tests_properties(voice_photo_pipeline_integration PROPERTIES TIMEOUT 30)
endif()
