#include "custom_memory/MemoryPool.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

#include <sys/mman.h>
#include <unistd.h>

namespace custom_memory {
namespace {

constexpr std::uint64_t live_block_magic = 0x434D414C4C4F4341ULL;
constexpr std::uint64_t free_block_magic = 0x46524545424C4F43ULL;
constexpr std::uint64_t cached_block_magic = 0x434143484544424CULL;
constexpr std::uint64_t reserved_block_magic = 0x5245534552564544ULL;
constexpr std::uint64_t retired_block_magic = 0x5245544952454442ULL;

bool isPowerOfTwo(std::size_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

bool addWouldOverflow(std::uintptr_t value, std::size_t increment) noexcept {
    return increment > std::numeric_limits<std::uintptr_t>::max() - value;
}

std::uintptr_t alignUp(std::uintptr_t value, std::size_t alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

std::size_t alignDown(std::size_t value, std::size_t alignment) noexcept {
    return value & ~(alignment - 1);
}

void storeCanary(void* address, std::uint32_t value) noexcept {
    std::memcpy(address, &value, sizeof(value));
}

std::uint32_t loadCanary(const void* address) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, address, sizeof(value));
    return value;
}

}

namespace detail {

struct alignas(std::max_align_t) BlockHeader {
    std::size_t total_size{0};
    std::size_t requested_size{0};
    void* user_pointer{nullptr};
    BlockHeader* previous_free{nullptr};
    BlockHeader* next_free{nullptr};
    std::atomic<std::uint64_t> magic{free_block_magic};
};

struct ThreadCache {
    // Fixed arrays avoid recursively allocating memory inside the allocator.
    MemoryPool* owner{nullptr};
    std::array<BlockHeader*, MemoryPool::small_bin_count> bins{};
    std::array<std::size_t, MemoryPool::small_bin_count> counts{};
    std::array<std::size_t, MemoryPool::small_bin_count> targets{};
    std::array<std::size_t, MemoryPool::small_bin_count> refill_events{};
    std::size_t cached_bytes{0};

    ~ThreadCache();
};

thread_local ThreadCache current_thread_cache;

ThreadCache::~ThreadCache() {
    if (owner != nullptr) {
        owner->releaseThreadCache(*this);
    }
}

}

namespace {

    #ifndef ENABLE_CHECKS_METRICS
        constexpr std::size_t allocation_overhead =
            sizeof(detail::BlockHeader) + sizeof(detail::BlockHeader*);
    #endif    
    #ifdef ENABLE_CHECKS_METRICS
        constexpr std::size_t allocation_overhead =
            sizeof(detail::BlockHeader) + sizeof(detail::BlockHeader*) +
            2 * sizeof(std::uint32_t);
    #endif

constexpr std::size_t minimum_remainder =
    allocation_overhead + alignof(std::max_align_t);

struct Layout {
    std::byte* user{nullptr};
    std::size_t used_size{0};
};

bool minimumBlockSize(std::size_t bytes, std::size_t& result) noexcept {
    if (bytes > std::numeric_limits<std::size_t>::max() -
            allocation_overhead) {
        return false;
    }
    result = allocation_overhead + bytes;
    return true;
}

Layout calculateLayout(
    detail::BlockHeader* block,
    std::size_t bytes,
    std::size_t alignment
) noexcept {
    const auto block_address = reinterpret_cast<std::uintptr_t>(block);
    auto cursor = block_address + sizeof(detail::BlockHeader);
    
    #ifndef ENABLE_CHECKS_METRICS
        size_t canarySize = 0;
    #endif

    #ifdef ENABLE_CHECKS_METRICS
        size_t canarySize = sizeof(std::uint32_t);
    #endif

    if (addWouldOverflow(
            cursor,
            sizeof(detail::BlockHeader*) 
                + canarySize
        )) {
        return {};
    }
    cursor += sizeof(detail::BlockHeader*) + canarySize;

    if (addWouldOverflow(cursor, alignment - 1)) {
        return {};
    }
    const auto user_address = alignUp(cursor, alignment);

    if (addWouldOverflow(user_address, bytes) ||
        addWouldOverflow(user_address + bytes, canarySize)) {
        return {};
    }
    const auto end_address = user_address + bytes + canarySize;

    if (addWouldOverflow(end_address, alignof(detail::BlockHeader) - 1)) {
        return {};
    }
    const auto aligned_end = alignUp(
        end_address,
        alignof(detail::BlockHeader)
    );
    const auto used_size = aligned_end - block_address;

    if (used_size > block->total_size) {
        return {};
    }
    return {
        reinterpret_cast<std::byte*>(user_address),
        static_cast<std::size_t>(used_size)
    };
}

}

MemoryPool& MemoryPool::instance() noexcept {
    alignas(MemoryPool) static std::byte storage[sizeof(MemoryPool)];
    static MemoryPool* pool =
        ::new (static_cast<void*>(storage)) MemoryPool{};
    return *pool;
}

bool MemoryPool::initialize(std::size_t bytes) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (region_ != nullptr) {
        return false;
    }

