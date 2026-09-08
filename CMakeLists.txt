cmake_minimum_required(VERSION 3.28)

project(
    SignalLab
    VERSION 0.3.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep generated programs and libraries in predictable folders.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

include(CTest)
include(cmake/Dependencies.cmake)
include(cmake/CompilerWarnings.cmake)

add_library(signallab_core STATIC
    include/signallab/core/error.hpp
    include/signallab/core/result.hpp
    include/signallab/core/signal.hpp
    include/signallab/core/signal_generator.hpp
    src/core/signal.cpp
    src/core/signal_generator.cpp
)

target_include_directories(signallab_core
    PUBLIC
        "${PROJECT_SOURCE_DIR}/include"
)

target_compile_features(signallab_core PUBLIC cxx_std_20)
signallab_set_compiler_warnings(signallab_core)

add_library(signallab_algorithm STATIC
    include/signallab/algorithm/features.hpp
    include/signallab/algorithm/fft.hpp
    include/signallab/algorithm/fir_filter.hpp
    include/signallab/algorithm/peak_detector.hpp
    include/signallab/algorithm/preprocessing.hpp
    include/signallab/algorithm/quality_metrics.hpp
    include/signallab/algorithm/spectrum.hpp
    include/signallab/algorithm/window.hpp
    src/algorithm/features.cpp
    src/algorithm/fft.cpp
    src/algorithm/fir_filter.cpp
    src/algorithm/peak_detector.cpp
    src/algorithm/preprocessing.cpp
    src/algorithm/quality_metrics.cpp
    src/algorithm/spectrum.cpp
    src/algorithm/window.cpp
)

target_include_directories(signallab_algorithm
    PUBLIC
        "${PROJECT_SOURCE_DIR}/include"
)

target_compile_features(signallab_algorithm PUBLIC cxx_std_20)
target_link_libraries(signallab_algorithm PUBLIC signallab_core)
signallab_set_compiler_warnings(signallab_algorithm)

add_library(signallab_io STATIC
    include/signallab/io/csv_reader.hpp
    include/signallab/io/hash_service.hpp
    include/signallab/io/import_types.hpp
    include/signallab/io/wav_reader.hpp
    src/io/csv_reader.cpp
    src/io/hash_service.cpp
    src/io/wav_reader.cpp
)

target_include_directories(signallab_io
    PUBLIC
        "${PROJECT_SOURCE_DIR}/include"
)

target_compile_features(signallab_io PUBLIC cxx_std_20)
target_link_libraries(signallab_io PUBLIC signallab_core Qt6::Core)
signallab_set_compiler_warnings(signallab_io)

add_library(signallab_app STATIC
    include/signallab/app/analysis_service.hpp
    include/signallab/app/display_downsampler.hpp
    include/signallab/app/import_service.hpp
    src/app/analysis_service.cpp
    src/app/display_downsampler.cpp
    src/app/import_service.cpp
)

target_include_directories(signallab_app
    PUBLIC
        "${PROJECT_SOURCE_DIR}/include"
)

target_compile_features(signallab_app PUBLIC cxx_std_20)
target_link_libraries(signallab_app
    PUBLIC
        signallab_core
        signallab_algorithm
    PRIVATE
        signallab_io
)
signallab_set_compiler_warnings(signallab_app)

add_library(signallab_gui STATIC
    src/gui/main_window.cpp
    src/gui/main_window.hpp
)

target_include_directories(signallab_gui
    PUBLIC
        "${PROJECT_SOURCE_DIR}/src"
)

target_compile_features(signallab_gui PUBLIC cxx_std_20)

target_compile_definitions(signallab_gui
    PUBLIC
        SIGNALLAB_VERSION_STRING="${PROJECT_VERSION}"
)

target_link_libraries(signallab_gui
    PUBLIC
        signallab_app
        Qt6::Core
        Qt6::Widgets
        Qt6::Charts
)

signallab_set_compiler_warnings(signallab_gui)

qt_add_executable(SignalLab WIN32
    src/gui/main.cpp
)

target_compile_features(SignalLab PRIVATE cxx_std_20)
target_link_libraries(SignalLab PRIVATE signallab_gui)
signallab_set_compiler_warnings(SignalLab)

add_executable(signallab_fft_benchmark
    benchmarks/fft_benchmark.cpp
)

target_compile_features(signallab_fft_benchmark PRIVATE cxx_std_20)
target_link_libraries(signallab_fft_benchmark PRIVATE signallab_algorithm)
signallab_set_compiler_warnings(signallab_fft_benchmark)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
