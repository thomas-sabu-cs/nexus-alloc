add_executable(nexusalloc_bench
    benchmark_allocators.cpp
)

target_link_libraries(nexusalloc_bench
    PRIVATE
        nexusalloc
)

target_compile_features(nexusalloc_bench PRIVATE cxx_std_17)

