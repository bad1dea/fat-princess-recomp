#define PPU_NO_INLINE_LOADS
#include "ppu_recomp.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

static std::vector<uint8_t> memory(0x400000);
extern "C" { uint8_t* vm_base = memory.data(); }
static std::map<uint32_t, void (*)(ppu_context*)> handlers;
static std::mutex line;
static unsigned wakes, last_task, last_taskset;
extern "C" void fp_platform_init();
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char*, void (*fn)(ppu_context*)) { handlers[nid] = fn; }
extern "C" void spu_lockline_lock() { line.lock(); }
extern "C" void spu_lockline_unlock() { line.unlock(); }
extern "C" void spu_coh_notify_write(uint32_t) {}
extern "C" void spu_taskset_signal_task(uint32_t ts, uint32_t task) { ++wakes; last_taskset = ts; last_task = task; }
extern "C" uint8_t vm_read8(uint64_t a) { return memory.at(a); }
extern "C" uint16_t vm_read16(uint64_t a) { return static_cast<uint16_t>(vm_read8(a) * 256 + vm_read8(a + 1)); }
extern "C" uint32_t vm_read32(uint64_t a) { return static_cast<uint32_t>(vm_read16(a)) * 65536 + vm_read16(a + 2); }
extern "C" void vm_write8(uint64_t a, uint8_t v) { memory.at(a) = v; }
extern "C" void vm_write16(uint64_t a, uint16_t v) { vm_write8(a, v >> 8); vm_write8(a + 1, v); }
extern "C" void vm_write32(uint64_t a, uint32_t v) { vm_write16(a, v >> 16); vm_write16(a + 2, v); }

static void check(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
static uint32_t call(uint32_t nid, std::initializer_list<uint64_t> args) {
    ppu_context ctx{}; unsigned reg = 3;
    for (auto value : args) ctx.gpr[reg++] = value;
    check(handlers.count(nid), "missing platform handler"); handlers.at(nid)(&ctx);
    return static_cast<uint32_t>(ctx.gpr[3]);
}

int main(int argc, char** argv) {
    try {
        fp_platform_init();
        constexpr uint32_t elf = 0x10000, q = 0x20000, buffer = 0x21000, data = 0x22000;
        vm_write32(elf, 0x7f454c46); vm_write8(elf + 4, 1); vm_write8(elf + 5, 2);
        vm_write16(elf + 18, 23); vm_write32(elf + 28, 52); vm_write16(elf + 42, 32); vm_write16(elf + 44, 1);
        vm_write32(elf + 52, 1); vm_write32(elf + 52 + 12, 0x3000); vm_write32(elf + 52 + 16, 0x800);
        check(call(0x82f294b2, {elf}) == 0, "ordinary SPU ELF must need no overlay table");
        vm_write32(elf + 52 + 12, 0x42000); vm_write32(elf + 52 + 16, 0x100);
        check(call(0x82f294b2, {elf}) == 0x2100, "overlay size follows physical file extent");
        check(call(0x82f294b2, {0}) == 0x80410402, "bad ELF must return OVIS invalid");

        check(call(0x082bfb09, {0x30000, 0, q, buffer, 16, 2, 1}) == 0, "initialize SPU-to-PPU queue");
        check(vm_read32(q + 8) == 16 && vm_read32(q + 12) == 2 && vm_read32(q + 20) == buffer &&
              vm_read32(q + 24) == UINT32_MAX && vm_read32(q + 28) == 1, "firmware queue header offsets");
        std::memset(vm_base + data, 0xab, 16);
        check(call(0x91066667, {q, data, 0, 0}) == 0x80410901, "empty nonblocking pop must report BUSY");
        check(vm_read32(data) == 0xabababab && vm_read32(q) == 0, "empty pop must preserve output and ring");
        vm_write32(buffer, 0x12345678); vm_write32(q + 4, 1);
        check(call(0x91066667, {q, data, 1, 0}) == 0 && vm_read32(q) == 0 && vm_read32(data) == 0x12345678,
              "peek returns real bytes without consuming");
        check(call(0x91066667, {q, data, 0, 0}) == 0 && vm_read32(q) == 1, "pop consumes published entry");
        auto waiter = std::async(std::launch::async, [&] { return call(0x91066667, {q, data, 0, 1}); });
        check(waiter.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout, "blocking pop must wait");
        {
            std::lock_guard<std::mutex> lock(line);
            vm_write32(buffer + 16, 0xdeadbeef); vm_write32(q + 4, 2);
        }
        check(waiter.wait_for(std::chrono::seconds(1)) == std::future_status::ready && waiter.get() == 0 &&
              vm_read32(data) == 0xdeadbeef, "blocking pop returns only after publication");
        check(call(0x082bfb09, {0x30000, 0, q, buffer, 16, 2, 2}) == 0, "initialize PPU-to-SPU queue");
        vm_write32(q + 0x64, 0x38000); vm_write8(q + 0x20, 1); vm_write8(q + 0x21, 7);
        vm_write32(data, 0xabcdef01);
        check(call(0x92cff6ed, {q, data, 0}) == 0 && vm_read32(buffer) == 0xabcdef01 && vm_read32(q + 4) == 1,
              "push publishes real bytes and correct tail");
        check(wakes == 1 && last_taskset == 0x38000 && last_task == 7 && vm_read8(q + 0x20) == 1,
              "push signals waiter without corrupting its task-owned list");
        check(call(0x92cff6ed, {q, data, 0}) == 0 && call(0x92cff6ed, {q, data, 0}) == 0x80410901,
              "full queue reports BUSY");
        vm_write32(q, 1);
        check(call(0x92cff6ed, {q, data, 0}) == 0 && vm_read32(q + 4) == 3, "ring reuses consumed slot");
        vm_write32(q, 2);
        check(call(0x92cff6ed, {q, data, 0}) == 0 && vm_read32(q + 4) == 0, "tail wraps modulo twice depth");
        for (int i = 1; i < argc; ++i) {
            std::ifstream input(argv[i], std::ios::binary);
            check(static_cast<bool>(input), "cannot open local SPU ELF");
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
            check(bytes.size() < memory.size() - 0x100000, "SPU ELF exceeds test arena");
            std::memcpy(vm_base + 0x100000, bytes.data(), bytes.size());
            check(call(0x82f294b2, {0x100000}) == 0, "this game's SPU ELF must need no overlay table");
        }
        std::puts("platform ABI tests passed: OVIS size, queue layout, empty/full, peek, blocking, signal, wrap");
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
