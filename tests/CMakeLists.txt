add_executable(signallab_tests
    unit/core/signal_generator_test.cpp
    unit/core/signal_test.cpp
)

target_compile_features(signallab_tests PRIVATE cxx_std_20)

target_link_libraries(signallab_tests
    PRIVATE
        GTest::gtest_main
        signallab_core
)

signallab_set_compiler_warnings(signallab_tests)

include(GoogleTest)
gtest_discover_tests(signallab_tests)

add_executable(signallab_algorithm_tests
    unit/algorithm/features_test.cpp
    unit/algorithm/fft_test.cpp
    unit/algorithm/fir_filter_test.cpp
    unit/algorithm/peak_detector_test.cpp
    unit/algorithm/preprocessing_test.cpp
    unit/algorithm/quality_metrics_test.cpp
    unit/algorithm/spectrum_test.cpp
    unit/algorithm/window_test.cpp
)

target_compile_features(signallab_algorithm_tests PRIVATE cxx_std_20)

target_link_libraries(signallab_algorithm_tests
    PRIVATE
        GTest::gtest_main
        signallab_algorithm
)

signallab_set_compiler_warnings(signallab_algorithm_tests)
gtest_discover_tests(signallab_algorithm_tests)

add_executable(signallab_io_tests
    unit/database/database_test_main.cpp
    unit/app/analysis_service_test.cpp
    unit/app/display_downsampler_test.cpp
    unit/app/import_service_test.cpp
    unit/io/csv_reader_test.cpp
    unit/io/wav_reader_test.cpp
)

target_compile_features(signallab_io_tests PRIVATE cxx_std_20)

target_link_libraries(signallab_io_tests
    PRIVATE
        GTest::gtest
        signallab_app
        signallab_database
        signallab_io
)

target_compile_definitions(signallab_io_tests
    PRIVATE
        SIGNALLAB_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

signallab_set_compiler_warnings(signallab_io_tests)
gtest_discover_tests(signallab_io_tests)

add_executable(signallab_database_tests
    unit/database/database_test_main.cpp
    unit/database/migration_runner_test.cpp
    unit/database/sqlite_signal_repository_test.cpp
    unit/database/sqlite_task_repository_test.cpp
)

target_compile_features(signallab_database_tests PRIVATE cxx_std_20)

target_compile_definitions(signallab_database_tests
    PRIVATE
        SIGNALLAB_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

target_link_libraries(signallab_database_tests
    PRIVATE
        GTest::gtest
        signallab_database
)

signallab_set_compiler_warnings(signallab_database_tests)
gtest_discover_tests(signallab_database_tests)

add_executable(signallab_smoke_tests
    unit/smoke_test.cpp
)

target_compile_features(signallab_smoke_tests PRIVATE cxx_std_20)

target_link_libraries(signallab_smoke_tests
    PRIVATE
        GTest::gtest_main
        Qt6::Core
        Qt6::Sql
)

signallab_set_compiler_warnings(signallab_smoke_tests)
gtest_discover_tests(signallab_smoke_tests)
