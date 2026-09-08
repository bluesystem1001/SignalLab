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

add_executable(signallab_io_tests
    unit/app/display_downsampler_test.cpp
    unit/app/import_service_test.cpp
    unit/io/csv_reader_test.cpp
    unit/io/wav_reader_test.cpp
)

target_compile_features(signallab_io_tests PRIVATE cxx_std_20)

target_link_libraries(signallab_io_tests
    PRIVATE
        GTest::gtest_main
        signallab_app
        signallab_io
)

signallab_set_compiler_warnings(signallab_io_tests)
gtest_discover_tests(signallab_io_tests)

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
