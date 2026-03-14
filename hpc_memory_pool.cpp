/**
 * @file hpc_memory_pool.cpp
 * @brief Production-grade, Lock-Free, ABA-Safe Memory Pool.
 * * Uses Tagged Indices to solve the ABA problem in lock-free stacks.
 * * Guarantees zero false sharing between allocated objects via strict alignment.
 * * Designed to eliminate malloc/new latency spikes in the critical path.
 */

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <iostream>
#include <vector>
#include <thread>
#include <cassert>
#include <chrono>

// Fallback for hardware interference size if C++17 feature is missing
#if defined(__cpp_lib_hardware_interference_size)
    constexpr size_t CACHE_LINE_SIZE = std::hardware_destructive_interference_size;
#else
    constexpr size_t CACHE_LINE_SIZE = 64;
#endif

constexpr uint32_t END_OF_LIST = 0xFFFFFFFF;

/**
 * @class LockFreeMemoryPool
 * @brief Pre-allocated memory pool providing O(1) lock-free allocations.
 */
template<typename T, size_t Capacity>
class LockFreeMemoryPool {
    static_assert(Capacity < END_OF_LIST, "Capacity exceeds 32-bit index limit.");

    // ------------------------------------------------------------------------
    // MEMORY BLOCK DEFINITION
    // Every block is forced to be a multiple of the cache line size.
    // If thread A allocates Block 0 and thread B allocates Block 1, they 
    // will NEVER suffer false sharing when modifying their respective 'payloads'.
    // ------------------------------------------------------------------------
    union alignas(CACHE_LINE_SIZE) Block {
        T payload;           // Used when the block is allocated to the application
        uint32_t next_free;  // Used when the block is in the free-list

        Block() {}           // Required because of union containing non-trivial types
        ~Block() {}
    };

    // The contiguous block of memory. In a true OS-bypass system, this array 
    // would be backed by 2MB or 1GB HugePages via mmap(MAP_HUGETLB) to eliminate 
    // TLB (Translation Lookaside Buffer) misses.
    Block blocks_[Capacity];

    // ------------------------------------------------------------------------
    // ABA-SAFE HEAD POINTER
    // We pack a 32-bit index and a 32-bit tag (generation counter) into a 
    // single 64-bit atomic integer. This allows us to use standard 64-bit CAS.
    // ------------------------------------------------------------------------
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> free_list_head_;

    // Helper to pack index and tag into 64 bits
    static constexpr uint64_t pack(uint32_t index, uint32_t tag) {
        return (static_cast<uint64_t>(tag) << 32) | index;
    }

    // Helpers to unpack 64 bits into index and tag
    static constexpr uint32_t get_index(uint64_t packed) { return static_cast<uint32_t>(packed); }
    static constexpr uint32_t get_tag(uint64_t packed) { return static_cast<uint32_t>(packed >> 32); }

public:
    LockFreeMemoryPool() {
        // Initialize the free list: Block 0 points to Block 1, Block 1 to 2, etc.
        for (uint32_t i = 0; i < Capacity - 1; ++i) {
            blocks_[i].next_free = i + 1;
        }
        blocks_[Capacity - 1].next_free = END_OF_LIST;

        // Set head to index 0, tag 0
        free_list_head_.store(pack(0, 0), std::memory_order_relaxed);
    }

    /**
     * @brief Allocates an object from the pool.
     * @return Pointer to uninitialized memory, or nullptr if out of memory.
     */
    T* allocate() {
        uint64_t current_head = free_list_head_.load(std::memory_order_acquire);
        uint64_t new_head;

        do {
            uint32_t index = get_index(current_head);

            if (index == END_OF_LIST) {
                return nullptr; // Pool exhausted
            }

            uint32_t tag = get_tag(current_head);
            uint32_t next_index = blocks_[index].next_free;

            // Increment the tag to prevent ABA on the next operation
            new_head = pack(next_index, tag + 1);

            // Compare-And-Swap. 
            // If another thread modified free_list_head_ in the meantime, 
            // current_head is automatically updated to the new value, and we loop.
        } while (!free_list_head_.compare_exchange_weak(
            current_head, new_head,
            std::memory_order_acq_rel, // Memory visibility on success
            std::memory_order_acquire  // Memory visibility on failure
        ));

        // Placement new to call constructor if needed (skipped here for raw speed, 
        // assuming POD types, but good practice for generic T).
        return &(blocks_[get_index(current_head)].payload);
    }