    const long page_size_result = ::sysconf(_SC_PAGESIZE);
    if (page_size_result <= 0) {
        return false;
    }
    const auto page_size = static_cast<std::size_t>(page_size_result);
    if (!isPowerOfTwo(page_size)) {
        return false;
    }
    if (bytes > std::numeric_limits<std::size_t>::max() - page_size + 1) {
        return false;
    }
    const auto mapped_size = alignDown(bytes + page_size - 1, page_size);
    if (mapped_size < minimum_remainder) {
        return false;
    }

    void* region = ::mmap(
        nullptr,
        mapped_size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0
    );
    if (region == MAP_FAILED) {
        return false;
    }

#ifdef MADV_HUGEPAGE
    // Linux may back suitable portions of the mapping with transparent huge
    // pages. Other platforms simply compile without this optional advice.
    static_cast<void>(::madvise(region, mapped_size, MADV_HUGEPAGE));
#endif

    region_ = region;
    region_size_ = mapped_size;
    page_size_ = page_size;
    active_thread_caches_ = 0;
    small_bins_.fill(nullptr);
    occupied_small_bins_.fill(0);
    large_bins_.fill(nullptr);
    occupied_large_bins_ = 0;

    auto* initial_block = ::new (region_) detail::BlockHeader{};
    initial_block->total_size = mapped_size;
    insertFreeBlock(initial_block);

#ifdef ENABLE_CHECKS_METRICS
    capacity_bytes_.store(mapped_size, std::memory_order_relaxed);
    allocated_bytes_.store(0, std::memory_order_relaxed);
    live_allocations_.store(0, std::memory_order_relaxed);
    total_allocations_.store(0, std::memory_order_relaxed);
    total_deallocations_.store(0, std::memory_order_relaxed);
#endif

    const auto begin = reinterpret_cast<std::uintptr_t>(region_);
    region_begin_.store(begin, std::memory_order_release);
    region_end_.store(begin + mapped_size, std::memory_order_release);
    return true;
}

bool MemoryPool::shutdown() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);

    auto& cache = detail::current_thread_cache;
    if (cache.owner == this) {
        flushThreadCacheUnlocked(cache);
        cache.owner = nullptr;
        --active_thread_caches_;
    }

    if (region_ == nullptr ||
        live_allocations_.load(std::memory_order_acquire) != 0 ||
        active_thread_caches_ != 0) {
        return false;
    }

    void* region = region_;
    const std::size_t region_size = region_size_;
    region_begin_.store(0, std::memory_order_release);
    region_end_.store(0, std::memory_order_release);

    if (::munmap(region, region_size) != 0) {
        const auto begin = reinterpret_cast<std::uintptr_t>(region);
        region_begin_.store(begin, std::memory_order_release);
        region_end_.store(begin + region_size, std::memory_order_release);
        return false;
    }

    region_ = nullptr;
    region_size_ = 0;
    page_size_ = 0;
    small_bins_.fill(nullptr);
    occupied_small_bins_.fill(0);
    large_bins_.fill(nullptr);
    occupied_large_bins_ = 0;
    capacity_bytes_.store(0, std::memory_order_relaxed);
    allocated_bytes_.store(0, std::memory_order_relaxed);
    live_allocations_.store(0, std::memory_order_relaxed);
    total_allocations_.store(0, std::memory_order_relaxed);
    total_deallocations_.store(0, std::memory_order_relaxed);
    return true;
}

bool MemoryPool::isInitialized() const noexcept {
    return region_begin_.load(std::memory_order_acquire) != 0;
}

