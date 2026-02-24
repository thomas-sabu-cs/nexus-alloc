add_executable(nexusalloc_tests
    test_memory_allocator.cpp
)

target_link_libraries(nexusalloc_tests
    PRIVATE
        nexusalloc
        GTest::gtest_main
)

target_compile_features(nexusalloc_tests PRIVATE cxx_std_17)

include(GoogleTest)
gtest_discover_tests(nexusalloc_tests)

