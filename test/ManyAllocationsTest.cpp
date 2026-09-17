#include "custom_memory/MemoryPool.hpp"
#include <cassert>
#include <cstddef>
#include <iterator>
#include <iostream>
#include <new>
#include <thread>

#define TEST_ALLOCATION_COUNT 1000000

int main(int argc, char* argv[])
{
    int allocationsToDo = TEST_ALLOCATION_COUNT;

    if (argc > 1) 
    {
        allocationsToDo = std::atoi(argv[1]);
    }
    auto& pool = custom_memory::MemoryPool::instance();
    assert(pool.initialize(1024 * 1024));
    
    constexpr std::size_t varied_sizes[] = { 24, 48, 96, 160, 320, 640, 1280 };
    
    //Run a ton of raw allocations of various sizes, and then immediately deallocate.  
    for (int index = 0; index < allocationsToDo; index++) 
    {
        void* foo = pool.allocate(varied_sizes[index % std::size(varied_sizes)]);
        pool.deallocate(foo);
    }
}