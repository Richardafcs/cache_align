/**
 * @file hpc_urcu.cpp
 * @brief Production-grade Userspace Read-Copy-Update (RCU) / Epoch-Based Reclamation.
 * * Allows lock-free, atomic-free traversal of large shared pointer data.
 * * Eliminates the cache-line bouncing of std::shared_ptr reference counting.
 * * Uses Thread-Local Epoch tracking and Grace Periods for safe memory deletion.
 */

#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <memory>
#include <cassert>

// Hardware destructive interference size
#if defined(__cpp_lib_hardware_interference_size)
    constexpr size_t CACHE_LINE_SIZE = std::hardware_destructive_interference_size;
#else
    constexpr size_t CACHE_LINE_SIZE = 64;
#endif

constexpr size_t MAX_THREADS = 64;

/**
 * @struct RCUThreadState
 * @brief Thread-local state for RCU readers.
 * * STRICLY ALIGNED to prevent false sharing between reader threads.
 * * If Thread 0 and Thread 1 update their epochs, they do not invalidate each other.
 */
struct alignas(CACHE_LINE_SIZE) RCUThreadState {
    std::atomic<bool> active{false};
    std::atomic<uint64_t> local_epoch{0};
};

/**
 * @class RCUDomain
 * @brief Manages global epochs and reader registrations for Safe Memory Reclamation.
 */
class RCUDomain {
    // The global clock for memory visibility
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> global_epoch_{1};

    // Array of states, one per thread. Padded to prevent false sharing.
    RCUThreadState thread_states_[MAX_THREADS];

public:
    RCUDomain() = default;

    /**
     * @brief Reader enters the critical section. Extremely fast.
     * @param tid The Thread ID (0 to MAX_THREADS-1)
     */
    void rcu_read_lock(size_t tid) {
        assert(tid < MAX_THREADS);

        // 1. Mark thread as active
        thread_states_[tid].active.store(true, std::memory_order_relaxed);

        // 2. Sample the global epoch and advertise it locally
        uint64_t current_epoch = global_epoch_.load(std::memory_order_relaxed);
        thread_states_[tid].local_epoch.store(current_epoch, std::memory_order_relaxed);

        // 3. Full memory barrier. 
        // Ensures the active/epoch advertisement is globally visible BEFORE
        // the thread begins reading the shared pointer.
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    /**
     * @brief Reader exits the critical section. Extremely fast.
     * @param tid The Thread ID (0 to MAX_THREADS-1)
     */
    void rcu_read_unlock(size_t tid) {
        assert(tid < MAX_THREADS);

        // 1. Mark thread as inactive.
        // Release order ensures all memory reads in the critical section 
        // are completed BEFORE the active flag is cleared.
        thread_states_[tid].active.store(false, std::memory_order_release);
    }

    /**
     * @brief Reader exits the critical section.
     */
    void synchronize_rcu() {
        // 1. Advance the global epoch.
        // Memory order seq_cst ensures this write is visible to all new readers.
        uint64_t new_epoch = global_epoch_.fetch_add(1, std::memory_order_seq_cst) + 1;

        // 2. Wait for all active threads to catch up or become inactive.
        for (size_t tid = 0; tid < MAX_THREADS; ++tid) {
            while (true) {
                // If thread is inactive, it doesn't hold any old pointers. Safe.
                if (!thread_states_[tid].active.load(std::memory_order_acquire)) {
                    break;
                }

                // If thread is active, but its epoch is >= our new epoch, 
                // it started reading AFTER we swapped the pointer. Safe.
                if (thread_states_[tid].local_epoch.load(std::memory_order_acquire) >= new_epoch) {
                    break;
                }

                // Thread is holding an old pointer! Spin and wait.
                // In a real implementation, we use yield() or a sleep with exponential backoff.
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#elif defined(__aarch64__) // Apple Silicon (M1/M2/M3/M4)
    // The ARM equivalent of _mm_pause. 
    // Tells the M4 core to yield execution resources to other threads.
    asm volatile("yield" ::: "memory"); 
#else
    std::this_thread::yield();
#endif
            }
        }
    }
};

// ============================================================================
// SIMULATION: Massive Configuration Table Update
// ============================================================================

// A heavy data structure that cannot be trivially copied (unlike Seqlock).
struct RoutingTable {
    std::vector<int> routes;
    std::string version;

    RoutingTable(int size, std::string v) : routes(size, 42), version(v) {}
    ~RoutingTable() {
        // Simulate heavy destruction
        // std::cout << "  [Writer] Physically destroying table: " << version << "\n";
    }
};

// Global RCU Domain
RCUDomain g_rcu_domain;

// Global Atomic Pointer to the current active configuration
std::atomic<RoutingTable*> g_current_table{nullptr};

void reader_thread(size_t tid, std::atomic<bool>& running, std::atomic<uint64_t>& ops) {
    uint64_t local_ops = 0;
    while (running.load(std::memory_order_relaxed)) {

        // --- ENTER RCU CRITICAL SECTION ---
        g_rcu_domain.rcu_read_lock(tid);

        // 1. Safely load the pointer. (Consume/Acquire semantics)
        RoutingTable* table = g_current_table.load(std::memory_order_acquire);

        if  (table) {
            // 2. Safely read from the pointer! 
            // We are GUARANTEED that the writer will not 'delete' this memory
            // until we call rcu_read_unlock().
            volatile int dummy = table->routes[0];
            (void)dummy;
            local_ops++;
        }

        // --- EXIT RCU CRITICAL SECTION ---
        g_rcu_domain.rcu_read_unlock(tid);

        // Simulate doing other work outside the lock
        for (int i = 0; i < 50; ++i) {
            std::atomic_signal_fence(std::memory_order_relaxed);
        }
    }

    ops.fetch_add(local_ops, std::memory_order_relaxed);
}

int main() {
    std::cout << "Initializing HPC Userspace RCU Domain...\n";
    
    // Initial publish
    g_current_table.store(new RoutingTable(10000, "v1.0"), std::memory_order_release);

    std::atomic<bool> running{true};
    std::atomic<uint64_t> total_reads{0};
    std::vector<std::thread> readers;

    const size_t num_readers = 4;
    for (size_t i = 0; i < num_readers; ++i) {
        readers.emplace_back(reader_thread, i, std::ref(running), std::ref(total_reads));
    }

    // Writer logic: Publish a new routing table every 10ms
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 2; i <= 100; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // 1. Create the new data structure in the background (Read-COPY-Update)
        RoutingTable* new_table = new RoutingTable(10000, "v" + std::to_string(i) + ".0");

        // 2. Atomically swap the pointer. Future readers will see the new table.
        // Old readers are still actively looking at the old table.
        RoutingTable* old_table = g_current_table.exchange(new_table, std::memory_order_release);

        // 3. WAIT for Grace Period.
        // Block until all readers who might be looking at `old_table` have called rcu_read_unlock().
        g_rcu_domain.synchronize_rcu();

        // 4. Safely destroy the old table.
        delete old_table; 
    }
    
    running.store(false, std::memory_order_release);
    for (auto& t : readers) {
        t.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    std::cout << "--- RCU Simulation Completed ---\n";
    std::cout << "Time elapsed: " << diff.count() << " seconds\n";
    std::cout << "Total pointer reads achieved: " << total_reads.load() << "\n";
    
    // Clean up final table
    delete g_current_table.load();

    return 0;
}
