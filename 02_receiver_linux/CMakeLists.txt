add_executable(gemini_receiver
    src/main.cpp
)

find_package(ZLIB REQUIRED)

target_include_directories(gemini_receiver PRIVATE
    "${CMAKE_SOURCE_DIR}/03_common_core/include"
)

target_compile_features(gemini_receiver PRIVATE cxx_std_17)

target_compile_options(gemini_receiver PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_link_libraries(gemini_receiver PRIVATE
    ZLIB::ZLIB
    pthread
)

set_target_properties(gemini_receiver PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/12_build/bin"
)
