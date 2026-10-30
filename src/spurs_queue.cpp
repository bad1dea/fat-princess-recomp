#define PPU_NO_INLINE_LOADS
#include "ppu_recomp.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

extern "C" void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*));
extern "C" void spu_lockline_lock(void);
extern "C" void spu_lockline_unlock(void);
extern "C" void spu_coh_notify_write(uint32_t);
extern "C" void spu_taskset_signal_task(uint32_t, uint32_t);

namespace {
constexpr uint32_t invalid = 0x80410902, busy = 0x80410901;
constexpr uint32_t null_pointer = 0x80410911, alignment = 0x80410910, bad_state = 0x80410909;

// SDK layout verified in the user's libsre: initializer +0x164C0,
// producer +0x169D0, consumer +0x16CFC. All fields are big endian.
uint32_t read32(uint32_t ea) {
    uint32_t v; std::memcpy(&v, vm_base + ea, 4); return __builtin_bswap32(v);
}
uint64_t read64(uint32_t ea) {
    return (static_cast<uint64_t>(read32(ea)) << 32) | read32(ea + 4);
}
void write32(uint32_t ea, uint32_t v) {
    v = __builtin_bswap32(v); std::memcpy(vm_base + ea, &v, 4);
}
void write64(uint32_t ea, uint64_t v) {
    write32(ea, static_cast<uint32_t>(v >> 32)); write32(ea + 4, static_cast<uint32_t>(v));
}
bool range(uint32_t ea, uint64_t bytes) {
    return ea >= 0x10000 && static_cast<uint64_t>(ea) + bytes <= 0xe0000000ull;
}
void initialize(ppu_context* ctx) {
    uint32_t spurs = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t taskset = static_cast<uint32_t>(ctx->gpr[4]);
    const uint32_t q = static_cast<uint32_t>(ctx->gpr[5]), buffer = static_cast<uint32_t>(ctx->gpr[6]);
    const uint32_t size = static_cast<uint32_t>(ctx->gpr[7]), depth = static_cast<uint32_t>(ctx->gpr[8]);
    const uint32_t direction = static_cast<uint32_t>(ctx->gpr[9]);
    ctx->gpr[3] = null_pointer;
    if ((!spurs && !taskset) || !q || (size && !buffer)) return;
    ctx->gpr[3] = alignment;
    if ((spurs | taskset | q) & 127 || buffer & 15) return;
    ctx->gpr[3] = invalid;
    if (size > 0x4000 || size & 15 || !depth || depth > 0x3fffffff || direction > 2 ||
        !range(q, 128) || !range(buffer, static_cast<uint64_t>(size) * depth)) return;
    if (taskset) {
        if (!range(taskset, 0x80) || vm_read32(taskset + 0x74) > 31) return;
        spurs = vm_read32(taskset + 0x64);
    }
    std::memset(vm_base + q, 0, 128);
    write32(q + 8, size); write32(q + 12, depth); write64(q + 16, buffer);
    write32(q + 24, UINT32_MAX); write32(q + 28, direction);
    write64(q + 0x60, taskset); write64(q + 0x68, spurs);
    ctx->gpr[3] = 0;
    std::fprintf(stderr, "[fp-queue] init q=0x%08X buffer=0x%08X size=%u depth=%u direction=%u\n",
                 q, buffer, size, depth, direction);
}

// The host wait polls the actual shared ring rather than registering an LV2
// waiter. It completes only after a producer publishes a real element (or a
// consumer frees space). SPU transactions use the same lock-line mutex, and
// notification invalidates their reservations exactly as a PPU store does.
// FP_QUEUE_WAIT_PROF=1: blocking queue waits (count, total ms) per direction, every 10 s.
void queue_wait_note(bool push, double ms) {
    static const bool on = std::getenv("FP_QUEUE_WAIT_PROF") != nullptr;
    if (!on) return;
    static std::atomic<uint64_t> n[2], us[2];
    static std::atomic<uint64_t> next{0};
    n[push]++; us[push] += uint64_t(ms * 1000);
    const uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t due = next.load();
    if (!due) { next.compare_exchange_strong(due, now + 10000); return; }
    if (now >= due && next.compare_exchange_strong(due, now + 10000)) {
        for (int d = 0; d < 2; d++)
            std::fprintf(stderr, "[queuewait] %s n=%llu total_ms=%.1f\n", d ? "push" : "pop",
                         (unsigned long long)n[d].exchange(0), us[d].exchange(0) / 1000.0);
    }
}
void transfer(ppu_context* ctx, bool push) {
    const uint32_t q = static_cast<uint32_t>(ctx->gpr[3]), data = static_cast<uint32_t>(ctx->gpr[4]);
    const bool peek = !push && (ctx->gpr[5] & 255);
    const bool block = (ctx->gpr[push ? 5 : 6] & 255) != 0;
    ctx->gpr[3] = null_pointer;
    if (!q || !data) return;
    ctx->gpr[3] = alignment;
    if (q & 127) return;
    ctx->gpr[3] = invalid;
    if (!range(q, 128)) return;
    static std::atomic_uint messages{0};
    static std::atomic_uint wait_messages{0};
    static const char* wait_env = std::getenv("FP_QUEUE_SPIN_US");
    static const long spin_us = wait_env ? std::atol(wait_env) : 1000;
    const auto wait_start = std::chrono::steady_clock::now();
    bool waited = false;
    for (;;) {
        spu_lockline_lock();
        const uint32_t direction = read32(q + 28), size = read32(q + 8), depth = read32(q + 12);
        const uint32_t buffer = read32(q + 20);
        if (direction != (push ? 2u : 1u) || !size || size > 0x4000 || !depth || depth > 0x3fffffff ||
            !range(buffer, static_cast<uint64_t>(size) * depth) || !range(data, size)) {
            spu_lockline_unlock(); ctx->gpr[3] = bad_state; return;
        }
        const uint32_t raw_head = read32(q), raw_tail = read32(q + 4);
        const uint32_t head = static_cast<int32_t>(raw_head) < 0 ? ~raw_head : raw_head;
        const uint32_t tail = static_cast<int32_t>(raw_tail) < 0 ? ~raw_tail : raw_tail;
        if (head >= depth * 2 || tail >= depth * 2) {
            spu_lockline_unlock(); ctx->gpr[3] = bad_state; return;
        }
        const uint32_t count = tail >= head ? tail - head : tail + depth * 2 - head;
        const bool locked = static_cast<int32_t>(push ? raw_tail : raw_head) < 0;
        const bool available = push ? count < depth : count > 0;
        if (!locked && available) {
            const uint32_t index = push ? tail : head, slot = index % depth;
            uint8_t* entry = vm_base + buffer + static_cast<size_t>(slot) * size;
            if (push) std::memcpy(entry, vm_base + data, size);
            else std::memcpy(vm_base + data, entry, size);
            if (!peek) write32(q + (push ? 4 : 0), (index + 1) % (depth * 2));
            const uint32_t waiter = q + (push ? 0x20 : 0x30);
            const bool wake = !peek && vm_base[waiter] && (push ? count == 0 : count == depth);
            const uint32_t task = vm_base[waiter + 1], taskset = read32(q + 0x64);
            if (!peek) spu_coh_notify_write(q);
            spu_lockline_unlock();
            if (wake && taskset) spu_taskset_signal_task(taskset, task);
            ctx->gpr[3] = 0;
            if (waited) queue_wait_note(push, std::chrono::duration<double, std::milli>(
                                                  std::chrono::steady_clock::now() - wait_start).count());
            if (messages.fetch_add(1) < 24)
                std::fprintf(stderr, "[fp-queue] %s q=0x%08X slot=%u bytes=%u occupancy=%u%s\n",
                    push ? "push" : "pop", q, slot, size, count, peek ? " (peek)" : "");
            return;
        }
        spu_lockline_unlock();
        if (block && std::getenv("SPURS_QUEUE_WAIT_TRACE") && wait_messages.fetch_add(1) < 32) {
            const uint32_t port = read32(q + 0x18);
            const uint32_t event_queue = read32(q + 0x74);
            std::fprintf(stderr,
                "[fp-queue-wait] %s q=0x%08X buffer=0x%08llX size=%u depth=%u count=%u port=%u eventq=%u\n",
                push ? "push" : "pop", q, (unsigned long long)read64(q + 0x10), size, depth, count,
                port, event_queue);
        }
        if (!block) { ctx->gpr[3] = busy; return; }
        waited = true;
        // A 1 ms sleep per poll adds a millisecond to every queue handoff with an SPU job.
        // FP_QUEUE_SPIN_US spins that long before falling back to short sleeps (0 = old 1 ms, default 1000).
        if (spin_us <= 0) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
        if (std::chrono::steady_clock::now() - wait_start < std::chrono::microseconds(spin_us))
            std::this_thread::yield();
        else
            std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}
void pop(ppu_context* ctx) { transfer(ctx, false); }
void push(ppu_context* ctx) { transfer(ctx, true); }
}

extern "C" void fp_spurs_queues_init() {
    ps3_hle_register_ctx(0x082bfb09, "_cellSpursQueueInitialize", initialize);
    ps3_hle_register_ctx(0x91066667, "cellSpursQueuePopBody", pop);
    ps3_hle_register_ctx(0x92cff6ed, "cellSpursQueuePushBody", push);
}
