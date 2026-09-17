add_executable(custom_memory_allocator_tests MemoryPoolTests.cpp)
target_link_libraries(
    custom_memory_allocator_tests
    PRIVATE CustomMemoryAllocator::core
)

add_test(
    NAME custom_memory_allocator_tests
    COMMAND custom_memory_allocator_tests
)

add_executable(global_new_tests GlobalNewTests.cpp)
target_link_libraries(
    global_new_tests
    PRIVATE CustomMemoryAllocator::global_new
)

add_test(NAME global_new_tests COMMAND global_new_tests)

option(ENABLE_CHECKS_METRICS "Enable the safety canaries for the allocaator, and additional metrics." ON)

add_executable(many_allocations_test ManyAllocationsTest.cpp)
target_link_libraries(
    many_allocations_test
    PRIVATE CustomMemoryAllocator::core
)

if(ENABLE_CHECKS_METRICS)
    target_compile_definitions(many_allocations_test PRIVATE ENABLE_CHECKS_METRICS)
endif()

add_test(
    NAME many_allocations_test
    COMMAND many_allocations_test
)
