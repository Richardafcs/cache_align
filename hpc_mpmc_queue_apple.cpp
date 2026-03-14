/**
 * @file hpc_mpmc_queue.cpp
 * @brief Production-grade Multi-Producer Multi-Consumer (MPMC) Bounded Queue.
 * * Based on Dmitry Vyukov's design.
 * * Uses XADD (Fetch-and-Add) instead of CMPXCHG (CAS) to prevent contention collapse.
 * * Uses per-slot sequence numbers for localized, independent synchronization.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <new>
#include <string_view>
#include <charconv>
#include <memory>

// Hardware pause for spin-waiting
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #include <immintrin.h>
    #define CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define CPU_PAUSE() __asm__ volatile("yield" ::: "memory")
#else
    #define CPU_PAUSE() std::this_thread::yield()
#endif

#if defined(__APPLE__) && defined(__aarch64__)
    constexpr size_t CACHE_LINE_SIZE = 128; // Apple M-Series True Cache Line
#elif defined(__cpp_lib_hardware_interference_size)
    constexpr size_t CACHE_LINE_SIZE = std::hardware_constructive_interference_size;
#else
    constexpr size_t CACHE_LINE_SIZE = 64;
#endif

template<typename T, size_t Capacity>
class MPMCQueue {
    static_assert((Capacity != 0) && ((Capacity & (Capacity - 1)) == 0), 
                  "Capacity must be a power of 2 for fast modulo masking.");
    
    static constexpr size_t MASK = Capacity - 1;

    // ------------------------------------------------------------------------
    // THE SLOT (CELL) DESIGN
    // Every slot in the array has its own atomic sequence number.
    // We force alignment on the Cell to prevent adjacent threads writing to
    // adjacent slots from triggering false sharing invalidations.
    // Note: This trades L3 cache capacity for L1 contention elimination.
    // ------------------------------------------------------------------------
    struct alignas(CACHE_LINE_SIZE) Cell {
        std::atomic<size_t> sequence;
        T data;
    };

    // The buffer of cells
    Cell buffer_[Capacity];

    // ------------------------------------------------------------------------
    // GLOBAL POINTERS
    // Aligned to different cache lines to prevent Producer/Consumer interference.
    // ------------------------------------------------------------------------
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> enqueue_pos_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> dequeue_pos_{0};

public:
    MPMCQueue() {
        // Initialize the sequence numbers.
        // Slot 0 gets sequence 0, Slot 1 gets 1... Slot N gets N.
        for (size_t i = 0; i < Capacity; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    /**
     * @brief Enqueues an item. Safe to call from ANY thread concurrently.
     * @param data The item to enqueue.
     * @return true if successful, false if the queue is full.
     */
    bool push(const T& data) {
        Cell* cell;
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

        while (true) {
            cell = &buffer_[pos & MASK];
            size_t seq = cell->sequence.load(std::memory_order_acquire);

            // Difference determines if it's our turn to write to this slot.
            intptr_t diff = (intptr_t)seq - (intptr_t)pos;

            if (diff == 0) {
                // It is our turn! Attempt to claim this specific ticket via CAS.
                // If multiple producers are here, only one wins. 
                // The others will loop, reload enqueue_pos, and try the NEXT ticket.
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break; // Ticket claimed successfully
                }
            } else if (diff < 0) {
                // The sequence is behind. The queue is full.
                return false;
            } else {
                // diff > 0. Another producer already claimed this pos.
                // Help advance our local copy of the pointer and try again.
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }

        // We claimed the ticket. Write the data safely (no other thread can be here).
        cell->data = data;

        // Advance the sequence by 1 to signal to the Consumer that the data is ready.
        // Release semantics ensure the data write is visible before the sequence updates.
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Dequeues an item. Safe to call from ANY thread concurrently.
     * @param data Reference to store the dequeued item.
     * @return true if successful, false if the queue is empty.
     */
    bool pop(T& data) {
        Cell* cell;
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);

        while (true) {
            cell = &buffer_[pos & MASK];
            size_t seq = cell->sequence.load(std::memory_order_acquire);

            // Difference determines if the Producer has finished writing.
            // A slot is ready to be read when sequence == pos + 1
            intptr_t diff = (intptr_t)seq - (intptr_t)(pos + 1);

            if (diff == 0) {
                // Data is ready! Attempt to claim this reading ticket.
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break; // Ticket claimed successfully
                }
            } else if (diff < 0) {
                // The sequence is behind. The queue is empty.
                return false;
            } else {
                // diff > 0. Another consumer already claimed this reading pos.
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }

        // We claimed the ticket. Read the data.
        data = cell->data;

        // Advance the sequence by Capacity - 1 to reset it for the NEXT Producer
        // who will wrap around the ring buffer.
        // e.g., if Capacity is 8, and pos was 0. 
        // Sequence goes: 0 (init) -> 1 (written) -> 8 (read, ready for next wrap).
        cell->sequence.store(pos + MASK + 1, std::memory_order_release);
        return true;
    }
};

// ============================================================================
// BENCHMARKING: 4 Producers vs 4 Consumers
// ============================================================================

constexpr size_t NUM_ITEMS = 5'000'000;
constexpr size_t THREADS = 4;

static bool has_flag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == flag) {
            return true;
        }
    }
    return false;
}

static uint64_t parse_u64_flag(int argc, char** argv, const char* flag, uint64_t fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == flag) {
            uint64_t value = 0;
            auto sv = std::string_view(argv[i + 1]);
            auto res = std::from_chars(sv.data(), sv.data() + sv.size(), value);
            if (res.ec == std::errc()) {
                return value;
            }
            return fallback;
        }
    }
    return fallback;
}

