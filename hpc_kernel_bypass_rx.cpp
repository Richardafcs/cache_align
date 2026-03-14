/**
 * @file hpc_kernel_bypass_rx.cpp
 * @brief Production-grade Kernel Bypass Network Polling Engine.
 * * Simulates a DPDK/Solarflare zero-copy RX ring buffer mapped to Userspace.
 * * Uses hardware Ownership Bits for lock-free NIC-to-CPU synchronization.
 * * Explains Intel DDIO (L3 Cache injection) optimization.
 */

#include <cstdint>
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <vector>
#include <algorithm>

#if defined(__cpp_lib_hardware_interference_size)
    constexpr size_t CACHE_LINE_SIZE = std::hardware_destructive_interference_size;
#else
    constexpr size_t CACHE_LINE_SIZE = 64;
#endif

// Hardware pause instruction to prevent pipeline starvation
#if defined(__x86_64__) || defined(_M_X64)
    #include <immintrin.h>
    #define CPU_PAUSE() _mm_pause()
#else
    #define CPU_PAUSE() std::this_thread::yield()
#endif

// ============================================================================
// HARDWARE DESCRIPTOR DEFINITION (NIC RX RING)
// ============================================================================

/**
 * @struct RXDescriptor
 * @brief Represents a single slot in the hardware network card's receive ring.
 * * EXACTLY 64 bytes (one cache line) to align with PCIe burst sizes.
 */
struct alignas(CACHE_LINE_SIZE) RXDescriptor {
    // ------------------------------------------------------------------------
    // THE OWNERSHIP BIT (Hardware-to-Software Synchronization)
    // 0 = Software owns the buffer (NIC is allowed to write a packet here).
    // 1 = Hardware owns the buffer (NIC has written a packet, CPU can read it).
    // ------------------------------------------------------------------------
    volatile uint8_t hw_ownership_bit;

    uint8_t reserved[1];
    uint16_t packet_length;
    uint32_t hardware_timestamp; // NIC hardware nanosecond timestamp

    // The actual raw packet payload (Ethernet + IP + UDP/TCP + Payload)
    // 56 bytes left in the cache line (fits a standard small market data UDP packet).
    // If the packet is larger, the NIC scatters it across multiple descriptors.
    uint8_t payload[56];
};

// ============================================================================
// KERNEL BYPASS POLLING ENGINE
// ============================================================================

class ZeroCopyNetworkPoller {
    static constexpr size_t RING_SIZE = 4096;
    static constexpr size_t MASK = RING_SIZE - 1;

    // Pointer to the memory-mapped PCIe ring buffer.
    // In reality, this is allocated via hugepages and mmap'd to the NIC's physical address.
    RXDescriptor* rx_ring_;

    // Software's local tracking index
    size_t current_rx_index_{0};
    size_t hw_rx_index_{0};

public:
    ZeroCopyNetworkPoller() {
        // Simulate mapping hardware memory. 
        // We use aligned_alloc to ensure the start of the array sits perfectly 
        // on a cache line boundary, mirroring how a NIC expects physical memory.
        rx_ring_ = static_cast<RXDescriptor*>(
            std::aligned_alloc(CACHE_LINE_SIZE, RING_SIZE * sizeof(RXDescriptor))
        );
        if (!rx_ring_) {
            throw std::bad_alloc();
        }

        // Initialize the ring, giving ownership of all slots to the hardware (NIC)
        // by setting the ownership bit to 0.
        for (size_t i = 0; i < RING_SIZE; ++i) {
            rx_ring_[i].hw_ownership_bit = 0;
            rx_ring_[i].packet_length = 0;
        }
    }

    ~ZeroCopyNetworkPoller() {
        std::free(rx_ring_);
    }

