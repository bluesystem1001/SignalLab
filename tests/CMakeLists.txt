add_executable(signallab_tests
    unit/smoke_test.cpp
)

target_compile_features(signallab_tests PRIVATE cxx_std_20)

target_link_libraries(signallab_tests
    PRIVATE
        GTest::gtest_main
        Qt6::Core
        Qt6::Sql
)

signallab_set_compiler_warnings(signallab_tests)

include(GoogleTest)
gtest_discover_tests(signallab_tests)
