cmake_minimum_required(VERSION 3.28)

project(
    SignalLab
    VERSION 1.0.0
    LANGUAGES CXX
)

set(SIGNALLAB_RELEASE_VERSION "1.0.0-rc1")

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

add_library(signallab_repository INTERFACE
    include/signallab/repository/backup_repository.hpp
    include/signallab/repository/audio_input.hpp
    include/signallab/repository/file_repository.hpp
    include/signallab/repository/script_file_gateway.hpp
    include/signallab/repository/script_repository.hpp
    include/signallab/repository/signal_repository.hpp
    include/signallab/repository/task_repository.hpp
)

target_include_directories(signallab_repository
    INTERFACE
        "${PROJECT_SOURCE_DIR}/include"
)

target_compile_features(signallab_repository INTERFACE cxx_std_20)
target_link_libraries(signallab_repository INTERFACE signallab_core)

add_library(signallab_algorithm STATIC
    include/signallab/algorithm/features.hpp
    include/signallab/algorithm/fft.hpp
    include/signallab/algorithm/fir_filter.hpp
    include/signallab/algorithm/streaming_fir_filter.hpp
    include/signallab/algorithm/peak_detector.hpp
    include/signallab/algorithm/preprocessing.hpp
    include/signallab/algorithm/quality_metrics.hpp
    include/signallab/algorithm/spectrum.hpp
    include/signallab/algorithm/window.hpp
    src/algorithm/features.cpp
    src/algorithm/fft.cpp
    src/algorithm/fir_filter.cpp
    src/algorithm/streaming_fir_filter.cpp
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

add_library(signallab_parser STATIC
    include/signallab/parser/ast.hpp
    include/signallab/parser/diagnostic.hpp
    include/signallab/parser/lexer.hpp
    include/signallab/parser/parser.hpp
    include/signallab/parser/semantic_checker.hpp
    include/signallab/parser/source_location.hpp
    include/signallab/parser/token.hpp
    src/parser/ast.cpp
    src/parser/lexer.cpp
    src/parser/parser.cpp
    src/parser/semantic_checker.cpp
)

target_include_directories(signallab_parser
    PUBLIC
        "${PROJECT_SOURCE_DIR}/include"
)

target_compile_features(signallab_parser PUBLIC cxx_std_20)
target_link_libraries(signallab_parser PUBLIC signallab_core)
signallab_set_compiler_warnings(signallab_parser)

add_library(signallab_task STATIC
    include/signallab/task/scheduler.hpp
    include/signallab/task/task.hpp
    include/signallab/task/task_queue.hpp
    include/signallab/task/thread_pool.hpp
    src/task/scheduler.cpp
    src/task/task_queue.cpp
    src/task/thread_pool.cpp
)

target_include_directories(signallab_task
    PUBLIC
        "${PROJECT_SOURCE_DIR}/include"
)

target_compile_features(signallab_task PUBLIC cxx_std_20)
target_link_libraries(signallab_task PUBLIC signallab_core)
signallab_set_compiler_warnings(signallab_task)

add_library(signallab_io STATIC
    include/signallab/io/backup_settings.hpp
    src/io/backup_settings.cpp
    include/signallab/io/csv_reader.hpp
    include/signallab/io/filesystem_file_repository.hpp
    include/signallab/io/filesystem_script_file_gateway.hpp
    include/signallab/io/hash_service.hpp
    include/signallab/io/import_types.hpp
    include/signallab/io/wav_reader.hpp
    include/signallab/io/wav_writer.hpp
    include/signallab/io/windows_audio_input.hpp
    src/io/csv_reader.cpp
    src/io/filesystem_file_repository.cpp
    src/io/filesystem_script_file_gateway.cpp
    src/io/hash_service.cpp
    src/io/wav_reader.cpp
    src/io/wav_writer.cpp
    src/io/windows_audio_input.cpp
)

target_include_directories(signallab_io
    PUBLIC
        "${PROJECT_SOURCE_DIR}/include"
)

target_compile_features(signallab_io PUBLIC cxx_std_20)
target_link_libraries(signallab_io
    PUBLIC
        signallab_core
        signallab_repository
        Qt6::Core
)
signallab_set_compiler_warnings(signallab_io)

if(WIN32)
    target_link_libraries(signallab_io PRIVATE winmm crypt32)
endif()

add_library(signallab_database STATIC
    include/signallab/database/mariadb_backup_repository.hpp
    include/signallab/database/project_backup_source.hpp
    src/database/mariadb_backup_repository.cpp
    src/database/project_backup_source.cpp
    include/signallab/database/database_connection.hpp
    include/signallab/database/database_executor.hpp
    include/signallab/database/migration_runner.hpp
    include/signallab/database/sqlite_script_repository.hpp
    include/signallab/database/sqlite_signal_repository.hpp
    include/signallab/database/sqlite_task_repository.hpp
    src/database/database_connection.cpp
    src/database/database_executor.cpp
    src/database/migration_runner.cpp
    src/database/sqlite_script_repository.cpp
    src/database/sqlite_signal_repository.cpp
    src/database/sqlite_task_repository.cpp
)

target_include_directories(signallab_database
    PUBLIC
        "${PROJECT_SOURCE_DIR}/include"
)

target_compile_features(signallab_database PUBLIC cxx_std_20)
target_link_libraries(signallab_database
    PUBLIC
        signallab_repository
        Qt6::Core
        Qt6::Sql
    PRIVATE
        MariaDB::Client
)
signallab_set_compiler_warnings(signallab_database)

add_library(signallab_app STATIC
    include/signallab/app/backup_service.hpp
    src/app/backup_service.cpp
    include/signallab/app/audio_capture_service.hpp
    include/signallab/app/analysis_service.hpp
    include/signallab/app/background_analysis_service.hpp
    include/signallab/app/background_file_service.hpp
    include/signallab/app/background_script_service.hpp
    include/signallab/app/display_downsampler.hpp
    include/signallab/app/import_service.hpp
    include/signallab/app/script_runtime/evaluator.hpp
    include/signallab/app/script_runtime/value.hpp
    include/signallab/app/script_service.hpp
    src/app/analysis_service.cpp
    src/app/audio_capture_service.cpp
    src/app/background_analysis_service.cpp
    src/app/background_file_service.cpp
    src/app/background_script_service.cpp
    src/app/display_downsampler.cpp
    src/app/import_service.cpp
    src/app/script_runtime/evaluator.cpp
    src/app/script_service.cpp
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
        signallab_repository
        signallab_parser
        signallab_task
)
signallab_set_compiler_warnings(signallab_app)

add_library(signallab_gui STATIC
    src/gui/database_backup_widget.cpp
    src/gui/database_backup_widget.hpp
    src/gui/audio_capture_widget.cpp
    src/gui/audio_capture_widget.hpp
    src/gui/file_import_dialog.cpp
    src/gui/file_import_dialog.hpp
    src/gui/interactive_chart_view.cpp
    src/gui/interactive_chart_view.hpp
    src/gui/main_window.cpp
    src/gui/main_window.hpp
    src/gui/stage7_models.cpp
    src/gui/stage7_models.hpp
)

target_include_directories(signallab_gui
    PUBLIC
        "${PROJECT_SOURCE_DIR}/src"
)

target_compile_features(signallab_gui PUBLIC cxx_std_20)

target_compile_definitions(signallab_gui
    PUBLIC
        SIGNALLAB_VERSION_STRING="${SIGNALLAB_RELEASE_VERSION}"
)

target_link_libraries(signallab_gui
    PUBLIC
        signallab_app
        signallab_io
        Qt6::Core
        Qt6::Widgets
        Qt6::Charts
)

signallab_set_compiler_warnings(signallab_gui)

qt_add_executable(SignalLab WIN32
    src/gui/main.cpp
)

target_compile_features(SignalLab PRIVATE cxx_std_20)
target_link_libraries(SignalLab
    PRIVATE
        signallab_gui
        signallab_io
        signallab_database
)
signallab_set_compiler_warnings(SignalLab)

add_executable(signallab_fft_benchmark
    benchmarks/fft_benchmark.cpp
)

target_compile_features(signallab_fft_benchmark PRIVATE cxx_std_20)
target_link_libraries(signallab_fft_benchmark PRIVATE signallab_algorithm)
signallab_set_compiler_warnings(signallab_fft_benchmark)

add_executable(signallab_import_benchmark
    benchmarks/import_benchmark.cpp
)

target_compile_features(signallab_import_benchmark PRIVATE cxx_std_20)
target_link_libraries(signallab_import_benchmark PRIVATE signallab_io)
signallab_set_compiler_warnings(signallab_import_benchmark)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
