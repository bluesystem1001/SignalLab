cmake_minimum_required(VERSION 3.28)

project(
    SignalLab
    VERSION 0.1.0
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

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
