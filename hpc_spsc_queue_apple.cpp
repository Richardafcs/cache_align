/**
 * @file hpc_spsc_queue.cpp
 * @brief Production-grade Lock-Free Single-Producer Single-Consumer Ring Buffer.
 * * Optimized for NUMA/multi-core architectures. Mitigates false sharing via
 * strict cache-line alignment and minimizes MESI interconnect traffic using
 * index caching and relaxed memory orderings.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>
#include <iostream>
#include <chrono>
#include <new>

// 1. THE APPLE SILICON FIX
#if defined(__APPLE__) && defined(__aarch64__)
    constexpr size_t CACHE_LINE_SIZE = 128; // Apple M-Series True Cache Line
#elif defined(__cp_lib_hardware_interference_size)
    constexpr size_t CACHE_LINE_SIZE = std::hardware_constructive_interference_size;
#else
    constexpr size_t CACHE_LINE_SIZE = 64;
#endif

// 2. THE HARDWARE YIELD MACRO
#if defined(__aarch64__) || defined(_M_ARM64)
    // ARM/Apple Silicon Yield
    #define CPU_YIELD() __asm__ volatile("yield" ::: "memory")
#elif defined(__x86_64__) || defined(_M_X64)
    #include <immintrin.h>
    // Intel/AMD Pause
    #define CPU_YIELD() _mm_pause()
#else
    #define CPU_YIELD()
#endif

/**
 * @class OptimizedSPSCQueue
 * @brief Zero-allocation, lock-free ring buffer with false-sharing immunity.
 */
template<typename T, size_t Capacity>
class OptimizedSPSCQueue {
    // Capacity must be a power of 2 to allow fast bitwise modulo operations.
    static_assert((Capacity != 0) && ((Capacity & (Capacity - 1)) == 0), "Capacity must be a power of 2 for bitwise masking.");  

    // Mask used for fast modulo arithmetic: index & mask == index % Capacity
    static constexpr size_t MASK = Capacity - 1;

    // ------------------------------------------------------------------------
    // PRODUCER STATE
    // Aligned to cache line boundary to prevent false sharing with Consumer.
    // ------------------------------------------------------------------------
    struct alignas(CACHE_LINE_SIZE) ProducerState {
        std::atomic<size_t> head{0};      // Written by Producer, Read by Consumer
        size_t cached_tail{0};            // Local shadow copy of Consumer's tail
    };

    // ------------------------------------------------------------------------
    // CONSUMER STATE
    // Aligned to cache line boundary to prevent false sharing with Producer.
    // ------------------------------------------------------------------------
    struct alignas(CACHE_LINE_SIZE) ConsumerState {
        std::atomic<size_t> tail{0};      // Written by Consumer, Read by Producer
        size_t cached_head{0};            // Local shadow copy of Producer's head
    };

    ProducerState prod_state_;
    ConsumerState cons_state_;

    // Buffer is also aligned to prevent the data array from spilling into
    // the same cache line as the atomic state variables.
    alignas(CACHE_LINE_SIZE) T buffer_[Capacity];

public:
    OptimizedSPSCQueue() = default;

     /**
     * @brief Pushes an item into the queue. Executed strictly by the Producer thread.
     * @param item The data to enqueue.
     * @return true if successful, false if the queue is full.
     */
    bool push(const T& item) {
        const size_t current_head = prod_state_.head.load(std::memory_order_relaxed);
        const size_t next_head = (current_head + 1) & MASK;

        // Check against the local, cached tail first. No cache coherency traffic!
        if (next_head == prod_state_.cached_tail) {
            // Local cache says full. We must fetch the actual tail from the Consumer core.
            // Acquire semantics ensure we see all memory writes that happened before the tail update.
            prod_state_.cached_tail = cons_state_.tail.load(std::memory_order_acquire);

            // Is it STILL full?
            if (next_head == prod_state_.cached_tail) {
                return false; // Queue genuinely full
            }
        }

        // Write the data to the buffer.
        buffer_[current_head] = item;

        // Publish the new head to the Consumer.
        // Release semantics guarantee the buffer write above is visible in memory 
        // BEFORE the head update becomes visible to other cores.
        prod_state_.head.store(next_head, std::memory_order_release);
        return true;
    }

