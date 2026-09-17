cmake_minimum_required(VERSION 3.16)

project(
    CustomMemoryAllocator
    VERSION 7.0.0
    LANGUAGES CXX
)

# Allows us to use pre-processor macros to enable/disable canaries and stat bookkeeping
option(ENABLE_CHECKS_METRICS "Enable the safety canaries for the allocaator, and additional metrics." ON)

include(GNUInstallDirs)
find_package(Threads REQUIRED)

if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
endif()

add_library(custom_memory_allocator src/MemoryPool.cpp)
add_library(CustomMemoryAllocator::core ALIAS custom_memory_allocator)
set_target_properties(custom_memory_allocator PROPERTIES EXPORT_NAME core)

add_library(custom_memory_allocator_global_new src/GlobalNew.cpp)
add_library(
    CustomMemoryAllocator::global_new
    ALIAS custom_memory_allocator_global_new
)
set_target_properties(
    custom_memory_allocator_global_new
    PROPERTIES EXPORT_NAME global_new
)

target_compile_features(custom_memory_allocator PUBLIC cxx_std_17)
target_link_libraries(custom_memory_allocator PUBLIC Threads::Threads)
target_include_directories(
    custom_memory_allocator
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(custom_memory_allocator_global_new PUBLIC cxx_std_17)
target_include_directories(
    custom_memory_allocator_global_new
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(
    custom_memory_allocator_global_new
    PUBLIC CustomMemoryAllocator::core
)

if(ENABLE_CHECKS_METRICS)
    target_compile_definitions(custom_memory_allocator PRIVATE ENABLE_CHECKS_METRICS)
    target_compile_definitions(custom_memory_allocator_global_new PRIVATE ENABLE_CHECKS_METRICS)
endif()

if(BUILD_TESTING AND PROJECT_IS_TOP_LEVEL)
    add_subdirectory(test)
endif()

install(
    TARGETS custom_memory_allocator custom_memory_allocator_global_new
    EXPORT CustomMemoryAllocatorTargets
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(
    EXPORT CustomMemoryAllocatorTargets
    FILE CustomMemoryAllocatorTargets.cmake
    NAMESPACE CustomMemoryAllocator::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CustomMemoryAllocator
)