bool MemoryPool::owns(const void* pointer) const noexcept {
    if (pointer == nullptr) {
        return false;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    const auto begin = region_begin_.load(std::memory_order_acquire);
    const auto end = region_end_.load(std::memory_order_acquire);
    return begin != 0 && address >= begin && address < end;
}

bool MemoryPool::ownsUnlocked(const void* pointer) const noexcept {
    if (region_ == nullptr || pointer == nullptr) {
        return false;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    const auto begin = reinterpret_cast<std::uintptr_t>(region_);
    return address >= begin && address < begin + region_size_;
}

Statistics MemoryPool::statistics() const noexcept {
#ifdef ENABLE_CHECKS_METRICS
    return {
        capacity_bytes_.load(std::memory_order_relaxed),
        allocated_bytes_.load(std::memory_order_relaxed),
        live_allocations_.load(std::memory_order_relaxed),
        total_allocations_.load(std::memory_order_relaxed),
        total_deallocations_.load(std::memory_order_relaxed)
    };
#else // No stats to be returned.
    return {0,0,0,0,0};
#endif
}

std::size_t MemoryPool::smallBinIndex(std::size_t block_size) noexcept {
    if (block_size == 0) {
        return 0;
    }
    return std::min(
        (block_size - 1) / small_bin_quantum,
        small_bin_count - 1
    );
}

std::size_t MemoryPool::largeBinIndex(std::size_t block_size) noexcept {
    std::size_t index = 0;
    while (block_size > 1 && index + 1 < large_bin_count) {
        block_size >>= 1;
        ++index;
    }
    return index;
}

std::size_t MemoryPool::effectiveAlignment(
    std::size_t bytes,
    std::size_t alignment
) const noexcept {
    if (bytes >= large_allocation_threshold) {
        return std::max(alignment, page_size_);
    }
    return alignment;
}

detail::ThreadCache* MemoryPool::registerThreadCache() noexcept {
    // Registration is the only lock needed before a thread can use its cache.
    auto& cache = detail::current_thread_cache;
    if (cache.owner == this) {
        return &cache;
    }
    if (cache.owner != nullptr) {
        cache.owner->releaseThreadCache(cache);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (region_ == nullptr) {
        return nullptr;
    }
    cache.bins.fill(nullptr);
    cache.counts.fill(0);
    cache.targets.fill(initial_cached_blocks_per_bin);
    cache.refill_events.fill(0);
    cache.cached_bytes = 0;
    cache.owner = this;
    ++active_thread_caches_;
    return &cache;
}

detail::BlockHeader* MemoryPool::takeCachedBlock(
    detail::ThreadCache& cache,
    std::size_t bytes,
    std::size_t alignment
) noexcept {
    // Search the narrowest usable 64-byte class first to preserve best fit.
    std::size_t minimum_size = 0;
    if (!minimumBlockSize(bytes, minimum_size) ||
        minimum_size > small_block_limit) {
        return nullptr;
    }

    for (std::size_t bin = smallBinIndex(minimum_size);
         bin < small_bin_count;
         ++bin) {
        detail::BlockHeader* best = nullptr;
        for (detail::BlockHeader* block = cache.bins[bin]; block != nullptr;
             block = block->next_free) {
            if (calculateLayout(block, bytes, alignment).user != nullptr) {
                best = block;
                break;
            }
        }
        if (best == nullptr) {
            continue;
        }

        if (best->previous_free != nullptr) {
            best->previous_free->next_free = best->next_free;
        } else {
            cache.bins[bin] = best->next_free;
        }
        if (best->next_free != nullptr) {
            best->next_free->previous_free = best->previous_free;
        }
        best->previous_free = nullptr;
        best->next_free = nullptr;
        --cache.counts[bin];
        cache.cached_bytes -= best->total_size;
        return best;
    }
    return nullptr;
}

detail::BlockHeader* MemoryPool::findBestFit(
    std::size_t bytes,
    std::size_t alignment,
    std::size_t minimum_block_size
) const noexcept {
    std::size_t minimum_size = 0;
    if (!minimumBlockSize(bytes, minimum_size)) {
        return nullptr;
    }
    minimum_size = std::max(minimum_size, minimum_block_size);

    if (minimum_size <= small_block_limit) {
        for (std::size_t bin = smallBinIndex(minimum_size);
             bin < small_bin_count;
             ++bin) {
            const std::size_t word = bin / 64;
            const std::uint64_t bit = std::uint64_t{1} << (bin % 64);
            if ((occupied_small_bins_[word] & bit) == 0) {
                continue;
            }

            for (detail::BlockHeader* block = small_bins_[bin];
                 block != nullptr;
                 block = block->next_free) {
                if (calculateLayout(block, bytes, alignment).user != nullptr) {
                    return block;
                }
            }
        }
    }

    const std::size_t first_large_bin = largeBinIndex(
        std::max(minimum_size, small_block_limit + 1)
    );
    for (std::size_t bin = first_large_bin;
         bin < large_bin_count;
         ++bin) {
        const std::uint64_t bit = std::uint64_t{1} << bin;
        if ((occupied_large_bins_ & bit) == 0) {
            continue;
        }

        detail::BlockHeader* best = nullptr;
        for (detail::BlockHeader* block = large_bins_[bin]; block != nullptr;
             block = block->next_free) {
            if (calculateLayout(block, bytes, alignment).user != nullptr &&
                (best == nullptr || block->total_size < best->total_size)) {
                best = block;
            }
        }
        if (best != nullptr) {
            return best;
        }
    }
    return nullptr;
}

void* MemoryPool::activateBlock(
    detail::BlockHeader* block,
    std::size_t bytes,
    std::size_t alignment
) noexcept {
    const Layout layout = calculateLayout(block, bytes, alignment);
    block->requested_size = bytes;
    block->user_pointer = layout.user;
    block->previous_free = nullptr;
    block->next_free = nullptr;
    block->magic = live_block_magic;

    #ifndef ENABLE_CHECKS_METRICS
         std::byte* front_address = layout.user;
    #endif

    #ifdef ENABLE_CHECKS_METRICS
        std::byte* front_address = layout.user - sizeof(std::uint32_t);
    #endif
    std::byte* owner_address =
        front_address - sizeof(detail::BlockHeader*);
    std::memcpy(owner_address, &block, sizeof(block));

#ifdef ENABLE_CHECKS_METRICS
    storeCanary(front_address, front_canary);
    storeCanary(layout.user + bytes, rear_canary);
    allocated_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    live_allocations_.fetch_add(1, std::memory_order_relaxed);
    total_allocations_.fetch_add(1, std::memory_order_relaxed);
#endif

    return layout.user;
}

void* MemoryPool::allocate(std::size_t bytes, std::size_t alignment) {
    if (bytes == 0) {
        bytes = 1;
    }
    if (!isPowerOfTwo(alignment)) {
        throw std::bad_alloc{};
    }

    detail::ThreadCache* cache = registerThreadCache();
    if (cache == nullptr) {
        throw std::bad_alloc{};
    }
    alignment = effectiveAlignment(bytes, alignment);

    if (detail::BlockHeader* cached =
            takeCachedBlock(*cache, bytes, alignment)) {
        // The common path never acquires the central-pool mutex.
        return activateBlock(cached, bytes, alignment);
    }

    detail::BlockHeader* best = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (region_ == nullptr) {
            throw std::bad_alloc{};
        }

        best = findBestFit(bytes, alignment);
        if (best == nullptr) {
            flushThreadCacheUnlocked(*cache);
            best = findBestFit(bytes, alignment);
        }
        if (best == nullptr) {
            // Free blocks normally remain separate. Pay for coalescing only
            // when no individual block can satisfy this allocation.
            coalesceFreeBlocksUnlocked();
            best = findBestFit(bytes, alignment);
        }
        if (best == nullptr) {
            throw std::bad_alloc{};
        }

        const Layout layout = calculateLayout(best, bytes, alignment);
        if (layout.used_size <= small_block_limit &&
            alignment <= alignof(std::max_align_t)) {
            best = refillSmallCacheUnlocked(
                *cache,
                best,
                bytes,
                alignment
            );
        } else {
            best = reserveFreeBlock(best, bytes, alignment, 0);
        }
    }
    return activateBlock(best, bytes, alignment);
}

detail::BlockHeader* MemoryPool::reserveFreeBlock(
    detail::BlockHeader* block,
    std::size_t bytes,
    std::size_t alignment,
    std::size_t preferred_size
) noexcept {
    const Layout layout = calculateLayout(block, bytes, alignment);
    std::size_t reserved_size = std::max(layout.used_size, preferred_size);
    if (reserved_size > block->total_size ||
        block->total_size - reserved_size < minimum_remainder) {
        reserved_size = block->total_size;
    }

    removeFreeBlock(block);
    const std::size_t remainder_size = block->total_size - reserved_size;
    if (remainder_size >= minimum_remainder) {
        auto* remainder_address =
            reinterpret_cast<std::byte*>(block) + reserved_size;
        auto* remainder =
            ::new (remainder_address) detail::BlockHeader{};
        remainder->total_size = remainder_size;
        block->total_size = reserved_size;
        insertFreeBlock(remainder);
    }
    block->magic = reserved_block_magic;
    return block;
}

detail::BlockHeader* MemoryPool::refillSmallCacheUnlocked(
    detail::ThreadCache& cache,
    detail::BlockHeader* first,
    std::size_t bytes,
    std::size_t alignment
) noexcept {
    const Layout layout = calculateLayout(first, bytes, alignment);
    const std::size_t slab_size = static_cast<std::size_t>(alignUp(
        layout.used_size,
        small_bin_quantum
    ));
    first = reserveFreeBlock(first, bytes, alignment, slab_size);
    if (first->total_size > small_block_limit) {
        return first;
    }

    const std::size_t bin = smallBinIndex(first->total_size);
    ++cache.refill_events[bin];
    if (cache.refill_events[bin] >= cache_growth_interval) {
        cache.targets[bin] = std::min(
            cache.targets[bin] * 2,
            maximum_cached_blocks_per_bin
        );
        cache.refill_events[bin] = 0;
    }

    const std::size_t room = cache.targets[bin] > cache.counts[bin]
        ? cache.targets[bin] - cache.counts[bin]
        : 0;
    const std::size_t byte_room =
        cache.cached_bytes < maximum_thread_cache_bytes
        ? (maximum_thread_cache_bytes - cache.cached_bytes) / slab_size
        : 0;
    const std::size_t refill_count = std::min(
        std::min(room, byte_room),
        cache_refill_batch
    );
    for (std::size_t index = 0; index < refill_count; ++index) {
        detail::BlockHeader* block = findBestFit(
            bytes,
            alignment,
            slab_size
        );
        if (block == nullptr) {
            break;
        }
        block = reserveFreeBlock(block, bytes, alignment, slab_size);
        if (block->total_size > small_block_limit) {
            block->magic = free_block_magic;
            insertFreeBlock(block);
            break;
        }
        pushCachedBlock(cache, block);
    }
    return first;
}

void MemoryPool::pushCachedBlock(
    detail::ThreadCache& cache,
    detail::BlockHeader* block
) noexcept {
    const std::size_t bin = smallBinIndex(block->total_size);
    block->magic = cached_block_magic;
    block->previous_free = nullptr;
    block->next_free = cache.bins[bin];
    if (block->next_free != nullptr) {
        block->next_free->previous_free = block;
    }
    cache.bins[bin] = block;
    ++cache.counts[bin];
    cache.cached_bytes += block->total_size;
}

void MemoryPool::cacheBlock(
    detail::ThreadCache& cache,
    detail::BlockHeader* block
) noexcept {
    const std::size_t bin = smallBinIndex(block->total_size);
    pushCachedBlock(cache, block);

    if (cache.counts[bin] > cache.targets[bin] ||
        cache.cached_bytes > maximum_thread_cache_bytes) {
        // Keep the frequently used half and return the rest in one batch.
        const std::size_t retained = std::max(
            std::size_t{1},
            cache.targets[bin] / 2
        );
        const std::size_t flush_count = cache.counts[bin] > retained
            ? cache.counts[bin] - retained
            : std::min(cache.counts[bin], cache_flush_batch);
        std::lock_guard<std::mutex> lock(mutex_);
        flushCacheBinUnlocked(cache, bin, flush_count);
        for (std::size_t candidate = small_bin_count;
             cache.cached_bytes > maximum_thread_cache_bytes &&
                 candidate > 0;
             --candidate) {
            const std::size_t candidate_bin = candidate - 1;
            while (cache.cached_bytes > maximum_thread_cache_bytes &&
                   cache.counts[candidate_bin] > 0) {
                flushCacheBinUnlocked(
                    cache,
                    candidate_bin,
                    std::min(
                        cache.counts[candidate_bin],
                        cache_flush_batch
                    )
                );
            }
        }
    }
}

void MemoryPool::deallocate(void* pointer) noexcept {
    if (pointer == nullptr) {
        return;
    }

    const auto begin = region_begin_.load(std::memory_order_acquire);
    const auto end = region_end_.load(std::memory_order_acquire);
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    if (begin == 0 || address < begin || address >= end) {
        report(MemoryError::invalid_pointer, pointer);
        return;
    }

    auto* user = static_cast<std::byte*>(pointer);
    const auto* region_start = reinterpret_cast<const std::byte*>(begin);
    if (user < region_start + sizeof(detail::BlockHeader) +
                   sizeof(detail::BlockHeader*) 
                   #ifdef ENABLE_CHECKS_METRICS
                     + sizeof(std::uint32_t) 
                   #endif
                    ) {
        report(MemoryError::invalid_pointer, pointer);
        return;
    }

    #ifndef ENABLE_CHECKS_METRICS
    std::byte* front_address = user;
    #endif

    #ifdef ENABLE_CHECKS_METRICS
    std::byte* front_address = user - sizeof(std::uint32_t);
    #endif
    std::byte* owner_address =
        front_address - sizeof(detail::BlockHeader*);
    detail::BlockHeader* block = nullptr;
    std::memcpy(&block, owner_address, sizeof(block));

    const auto block_address = reinterpret_cast<std::uintptr_t>(block);
    if (block_address < begin || block_address >= end) {
        report(MemoryError::invalid_pointer, pointer);
        return;
    }
    if (block->magic == free_block_magic ||
        block->magic == cached_block_magic ||
        block->magic == retired_block_magic) {
        report(MemoryError::double_free, pointer);
        return;
    }
    if (block->magic != live_block_magic 
#ifdef ENABLE_CHECKS_METRICS
        || block->user_pointer != pointer
#endif
    ) {
        report(MemoryError::invalid_pointer, pointer);
        return;
    }

#ifdef ENABLE_CHECKS_METRICS
    const bool front_valid = loadCanary(front_address) == front_canary;
    const bool rear_valid =
        loadCanary(user + block->requested_size) == rear_canary;
    if (!front_valid && !rear_valid) {
        report(MemoryError::both_canaries_corrupted, pointer);
    } else if (!front_valid) {
        report(MemoryError::front_canary_corrupted, pointer);
    } else if (!rear_valid) {
        report(MemoryError::rear_canary_corrupted, pointer);
    }
#endif

    detail::ThreadCache* cache = nullptr;
    if (block->total_size <= small_block_limit) {
        cache = registerThreadCache();
    }

#ifdef ENABLE_CHECKS_METRICS

    allocated_bytes_.fetch_sub(
        block->requested_size,
        std::memory_order_relaxed
    );
    live_allocations_.fetch_sub(1, std::memory_order_release);
    total_deallocations_.fetch_add(1, std::memory_order_relaxed);

    block->requested_size = 0;
    block->user_pointer = nullptr;
#endif

    if (cache != nullptr) {
        // Coalescing is deferred until this cache returns a batch.
        cacheBlock(*cache, block);
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    block->magic = free_block_magic;
    insertFreeBlock(block);
}

void MemoryPool::flushCacheBinUnlocked(
    detail::ThreadCache& cache,
    std::size_t bin,
    std::size_t count
) noexcept {
    while (count > 0 && cache.bins[bin] != nullptr) {
        detail::BlockHeader* block = cache.bins[bin];
        cache.bins[bin] = block->next_free;
        if (cache.bins[bin] != nullptr) {
            cache.bins[bin]->previous_free = nullptr;
        }
        block->previous_free = nullptr;
        block->next_free = nullptr;
        --cache.counts[bin];
        cache.cached_bytes -= block->total_size;
        --count;

        block->magic = free_block_magic;
        insertFreeBlock(block);
    }
}

void MemoryPool::flushThreadCacheUnlocked(
    detail::ThreadCache& cache
) noexcept {
    for (std::size_t bin = 0; bin < small_bin_count; ++bin) {
        flushCacheBinUnlocked(cache, bin, cache.counts[bin]);
    }
}

void MemoryPool::releaseThreadCache(detail::ThreadCache& cache) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cache.owner != this) {
        return;
    }
    flushThreadCacheUnlocked(cache);
    cache.owner = nullptr;
    --active_thread_caches_;
}

void MemoryPool::setErrorHandler(ErrorHandler handler) noexcept {
    error_handler_.store(
        handler == nullptr ? defaultErrorHandler : handler,
        std::memory_order_release
    );
}

detail::BlockHeader* MemoryPool::nextPhysicalBlock(
    detail::BlockHeader* block
) const noexcept {
    auto* next_address =
        reinterpret_cast<std::byte*>(block) + block->total_size;
    auto* region_end = static_cast<std::byte*>(region_) + region_size_;
    if (next_address >= region_end) {
        return nullptr;
    }
    return reinterpret_cast<detail::BlockHeader*>(next_address);
}

void MemoryPool::insertFreeBlock(detail::BlockHeader* block) noexcept {
    block->magic = free_block_magic;
    block->previous_free = nullptr;

    if (block->total_size <= small_block_limit) {
        const std::size_t bin = smallBinIndex(block->total_size);
        block->next_free = small_bins_[bin];
        if (block->next_free != nullptr) {
            block->next_free->previous_free = block;
        }
        small_bins_[bin] = block;
        occupied_small_bins_[bin / 64] |=
            std::uint64_t{1} << (bin % 64);
        return;
    }

    const std::size_t bin = largeBinIndex(block->total_size);
    block->next_free = large_bins_[bin];
    if (block->next_free != nullptr) {
        block->next_free->previous_free = block;
    }
    large_bins_[bin] = block;
    occupied_large_bins_ |= std::uint64_t{1} << bin;
}

void MemoryPool::removeFreeBlock(detail::BlockHeader* block) noexcept {
    detail::BlockHeader** head = nullptr;
    std::uint64_t* occupied_word = nullptr;
    std::uint64_t occupied_bit = 0;

    if (block->total_size <= small_block_limit) {
        const std::size_t bin = smallBinIndex(block->total_size);
        head = &small_bins_[bin];
        occupied_word = &occupied_small_bins_[bin / 64];
        occupied_bit = std::uint64_t{1} << (bin % 64);
    } else {
        const std::size_t bin = largeBinIndex(block->total_size);
        head = &large_bins_[bin];
        occupied_word = &occupied_large_bins_;
        occupied_bit = std::uint64_t{1} << bin;
    }

    if (block->previous_free != nullptr) {
        block->previous_free->next_free = block->next_free;
    } else {
        *head = block->next_free;
    }
    if (block->next_free != nullptr) {
        block->next_free->previous_free = block->previous_free;
    }
    block->previous_free = nullptr;
    block->next_free = nullptr;
    if (*head == nullptr) {
        *occupied_word &= ~occupied_bit;
    }
}

void MemoryPool::coalesceFreeBlocksUnlocked() noexcept {
    if (region_ == nullptr) {
        return;
    }

    auto* block = static_cast<detail::BlockHeader*>(region_);
    while (detail::BlockHeader* next = nextPhysicalBlock(block)) {
        if (block->magic == free_block_magic &&
            next->magic == free_block_magic) {
            removeFreeBlock(block);
            removeFreeBlock(next);
            block->total_size += next->total_size;
            next->magic = retired_block_magic;
            insertFreeBlock(block);
            continue;
        }
        block = next;
    }
}

void MemoryPool::report(MemoryError error, const void* pointer) const noexcept {
    error_handler_.load(std::memory_order_acquire)(error, pointer);
}

void MemoryPool::defaultErrorHandler(
    MemoryError error,
    const void*
) noexcept {
    const char* message =
        "CustomMemoryAllocator: invalid memory operation\n";
    switch (error) {
        case MemoryError::front_canary_corrupted:
            message = "CustomMemoryAllocator: front canary corrupted\n";
            break;
        case MemoryError::rear_canary_corrupted:
            message = "CustomMemoryAllocator: rear canary corrupted\n";
            break;
        case MemoryError::both_canaries_corrupted:
            message = "CustomMemoryAllocator: both canaries corrupted\n";
            break;
        case MemoryError::double_free:
            message = "CustomMemoryAllocator: double free\n";
            break;
        case MemoryError::invalid_pointer:
            break;
    }
    static_cast<void>(::write(STDERR_FILENO, message, std::strlen(message)));
}

}