    /**
     * @brief The ultra-low-latency busy-polling loop.
     * @param handler A lambda/callback to process the packet inline.
     */
    template<typename Handler>
    void poll_forever(Handler&& handler, std::atomic<bool>& running) {

        // Pin this thread to an isolated CPU core (e.g., Core 4).
        // In production, you would use pthread_setaffinity_np here.
        std::cout << "Starting zero-copy polling loop on isolated core...\n";

        while (running.load(std::memory_order_relaxed)) {

            // 1. Locate the current descriptor in the ring buffer
            RXDescriptor* desc = &rx_ring_[current_rx_index_];

            // 2. Poll the Ownership Bit.
            // This is NOT an atomic instruction! We are just reading raw memory.
            // Thanks to Intel DDIO, when the NIC writes the packet over PCIe, 
            // it lands directly in this CPU core's L3 cache. This read takes ~1ns.
            if (desc->hw_ownership_bit == 1) {

                // 3. Memory barrier to prevent speculative execution of packet 
                // processing BEFORE we confirm the hardware actually gave it to us.
                std::atomic_thread_fence(std::memory_order_acquire);

                // 4. We have a packet! Process it INLINE (Zero Copy).
                // We pass a pointer directly to the memory-mapped NIC buffer.
                handler(desc->payload, desc->packet_length, desc->hardware_timestamp);

                // 5. Return ownership of this slot back to the physical NIC hardware.
                // Memory barrier ensures our processing is done before we yield.
                std::atomic_thread_fence(std::memory_order_release);
                desc->hw_ownership_bit = 0;

                // 6. Advance the software index
                current_rx_index_ = (current_rx_index_ + 1) & MASK;
            } else {
                // No packet arrived yet.
                // Pause the pipeline to save power and prevent thermal throttling,
                // but do NOT sleep or yield to the OS!
                CPU_PAUSE(); 
            }
        }
    }

    // --- SIMULATION ONLY ---
    // Simulates the physical NIC hardware injecting a packet via PCIe DMA.
    void hardware_inject_packet(const char* data, uint32_t hw_ts) {
        RXDescriptor* desc = &rx_ring_[hw_rx_index_];
        while (desc->hw_ownership_bit == 1) {
            CPU_PAUSE();
        }

        // NIC writes the payload
        size_t len = std::min(strlen(data), static_cast<size_t>(56));
        std::memcpy(desc->payload, data, len);
        desc->packet_length = static_cast<uint16_t>(len);
        desc->hardware_timestamp = hw_ts;

        // NIC flips the ownership bit, instantly alerting the spinning CPU core
        // (Memory barrier simulates PCIe ordering guarantees)
        std::atomic_thread_fence(std::memory_order_release);
        desc->hw_ownership_bit = 1;

        // Advance hardware index
        hw_rx_index_ = (hw_rx_index_ + 1) & MASK;
    }
};

// ============================================================================
// SIMULATION HARNESS
// ============================================================================

int main() {
    ZeroCopyNetworkPoller nic_interface;
    std::atomic<bool> running{true};
    std::atomic<int> packets_processed{0};
    constexpr int k_packets = 50;

    // --- START THE CPU POLLING THREAD ---
    // This thread acts as the Application / Strategy Engine.
    std::thread app_thread([&]() {
        auto packet_handler = [&](const uint8_t* payload, uint16_t len, uint32_t hw_ts) {
            // Processing directly from the "NIC" memory!
            // No malloc, no copy, no OS involvement.
            packets_processed.fetch_add(1, std::memory_order_relaxed);
            
            // In a real system, we'd parse the UDP/IP header and trigger a trade here.
        };

        // Spin forever at 100% CPU utilization
        nic_interface.poll_forever(packet_handler, running);
    });

    // --- START THE HARDWARE SIMULATION THREAD ---
    // This simulates the physical Network Card sending pulses over PCIe.
    std::cout << "Simulating PCIe DMA hardware injections...\n";
    for (int i = 0; i < k_packets; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); // 10ms network gap
        nic_interface.hardware_inject_packet("MARKET_TICK_AAPL_150.00", i * 1000);
    }

    while (packets_processed.load(std::memory_order_relaxed) < k_packets) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Shutdown gracefully
    running.store(false, std::memory_order_release);
    app_thread.join();

    std::cout << "Total packets processed: " << packets_processed.load() << "\n";
    std::cout << "Zero-copy network layer successfully simulated.\n";

    return 0;
}
