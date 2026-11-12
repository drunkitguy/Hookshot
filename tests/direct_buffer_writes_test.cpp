// Actual binding and page-tracking functions, authored pool/address/mirror stubs.
// No guest memory, GPU allocation, Vulkan command, timeline wait or real device.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using VkBuffer = uint64_t;
using VkDeviceMemory = uint64_t;
using VkDeviceAddress = uint64_t;
using VmaAllocation = void*;
constexpr VkBuffer VK_NULL_HANDLE = 0;
#include "direct_write_types.inc"

static Pool pool;
static Pool* last_pool = nullptr;
static std::mutex pools_mu;
static std::atomic<uint64_t> pools_gen{1};
static std::atomic<bool> par_active{false};
static thread_local TlPool tl_pools[1024];
static uint64_t mirror_epoch = 7, mem_rec_value = 21;
static bool mirrors = true, mirror_upload = true, have_pool = true, mapping_valid = true;
static std::vector<Pool*> mirror_wanted;
static std::vector<WriteBack> writebacks;
static std::unordered_map<uint64_t, std::pair<int64_t, int>> direct_by_pool;
static std::unordered_map<uint64_t, uint64_t> direct_bytes_by_pool;
static unsigned lookups, made, synced, texture_writes, hazards;
static int64_t st_async_copies;
static uint64_t hazard_lo, hazard_hi;
static constexpr uint64_t virtual_base = 1ull << 40;
static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct ParLock { ParLock() {} ~ParLock() {} };
static void texture_gpu_wrote(uint64_t, uint64_t) { ++texture_writes; }
static void rb_hazard_locked(uint64_t lo, uint64_t hi) { ++hazards; hazard_lo = lo; hazard_hi = hi; }
static bool mirror_on() { return mirrors; }
static Pool* pool_of(uint64_t guest, uint64_t size) {
    ++lookups;
    return have_pool && guest >= pool.lo && guest <= pool.hi && size <= pool.hi - guest ? &pool : nullptr;
}
static bool mirror_make(Pool&) { ++made; return false; }
static void mirror_sync(Pool& p) { ++synced; p.synced_epoch = mirror_epoch; p.sync_seg = 0; }
static uint64_t texture_segment() { return 0; }
static void publish_cap_ranges() { throw std::runtime_error("unexpected mirror publication"); }
static void logf(const char*, ...) {}
static bool import_in_background(uint64_t, uint64_t) { throw std::runtime_error("unexpected asynchronous import"); }
static BufferRef stream_copy(const uint8_t*, uint64_t) { throw std::runtime_error("unexpected streaming copy"); }
static bool copy_reads = false;   // (RX_VK_COPY_READS: guest memory not imported)
static unsigned copies;
static bool copy_reads_on() { return copy_reads; }
static constexpr uint64_t kCopyReadMax = 8ull << 20;
static std::atomic<int64_t> st_copy_reads{0}, st_copy_read_kb{0};
static std::atomic<int64_t> st_copy_refused[3]{};
static std::atomic<uint64_t> st_copy_refused_largest{0};
static void copy_refusals_log() {}
static constexpr VkBuffer kStreamBuffer = 77;
static BufferRef stream_copy_guest(uint64_t, uint64_t n) {
    if (!copy_reads) throw std::runtime_error("unexpected copied read");
    ++copies;
    return {kStreamBuffer, 256u * copies, n, 0};
}
static uint64_t serial = 1;   // (the recording being made)
static uint64_t recording_serial() { return serial; }
struct CopyMemo { uint64_t gpu = 0, size = 0, serial = 0, wgen = 0; BufferRef ref{}; };
static thread_local CopyMemo copy_memo[256];
static bool replay_wrote = false;   // (a replay write over the range since the copy)
static uint64_t replay_write_now() { return 0; }
static bool replay_wrote_since(uint64_t, uint64_t, uint64_t) { return replay_wrote; }
static const uint8_t* gpu_ptr(uint64_t, uint64_t* avail) {
    *avail = mapping_valid ? 4ull << 20 : 0;
    return mapping_valid ? reinterpret_cast<const uint8_t*>(&pool) : nullptr;
}
static uint64_t gpu_guest(uint64_t gpu) { return gpu >= virtual_base ? pool.lo + (gpu - virtual_base) : gpu; }
struct Host { const uint8_t* (*gpu_ptr)(uint64_t, uint64_t*); uint64_t (*gpu_guest)(uint64_t); };
static Host interface{gpu_ptr, gpu_guest};
static Host* host = &interface;

#include "direct_write_actual.inc"

static void readback_disabled(bool off) {
#ifdef _WIN32
    require(_putenv_s("RX_VK_NO_READBACK_WAIT", off ? "1" : "") == 0, "authored environment failed");
    _putenv_s("RX_VK_MIRROR_DEFER", "");
    _putenv_s("RX_VK_ASYNC_IMPORT", "");
#else
    require((off ? setenv("RX_VK_NO_READBACK_WAIT", "1", 1) : unsetenv("RX_VK_NO_READBACK_WAIT")) == 0,
            "authored environment failed");
    unsetenv("RX_VK_MIRROR_DEFER");
    unsetenv("RX_VK_ASYNC_IMPORT");
#endif
}
static void page_state(uint64_t guest, uint64_t size, bool expected) {
    bool ever = true;
    require(memory_writeback_value(guest, size, &ever) == (expected ? mem_rec_value : 0) && ever == expected,
            "readback page registration missing or unexpected");
    if (expected) {
        require(memory_writeback_value(guest, 1, nullptr) == mem_rec_value &&
                memory_writeback_value(guest + size - 1, 1, nullptr) == mem_rec_value,
                "written range endpoints not registered");
        const uint64_t after = (guest + size + 4095) & ~4095ull;
        ever = true;
        require(memory_writeback_value(after, 1, &ever) == 0 && !ever, "registered a page beyond the written range");
    }
}

