#include <cstdint>
#include <cstdio>
#include <windows.h>
#include "../third_party/ps3recomp/libs/video/rsx_live_draw.h"
#include "../third_party/ps3recomp/libs/video/rsx_null_backend.h"

extern "C" uint8_t* vm_base;
extern "C" uint32_t cellGcmResolveLocated(int, uint32_t);
extern "C" uint32_t cellGcmResolveIO(uint32_t);
extern "C" uint32_t cellGcmGetCurrentDisplayBufferId(void);

static const uint8_t* guest_pixels(void*, uint32_t location, uint32_t offset, uint32_t bytes) {
    if (!vm_base) return nullptr;
    uint32_t address = location == 0 ? cellGcmResolveLocated(1, offset) : cellGcmResolveIO(offset);
    if (location != 0 && !address) address = cellGcmResolveLocated(0, offset);
    if (!address || address == UINT32_MAX || uint64_t(address) + bytes > 0x100000000ull) return nullptr;
    return vm_base + address;
}

extern "C" int fp_d3d12_init(uint32_t width, uint32_t height, const char* title) {
    if (rsx_null_backend_init(width, height, title) != 0) return -1;
    if (!rsx_live_draw_enabled() || rsx_live_draw_init(rsx_null_backend_get_hwnd(), width, height, guest_pixels, nullptr) != 0) {
        rsx_null_backend_shutdown();
        return -1;
    }
    rsx_null_backend_suppress_present(1);
    std::fprintf(stderr, "[fp-gfx] live D3D12 engine ready; guest flip selects the display buffer\n");
    return 0;
}

extern "C" void fp_d3d12_present(void) {
    rsx_live_draw_present(cellGcmGetCurrentDisplayBufferId());
}
extern "C" int fp_d3d12_pump_messages(void) { return rsx_null_backend_pump_messages(); }
extern "C" void* fp_d3d12_get_hwnd(void) { return rsx_null_backend_get_hwnd(); }
extern "C" void fp_d3d12_set_fullscreen(int on) { rsx_null_backend_set_fullscreen(on); }
extern "C" int fp_d3d12_set_output_size(uint32_t width, uint32_t height) {
    if (rsx_live_draw_set_output_size(width, height) != 0) return -1;
    if (rsx_null_backend_set_output_size(width, height) != 0)
        std::fprintf(stderr, "[fp-gfx] output resized; window manager kept the client size\n");
    return 0;
}
extern "C" void fp_d3d12_set_overlay_anchor(int anchor) { rsx_live_draw_set_overlay_anchor(anchor); }
extern "C" void fp_d3d12_set_overlay_radius(uint32_t radius) { rsx_live_draw_set_overlay_radius(radius); }
extern "C" void fp_d3d12_set_notify_overlay(const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t radius) {
    rsx_live_draw_set_notify_overlay(pixels, width, height, radius);
}
extern "C" void fp_d3d12_set_debug_overlay(const uint8_t* pixels, uint32_t width, uint32_t height, const char* title) {
    rsx_live_draw_set_debug_overlay(pixels, width, height);
    if (title) SetWindowTextA(static_cast<HWND>(rsx_null_backend_get_hwnd()), title);
}