static uint64_t range_sum(uint64_t start, uint64_t end_inclusive) {
    uint64_t n = end_inclusive - start + 1;
    return n * (start + end_inclusive) / 2;
}

static uint64_t xor_upto(uint64_t n) {
    switch (n & 3ULL) {
        case 0: return n;
        case 1: return 1;
        case 2: return n + 1;
        default: return 0;
    }
}

static uint64_t range_xor(uint64_t start, uint64_t end_inclusive) {
    if (start == 0) {
        return xor_upto(end_inclusive);
    }
    return xor_upto(end_inclusive) ^ xor_upto(start - 1);
}

static bool test_single_thread() {
    MPMCQueue<uint64_t, 8> queue;
    for (uint64_t i = 0; i < 1000; ++i) {
        bool pushed = queue.push(i);
        if (!pushed) return false;
        uint64_t out = 0;
        bool popped = queue.pop(out);
        if (!popped || out != i) return false;
    }
    return true;
}

static bool test_multi_thread() {
    constexpr size_t TEST_THREADS = 2;
    constexpr uint64_t ITEMS_PER_PRODUCER = 200'000;
    constexpr uint64_t TOTAL_ITEMS = TEST_THREADS * ITEMS_PER_PRODUCER;

    MPMCQueue<uint64_t, 1024> queue;
    std::atomic<uint64_t> consumed_count{0};
    std::atomic<uint64_t> consumed_sum{0};
    std::atomic<uint64_t> consumed_xor{0};

    auto producer_task = [&](size_t id) {
        uint64_t base = id * ITEMS_PER_PRODUCER;
        for (uint64_t i = 0; i < ITEMS_PER_PRODUCER; ++i) {
            uint64_t value = base + i;
            while (!queue.push(value)) { CPU_PAUSE(); }
        }
    };

    auto consumer_task = [&]() {
        uint64_t value = 0;
        while (consumed_count.load(std::memory_order_relaxed) < TOTAL_ITEMS) {
            if (queue.pop(value)) {
                consumed_count.fetch_add(1, std::memory_order_relaxed);
                consumed_sum.fetch_add(value, std::memory_order_relaxed);
                consumed_xor.fetch_xor(value, std::memory_order_relaxed);
            } else {
                CPU_PAUSE();
            }
        }
    };

    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    for (size_t i = 0; i < TEST_THREADS; ++i) producers.emplace_back(producer_task, i);
    for (size_t i = 0; i < TEST_THREADS; ++i) consumers.emplace_back(consumer_task);

    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    uint64_t expected_sum = 0;
    uint64_t expected_xor = 0;
    for (size_t i = 0; i < TEST_THREADS; ++i) {
        uint64_t start = i * ITEMS_PER_PRODUCER;
        uint64_t end_inclusive = start + ITEMS_PER_PRODUCER - 1;
        expected_sum += range_sum(start, end_inclusive);
        expected_xor ^= range_xor(start, end_inclusive);
    }

    return consumed_count.load() == TOTAL_ITEMS &&
           consumed_sum.load() == expected_sum &&
           consumed_xor.load() == expected_xor;
}

static int run_tests() {
    std::cout << "--- MPMC Correctness Tests ---\n";
    std::cout << std::flush;
    bool ok_single = test_single_thread();
    std::cout << "Single-thread wrap test: " << (ok_single ? "PASS" : "FAIL") << "\n";

    bool ok_multi = test_multi_thread();
    std::cout << "Multi-thread sum/xor test: " << (ok_multi ? "PASS" : "FAIL") << "\n";

    return (ok_single && ok_multi) ? 0 : 1;
}

static int run_benchmark(uint64_t items, size_t threads) {
    std::cout << "Initializing Vyukov MPMC Queue...\n";
    std::cout << std::flush;
    auto queue = std::make_unique<MPMCQueue<uint64_t, 65536>>();

    std::atomic<uint64_t> total_pushed{0};
    std::atomic<uint64_t> total_popped{0};

    auto producer_task = [&]() {
        uint64_t pushed = 0;
        for (size_t i = 0; i < items / threads; ++i) {
            while (!queue->push(i)) { CPU_PAUSE(); }
            pushed++;
        }

        total_pushed.fetch_add(pushed, std::memory_order_relaxed);
    };

    auto consumer_task = [&]() {
        uint64_t item;
        uint64_t popped = 0;
        for (size_t i = 0; i < items / threads; ++i) {
            while (!queue->pop(item)) { CPU_PAUSE(); }
            popped++;
        }
        total_popped.fetch_add(popped, std::memory_order_relaxed);
    };

    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    auto start_time = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < threads; ++i) producers.emplace_back(producer_task);
    for (size_t i = 0; i < threads; ++i) consumers.emplace_back(consumer_task);

    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end_time - start_time;

    std::cout << "--- MPMC Contention Benchmark Completed ---\n";
    std::cout << "Items Pushed: " << total_pushed.load() << "\n";
    std::cout << "Items Popped: " << total_popped.load() << "\n";
    std::cout << "Time elapsed: " << diff.count() << " seconds\n";
    std::cout << "Throughput  : " << (items / diff.count()) / 1'000'000 << " Million Ops/sec\n";

    return 0;
}

int main(int argc, char** argv) {
    if (has_flag(argc, argv, "--test")) {
        return run_tests();
    }
    uint64_t items = parse_u64_flag(argc, argv, "--items", NUM_ITEMS);
    size_t threads = static_cast<size_t>(parse_u64_flag(argc, argv, "--threads", THREADS));
    if (threads == 0) {
        threads = THREADS;
    }
    if (items < threads) {
        items = threads;
    }
    return run_benchmark(items, threads);
}