    /**
     * @brief Pops an item from the queue. Executed strictly by the Consumer thread.
     * @param item Reference to store the dequeued data.
     * @return true if successful, false if the queue is empty.
     */
    bool pop(T& item) {
        const size_t current_tail = cons_state_.tail.load(std::memory_order_relaxed);

        // Check against the local, cached head first.
        if (current_tail == cons_state_.cached_head) {
            // Local cache says empty. Fetch the actual head from the Producer core.
            cons_state_.cached_head = prod_state_.head.load(std::memory_order_acquire);

            // Is it STILL empty?
            if (current_tail == cons_state_.cached_head) {
                return false; // Queue genuinely empty
            }
        }

        // Read the data from the buffer.
        item = buffer_[current_tail];

        // Publish the new tail to the Producer.
        const size_t next_tail = (current_tail + 1) & MASK;
        cons_state_.tail.store(next_tail, std::memory_order_release);
        return true;
    }
};

// ============================================================================
// NAIVE IMPLEMENTATION FOR BENCHMARKING (INTENTIONAL FALSE SHARING)
// ============================================================================
template<typename T, size_t Capacity>
class NaiveSPSCQueue {
    static_assert((Capacity != 0) && ((Capacity & (Capacity - 1)) == 0),
                  "Capacity must be a power of 2 for bitwise masking.");
    static constexpr size_t MASK = Capacity - 1;

    // INTENTIONAL DESIGN FLAW: 
    // head and tail are allocated adjacently. They will share the same 64-byte 
    // L1 cache line. Every write to 'head' invalidates 'tail' in the other core,
    // and vice versa, triggering massive MESI invalidation storms (Cache Thrashing).
    std::atomic<size_t> head{0};
    std::atomic<size_t> tail{0};

    T buffer_[Capacity];

public:
    bool push(const T& item) {
        // Using seq_cst (default) which is expensive, and loading remote atomic every time.
        size_t current_head = head.load();
        size_t next_head = (current_head + 1) & MASK;

        if (next_head == tail.load()) {
            return false;
        }

        buffer_[current_head] = item;
        head.store(next_head);
        return true;
    }

    bool pop(T& item) {
        size_t current_tail = tail.load();

        if (current_tail == head.load()) {
            return false;
        }

        item = buffer_[current_tail];
        tail.store((current_tail + 1) & MASK);
        return true;
    }
};

// ============================================================================
// BENCHMARKING HARNESS
// ============================================================================

constexpr size_t NUM_OPERATIONS = 50'000'000;
constexpr size_t QUEUE_CAPACITY = 1024;

template<typename QueueType>
void run_benchmark(const std::string& name) {
    QueueType queue;

    auto start_time = std::chrono::high_resolution_clock::now();

    std::thread producer([&]() {
        for (size_t i = 0; i < NUM_OPERATIONS; ++i) {
            while (!queue.push(i)) {
                CPU_YIELD(); // Let the M4 memory bus breathe!
            }
        }
    });

    std::thread consumer([&](){
        size_t item;
        for (size_t i = 0; i < NUM_OPERATIONS; ++i) {
            while (!queue.pop(item)) {
                CPU_YIELD(); // Let the M4 memory bus breathe!
            }
        }
    });

    producer.join();
    consumer.join();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end_time - start_time;
    
    double ops_per_sec = NUM_OPERATIONS / diff.count();
    
    std::cout << "--- " << name << " ---" << std::endl;
    std::cout << "Time elapsed : " << diff.count() << " seconds\n";
    std::cout << "Throughput   : " << ops_per_sec / 1'000'000 << " Million Ops/sec\n\n";
}

int main() {
    std::cout << "Hardware Destructive Interference Size: " << CACHE_LINE_SIZE << " bytes\n\n";

    // Run the poorly designed queue (False Sharing)
    run_benchmark<NaiveSPSCQueue<size_t, QUEUE_CAPACITY>>("Naive Queue (False Sharing & seq_cst)");

    // Run the production-grade queue (Cache Aligned, Shadow Variables, Acquire/Release)
    run_benchmark<OptimizedSPSCQueue<size_t, QUEUE_CAPACITY>>("Optimized Queue (Cache Isolated & Memory Order)");

    return 0;
}