    /**
     * @brief Returns an object to the pool.
     * @param ptr Pointer to the memory previously allocated by this pool.
     */
    void deallocate(T* ptr) {
        if (!ptr) return;

        // Calculate index based on pointer arithmetic
        Block* block_ptr = reinterpret_cast<Block*>(ptr);
        uint32_t index = static_cast<uint32_t>(block_ptr - blocks_);

        assert(index < Capacity && "Pointer does not belong to this pool!");

        uint64_t current_head = free_list_head_.load(std::memory_order_acquire);
        uint64_t new_head;

        do {
            uint32_t head_index = get_index(current_head);
            uint32_t tag = get_tag(current_head);

            // Link the returning block to the current head of the free list
            blocks_[index].next_free = head_index;

            // Increment the tag to prevent ABA
            new_head = pack(index, tag + 1);
        } while (!free_list_head_.compare_exchange_weak(
            current_head, new_head,
            std::memory_order_release, // Ensure payload writes are visible before freeing
            std::memory_order_acquire
        ));
    }
};

// ============================================================================
// SIMULATION: Combining Pool + SPSC Queue for HFT message passing
// ============================================================================

struct alignas(CACHE_LINE_SIZE) MarketUpdate {
    uint64_t timestamp_ns;
    double price;
    uint32_t volume;
    char ticker[8];
    // Struct is forced to 64 bytes by alignas. No false sharing when processing.
};

// Large pool must not live on the stack; keep it in static storage.
static LockFreeMemoryPool<MarketUpdate, 1'000'000> pool;

int main() {
    std::cout << "Initializing Lock-Free ABA-Safe Memory Pool...\n";

    // Benchmark parameters
    constexpr int num_threads = 4;
    constexpr int iterations = 100'000;
    constexpr int burst_size = 50;

    auto stress_task = [](int iters) {
        std::vector<MarketUpdate*> held_blocks;
        held_blocks.reserve(burst_size); // Prevent hidden mallocs

        for (int i = 0; i < iters; ++i) {
            // Burst allocate
            for(int j = 0; j < burst_size; ++j) {
                if (auto* msg = pool.allocate()) {
                    msg->price = 100.0 + j; // Touch the memory (forces cache coherence)
                    held_blocks.push_back(msg);
                }
            }
            // Burst deallocate
            for(auto* msg : held_blocks) {
                pool.deallocate(msg);
            }
            held_blocks.clear();
        }
    };

    std::vector<std::thread> threads;
    std::cout << "Starting benchmark across " << num_threads << " threads...\n";

    // ==========================================
    // START THE CLOCK
    // ==========================================
    auto start_time = std::chrono::steady_clock::now();

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(stress_task, iterations);
    }

    for (auto& t : threads) {
        t.join();
    }

    // ==========================================
    // STOP THE CLOCK
    // ==========================================
    auto end_time = std::chrono::steady_clock::now();

    // Calculate total operations
    // 4 threads * 100,000 iterations * (50 allocs + 50 deallocs)
    uint64_t total_operations = static_cast<uint64_t>(num_threads) * iterations * (burst_size * 2);

    // Calculate durations
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();

    // Prevent divide-by-zero if it runs too fast (sub-millisecond)
    double time_in_seconds = std::max(duration_ms / 1000.0, 0.0001); 
    double ns_per_op = static_cast<double>(duration_ns) / total_operations;
    double throughput_millions = (total_operations / time_in_seconds) / 1'000'000.0;

    std::cout << "=================================================\n";
    std::cout << "STRESS TEST COMPLETED\n";
    std::cout << "=================================================\n";
    std::cout << "Total Operations : " << total_operations << " (allocs + frees)\n";
    std::cout << "Total Time       : " << duration_ms << " ms\n";
    std::cout << "Latency per Op   : " << ns_per_op << " ns/op\n";
    std::cout << "Throughput       : " << throughput_millions << " Million Ops/sec\n";
    std::cout << "=================================================\n";

    return 0;
}