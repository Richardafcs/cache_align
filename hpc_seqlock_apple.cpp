/**
 * @file hpc_seqlock.cpp
 * @brief Production-grade Sequential Lock (Seqlock).
 * * Exploits the MESI 'Shared' state by ensuring readers perform ZERO writes.
 * * Uses memory fences and release/acquire semantics to prevent torn reads.
 * * Hardware-aware spin-waiting to prevent branch prediction starvation.
 */

#include <atomic>
#include <cstdint>
#include <type_traits>
#include <iostream>
#include <thread>
#include <vector>
#include <chrono>

// Hardware pause instruction to prevent pipeline starvation and reduce power
// consumption during tight spin-loops.
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
#elif defined(__cp_lib_hardware_interference_size)
    constexpr size_t CACHE_LINE_SIZE = std::hardware_constructive_interference_size;
#else
    constexpr size_t CACHE_LINE_SIZE = 64;
#endif

/**
 * @class Seqlock
 * @brief Lock-free reading, spin-locked writing for 1-to-N shared state.
 */
template<typename T>
class Seqlock {
    // Seqlocks ONLY work with trivially copyable data (like structs of PODs).
    // If you use objects with complex copy constructors or pointers, a reader 
    // might crash if it reads a half-written object before the sequence check fails.
    static_assert(std::is_trivially_copyable_v<T>, 
                  "T must be trivially copyable to prevent torn read crashes.");

    // The sequence counter and the data are packed together.
    // They fit within one or two cache lines. We align the struct to ensure
    // the sequence counter sits at the very beginning of a fresh cache line.
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> sequence_{0};
    T payload_;

public:
    Seqlock() = default;
    explicit Seqlock(const T& initial_val) : payload_(initial_val) {}

    /**
    * @brief Writes new data to the payload. 
    * Executed by the single Writer thread.
    */
    void store(const T& new_data) {
        // 1. Load current sequence
        size_t seq = sequence_.load(std::memory_order_relaxed);

        // 2. Increment to an ODD number.
        // Release semantics ensure prior operations finish before we signal we are writing.
        sequence_.store(seq + 1, std::memory_order_relaxed);

        // 2. THE FIX: Drop a strict compiler/hardware barrier!
        // Nothing below this line can float above it.
        std::atomic_thread_fence(std::memory_order_release);

        // 3. Write the actual data. This is NOT an atomic operation!
        // It is incredibly fast, utilizing standard store instructions.
        payload_ = new_data;

        // 4. Increment to an EVEN number.
        // Release semantics ensure the payload write is globally visible BEFORE
        // the sequence number becomes even again.
        sequence_.store(seq + 2, std::memory_order_release);
   }

    /**
     * @brief Reads data from the payload.
     * Executed by N Reader threads. Does ZERO writes to memory.
     */
    T load() const {
        T copy;
        size_t seq0, seq1;

        do {
            // 1. Read sequence before touching data
            seq0 = sequence_.load(std::memory_order_acquire);

            // 2. If sequence is ODD, a writer is currently modifying the data.
            // We must wait. We use a hardware pause to yield the CPU pipeline.
            if (seq0 & 1) {
                CPU_PAUSE();
                continue;
            }

            // 3. Copy the data. (Non-atomic read!)
            copy = payload_;

            // 4. CRITICAL: Prevent compiler/CPU from reordering the payload read
            // to happen AFTER the second sequence read.
            std::atomic_thread_fence(std::memory_order_acquire);

            // 5. Read sequence again.
            seq1 = sequence_.load(std::memory_order_relaxed);

            // 6. If the sequence changed while we were reading, we read corrupted
            // (torn) data. Loop and try again.
        } while (seq0 != seq1);

        return copy;
    }
};

// ============================================================================
// SIMULATION: Market Reference Price Broadcasting
// ============================================================================

struct MarketData {
    double best_bid;
    double best_ask;
    uint64_t last_updata_ts;
};

int main() {
    std::cout << "Initializing HPC Seqlock...\n";
    Seqlock<MarketData> ticker_lock({100.00, 100.05, 0});

    std::atomic<bool> running{true};
    std::atomic<uint64_t> total_reads{0};

    // Spin up 8 Consumer (Strategy) Threads
    std::vector<std::thread> readers;
    for (int i = 0; i < 8; ++i) {
        readers.emplace_back([&ticker_lock, &running, &total_reads]() {
            uint64_t local_readers = 0;
            while (running.load(std::memory_order_relaxed)) {
                // READ ONLY: Zero cache invalidations across the 8 cores!
                MarketData data = ticker_lock.load();

                // Prevent compiler from optimizing the loop away
                volatile double dummy = data.best_bid;
                (void)dummy;

                local_readers++;
            }
            total_reads.fetch_add(local_readers, std::memory_order_relaxed);
        });
    }

    // Single Producer (Market Data Handler) Thread
    auto start = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 1; i <= 5'000'000; ++i) {
        ticker_lock.store({100.00 + (i * 0.01), 100.05 + (i * 0.01), i});

        // Simulate microsecond delay between ticks
        if (i % 1000 == 0) CPU_PAUSE(); 
    }

    running.store(false, std::memory_order_release);

    for (auto& t : readers) {
        t.join();
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    std::cout << "--- Seqlock Benchmark Completed ---\n";
    std::cout << "Time elapsed: " << diff.count() << " seconds\n";
    std::cout << "Total concurrent torn-free reads achieved: " << total_reads.load() << "\n";
    std::cout << "Read throughput: " << (total_reads.load() / diff.count()) / 1'000'000 
              << " Million Reads/sec\n";

    return 0;
}