# Improved CustomMemoryAllocator for Group 1 in CSE 625

## Build and test

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Build and test allocations with canaries and metrics disabled

```sh
cmake -S . -B build -DBUILD_TESTING=ON -DENABLE_CHECKS_METRICS=OFF
cmake --build build
time ./build/test/many_allocations_test 100000000
```

## Global allocation

The global `new` target replaces ordinary `new`, `new[]`, `delete`, and
`delete[]` after the caller initializes the allocator. This includes ordinary
allocations made inside dependencies used by that caller.

## Changelog

- v1.0.0 uses one ordered free list, first fit allocation, and immediate merging
- v2.0.0 searches for the smallest usable block to reduce wasted space
- v3.0.0 groups free blocks by size to reduce search work
- v4.0.0 caches small blocks per thread to avoid repeated global locking
- v5.0.0 merges adjacent blocks only when an allocation needs more space
- v6.0.0 refills caches in batches and creates reusable small block slabs
- v7.0.0 grows busy cache classes while limiting retained memory
- v7.0.1 allows for the disabling of canaries and metrics used in the allocator (and adds a test that allows to check many repeated allocations and deallocations)