int main(int argc, char** argv) {
    const std::string mode = argc == 2 ? argv[1] : "";
    try {
        pool.lo = 0x200000, pool.hi = pool.lo + (4u << 20), pool.buffer = 11, pool.mirror = 22;
        pool.shadow = 33;
        const uint64_t pages = (pool.hi - pool.lo) >> kPageShift;
        pool.shadow_known.assign((pages + 63) / 64, UINT64_MAX);
        uint64_t guest = pool.lo + 4092, gpu = guest, size = 16;
        readback_disabled(mode == "wait_disabled");
        if (mode == "mirror_off" || mode == "virtual_write" || mode == "large_write" ||
            mode == "no_recording" || mode == "wait_disabled" || mode == "readonly") mirrors = false;
        else if (mode == "upload_off") mirror_upload = false;
        else if (mode == "refused") pool.mstate = Pool::MIRROR_REFUSED, size = 1u << 20;
        else if (mode == "mirror_pending") pool.mstate = Pool::MIRROR_WANTED;
        else if (mode == "mirror_on") pool.mstate = Pool::MIRROR_ON;
        else if (mode == "invalid_pool") have_pool = false;
        else if (mode == "copied_read") have_pool = false, copy_reads = true;
        else if (mode == "invalid_mapping") mapping_valid = false, gpu = virtual_base + 4092;
        else throw std::runtime_error("unknown fixture case");
        if (mode == "virtual_write") gpu = virtual_base + 4092;
        if (mode == "large_write") size = (1u << 20) + 4;
        if (mode == "no_recording") mem_rec_value = 0;
        const bool written = mode != "readonly" && mode != "copied_read";
        const BufferRef ref = gpu_buffer_bind(gpu, size, written);
        require(hazards == (mode == "mirror_on" ? 1u : 0u), "changed mirrored-write hazard notification");
        if (hazards) require(hazard_lo == guest && hazard_hi == guest + size, "changed mirrored-write hazard range");
        if (mode == "copied_read") {
            require(ref.buffer == kStreamBuffer && ref.size == size && copies == 1, "a read no pool holds was not copied");
            require(gpu_buffer_bind(gpu, size, false).offset == ref.offset && copies == 1,
                    "a range copied in this recording was copied again");
            replay_wrote = true;
            require(gpu_buffer_bind(gpu, size, false).buffer == kStreamBuffer && copies == 2,
                    "a copy the replay wrote over since was reused");
            replay_wrote = false, serial++;
            require(gpu_buffer_bind(gpu, size, false).buffer == kStreamBuffer && copies == 3,
                    "a copy of an earlier recording was reused");
            require(!gpu_buffer_bind(gpu, size, true) && copies == 3, "a write no pool holds was copied");
            require(!pool.last_value && writebacks.empty(), "a copied read changed pool bookkeeping");
            page_state(guest, size, false);
        } else if (mode == "invalid_pool" || mode == "invalid_mapping") {
            require(!ref && !pool.last_value && !pool.direct_write_epoch && writebacks.empty(),
                    "invalid binding changed pool bookkeeping");
            page_state(guest, size, false);
        } else {
            require(ref.buffer == (mode == "mirror_on" ? pool.mirror : pool.buffer) &&
                    ref.offset == guest - pool.lo && ref.size == size && !ref.address,
                    "changed binding buffer, offset, size or address");
            require(pool.last_value == mem_rec_value, "changed binding lifetime value");
            if (mode == "mirror_on") {
                require(writebacks.size() == 1 && writebacks[0].pool == &pool &&
                        writebacks[0].lo == ref.offset && writebacks[0].hi == ref.offset + size &&
                        !pool.direct_write_epoch && synced == 1, "changed mirrored writeback behavior");
                page_state(guest, size, false);
            } else if (mode == "readonly") {
                require(!pool.direct_write_epoch && !texture_writes && writebacks.empty(), "readonly binding recorded a write");
                page_state(guest, size, false);
                require(gpu_buffer_bind(gpu, size, false).buffer == ref.buffer && lookups == 1,
                        "readonly memo stopped reusing its binding");
                for (auto& m : bind_memo) m = {}; // Exercise the actual readonly TLS-pool shortcut too.
                require(gpu_buffer_bind(gpu, size, false).buffer == ref.buffer && lookups == 1 &&
                        !pool.direct_write_epoch, "readonly TLS shortcut changed write bookkeeping");
                page_state(guest, size, false);
            } else {
                page_state(guest, size, size <= (1u << 20) && mem_rec_value && mode != "wait_disabled");
                require(pool.direct_write_epoch == mirror_epoch, "direct write epoch missing");
                require(writebacks.empty() && !synced, "direct binding entered mirror writeback path");
            }
        }
        require(texture_writes == (written || mode == "copied_read" ? 1u : 0u), "changed existing texture write notification");
        require(!st_async_copies, "unexpected asynchronous streaming copy");
        require(!made, "unexpected mirror allocation attempt");
        const bool forgot = written && have_pool && mapping_valid && (!mirrors || !mirror_upload || mode == "refused");
        const uint64_t off = guest - pool.lo, span = std::min(size, pool.hi - guest);
        for (uint64_t pg = 0; pg < pages; pg++) {
            const bool covered = forgot && span && pg >= (off >> kPageShift) && pg <= ((off + span - 1) >> kPageShift);
            require(bool(pool.shadow_known[pg >> 6] & (1ull << (pg & 63))) == !covered,
                    "direct binding changed the wrong shadow pages");
        }
        std::printf("PASS %s\n", mode.c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL %s: %s\n", mode.c_str(), e.what());
        return 1;
    }
}
