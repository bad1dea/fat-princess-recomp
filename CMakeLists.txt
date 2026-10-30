cmake_minimum_required(VERSION 3.20)
project(FatPrincessNative LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(ZLIB REQUIRED)
find_package(SDL2 REQUIRED)
if(MSVC)
  # The SDL2 package for Visual C++ puts its headers directly in include/, the sources include <SDL2/SDL.h>.
  get_target_property(FP_SDL2_INCLUDE SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
  if(FP_SDL2_INCLUDE AND EXISTS "${FP_SDL2_INCLUDE}/SDL.h" AND NOT EXISTS "${FP_SDL2_INCLUDE}/SDL2/SDL.h")
    file(GLOB FP_SDL2_HEADERS "${FP_SDL2_INCLUDE}/*.h")
    file(COPY ${FP_SDL2_HEADERS} DESTINATION "${CMAKE_BINARY_DIR}/sdl2-shim/SDL2")
    include_directories(BEFORE SYSTEM "${CMAKE_BINARY_DIR}/sdl2-shim")
  endif()
  # SDL_endian.h defines _m_prefetch itself, which recent clang headers define as a builtin as well.
  # The macro is SDL's own guard for that definition.
  add_compile_definitions(__PRFCHWINTRIN_H)
endif()
find_package(OpenGL REQUIRED)
add_executable(fp_assets src/assets.cpp)
target_include_directories(fp_assets SYSTEM PRIVATE third_party/ps3recomp/libs/codec)
target_link_libraries(fp_assets PRIVATE ZLIB::ZLIB SDL2::SDL2)
add_executable(fp_map_viewer src/map_viewer.cpp)
target_link_libraries(fp_map_viewer PRIVATE SDL2::SDL2 OpenGL::GL)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(fp_map_viewer PRIVATE -Wall -Wextra -Wconversion)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(fp_assets PRIVATE -Wall -Wextra -Wconversion)
endif()

option(FP_BUILD_GAME "Build the local, statically recompiled game" OFF)
if(FP_BUILD_GAME)
  option(PS3RECOMP_RSX_VULKAN "Use the framework's Vulkan renderer on Linux" ON)
  if(PS3RECOMP_RSX_VULKAN AND (UNIX OR WIN32) AND NOT APPLE)
    find_package(glslang CONFIG REQUIRED)
  endif()
  set(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ps3recomp" CACHE PATH "Pinned ps3recomp checkout")
  set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/private/lift/game" CACHE PATH "Private game lift")
  # Integrate the pinned upstream scaffold with the project-local boot and HLE adapters.
  add_subdirectory("${PS3RECOMP_DIR}/templates/project" game)
  target_sources(MyGameRecomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/diagnostics.cpp")
  target_sources(MyGameRecomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/platform.cpp")
  target_sources(MyGameRecomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/spurs_queue.cpp")
  # AVX2/FMA3 selects the vector SPU float helpers and SSE4.1 shufb. -ffp-contract=off keeps
  # separate multiplies and adds exactly as before FMA was available to the compiler.
  option(FP_X86_V3 "Compile the game for x86-64-v3 (AVX2, FMA3, BMI2)" ON)
  if(FP_X86_V3 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(MyGameRecomp PRIVATE -mavx2 -mfma -mbmi2 -mmovbe -msse4.1 -ffp-contract=off)
  endif()
  if(MSVC)
    target_compile_options(MyGameRecomp PRIVATE /FI "${CMAKE_CURRENT_SOURCE_DIR}/src/msvc_clock.h")
  endif()
  add_executable(fp_platform_test tests/platform.cpp src/platform.cpp src/spurs_queue.cpp)
  target_include_directories(fp_platform_test PRIVATE "${RECOMP_DIR}")
  set(FP_SPU_LIFT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/private/lift/spu" CACHE PATH
      "Private directory containing the lifted game SPU images and workload registry")
  file(GLOB_RECURSE FP_SPU_SOURCES CONFIGURE_DEPENDS "${FP_SPU_LIFT_DIR}/*.c")
  if(NOT FP_SPU_SOURCES)
    message(FATAL_ERROR "No lifted SPU sources found under ${FP_SPU_LIFT_DIR}")
  endif()
  target_sources(MyGameRecomp PRIVATE ${FP_SPU_SOURCES})
  set_source_files_properties(${FP_SPU_SOURCES} PROPERTIES
    INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
    COMPILE_DEFINITIONS "SPU_USE_DISPATCH_MT;SPU_LS_LEAN")
  set(FP_OVERLAY_DIR "" CACHE PATH "Private lifts of modules at observed local-store addresses")
  if(FP_OVERLAY_DIR)
    file(GLOB_RECURSE FP_OVERLAY_SOURCES CONFIGURE_DEPENDS "${FP_OVERLAY_DIR}/*.c")
    target_sources(MyGameRecomp PRIVATE ${FP_OVERLAY_SOURCES})
    set_source_files_properties(${FP_OVERLAY_SOURCES} PROPERTIES
      INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
      COMPILE_DEFINITIONS "SPU_USE_DISPATCH_MT;SPU_LS_LEAN"
      COMPILE_OPTIONS "-O3")
  endif()
  # The POSIX template uses calloc for the guest arena. Its unaligned base
  # makes vm_commit's mprotect fail, and violates the HLE pointer truncation
  # invariant. Adapt ONLY our generated copy; leave the checkout untouched.
  if(UNIX OR WIN32)
    file(READ "${PS3RECOMP_DIR}/templates/project/main.cpp" FP_BOOT_MAIN)
    if(UNIX)
    set(FP_OLD_ALLOC "vm_base = (uint8_t*)calloc(1, 0xE0000000u);\n    ppu_vm_size = 0xE0000000u;")
    string(FIND "${FP_BOOT_MAIN}" "${FP_OLD_ALLOC}" FP_ALLOC_POS)
    if(FP_ALLOC_POS LESS 0)
      message(FATAL_ERROR "Pinned upstream POSIX allocation changed; review the adapter")
    endif()
    string(REPLACE "${FP_OLD_ALLOC}"
      "if (vm_init() != CELL_OK) return false;\n    if (mprotect(vm_base, (size_t)VM_TOTAL_SIZE, PROT_READ | PROT_WRITE) != 0) { vm_shutdown(); return false; }\n    ppu_vm_size = 0;\n    fprintf(stderr, \"[fp-vm] aligned guest arena: base=%p span=4GiB\\n\", (void*)vm_base);"
      FP_BOOT_MAIN "${FP_BOOT_MAIN}")
    string(REPLACE "#include <string.h>" "#include <string.h>\n#include \"vm.h\"" FP_BOOT_MAIN "${FP_BOOT_MAIN}")
    endif()
    string(REPLACE "lv2_init_syscalls();" "lv2_init_syscalls();\n    extern void fp_platform_init_cpp();\n    fp_platform_init_cpp();" FP_BOOT_MAIN "${FP_BOOT_MAIN}")
    string(APPEND FP_BOOT_MAIN "\nextern \"C\" void fp_platform_init(void);\nvoid fp_platform_init_cpp() { fp_platform_init(); }\n")
    if(PS3RECOMP_RSX_VULKAN AND NOT APPLE)
      target_sources(MyGameRecomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/host.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/settings.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/game_controls.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/self_test.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/overlay_stats.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/patches.cpp")
      # Route guards default off; the AI settings menu or FP_AI_NAV_GRAPH enables them.
      target_sources(MyGameRecomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/ai_route_policy.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/ai_ground_route.cpp")
      if(WIN32)
        target_sources(MyGameRecomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer_d3d12.cpp")
      endif()
      file(READ "${CMAKE_CURRENT_SOURCE_DIR}/assets/ui/LiberationSans-Regular.ttf" FP_UI_FONT_HEX HEX)
      string(REGEX REPLACE "(..)" "0x\\1," FP_UI_FONT_BYTES "${FP_UI_FONT_HEX}")
      file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/fp_ui_font_data.h"
        CONTENT "static const unsigned char fp_ui_font_data[] = {${FP_UI_FONT_BYTES}};\n")
      target_sources(MyGameRecomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/osk_panel.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/trophy_ui.cpp")
      set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/osk_panel.cpp" TARGET_DIRECTORY MyGameRecomp PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_BINARY_DIR}")
      # The build id and secret the psnr server checks (its builds.json). The secret lives outside git and is made
      # on first configure; the id is the date and commit of the configure.
      set(FP_SECRET_FILE "${CMAKE_CURRENT_SOURCE_DIR}/private/psnr_build_secret.txt")
      if(NOT EXISTS "${FP_SECRET_FILE}")
        string(RANDOM LENGTH 48 ALPHABET 0123456789abcdef FP_NEW_SECRET)
        file(WRITE "${FP_SECRET_FILE}" "${FP_NEW_SECRET}\n")
      endif()
      file(STRINGS "${FP_SECRET_FILE}" FP_BUILD_SECRET LIMIT_COUNT 1)
      execute_process(COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --short HEAD
                      OUTPUT_VARIABLE FP_GIT_SHORT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
      string(TIMESTAMP FP_BUILD_DATE "%Y%m%d")
      set(FP_BUILD_ID "fp-${FP_BUILD_DATE}-${FP_GIT_SHORT}")
      string(SUBSTRING "${FP_BUILD_ID}" 0 16 FP_BUILD_ID)
      set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/settings.cpp" TARGET_DIRECTORY MyGameRecomp PROPERTIES
        COMPILE_DEFINITIONS "FP_BUILD_ID=\"${FP_BUILD_ID}\";FP_BUILD_SECRET=\"${FP_BUILD_SECRET}\"")
      # The settings menu's DEBUG tab (trophy card preview, test pages). Configure release packages with -DFP_DEBUG_MENU=OFF.
      option(FP_DEBUG_MENU "Settings menu DEBUG tab" ON)
      if(FP_DEBUG_MENU)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/settings.cpp" TARGET_DIRECTORY MyGameRecomp APPEND PROPERTY COMPILE_DEFINITIONS FP_DEBUG_MENU=1)
      endif()
      target_link_libraries(MyGameRecomp PRIVATE ZLIB::ZLIB SDL2::SDL2)
      string(REPLACE "rsx_null_backend_" "rsx_vulkan_backend_" FP_BOOT_MAIN "${FP_BOOT_MAIN}")
      foreach(FP_CALL init present pump_messages)
        string(REPLACE "rsx_vulkan_backend_${FP_CALL}" "fp_backend_${FP_CALL}" FP_BOOT_MAIN "${FP_BOOT_MAIN}")
      endforeach()
      # The template picks the D3D12 backend under _WIN32. The window and renderer here are Vulkan, so route that branch
      # through the same host entry points; otherwise every guest flip goes to a D3D12 present that was never initialised.
      foreach(FP_CALL init present pump_messages)
        string(REPLACE "rsx_d3d12_backend_${FP_CALL}" "fp_backend_${FP_CALL}" FP_BOOT_MAIN "${FP_BOOT_MAIN}")
      endforeach()
      string(REPLACE "\"D3D12\"" "\"Vulkan\"" FP_BOOT_MAIN "${FP_BOOT_MAIN}")
      string(REPLACE "null (headless software)" "Vulkan" FP_BOOT_MAIN "${FP_BOOT_MAIN}")
      string(REPLACE "g_ps3_guest_caller = harness_guest_caller;"
        "g_ps3_guest_caller = harness_guest_caller;\n    extern void fp_host_prepare_cpp();\n    fp_host_prepare_cpp();"
        FP_BOOT_MAIN "${FP_BOOT_MAIN}")
      string(APPEND FP_BOOT_MAIN "\nextern \"C\" void fp_host_prepare(void);\nvoid fp_host_prepare_cpp() { fp_host_prepare(); }\n")
      string(REPLACE "rsx_ok = 0;              /* window closed */\n                continue;"
        "fprintf(stderr, \"[fp-host] window closed\\n\");\n                exit(0);"
        FP_BOOT_MAIN "${FP_BOOT_MAIN}")
    endif()
    string(REPLACE "static DWORD WINAPI frame_clock(LPVOID)"
      "static DWORD WINAPI legacy_frame_clock(LPVOID);\nextern \"C\" unsigned long fp_run_frame_clock(void (*)(void));\nstatic DWORD WINAPI frame_clock(LPVOID p) { if (getenv(\"FP_LEGACY_PRESENT\")) return legacy_frame_clock(p); return (DWORD)fp_run_frame_clock(present_guest_frame); }\nstatic DWORD WINAPI legacy_frame_clock(LPVOID)"
      FP_BOOT_MAIN "${FP_BOOT_MAIN}")
    string(REPLACE "InterlockedIncrement(&g_frames_presented);"
      "unsigned fp_count = (unsigned)InterlockedIncrement(&g_frames_presented);\n    extern void fp_report_game_frame_cpp(unsigned);\n    fp_report_game_frame_cpp(fp_count);"
      FP_BOOT_MAIN "${FP_BOOT_MAIN}")
    string(APPEND FP_BOOT_MAIN "\nextern \"C\" void fp_report_game_frame(unsigned);\nvoid fp_report_game_frame_cpp(unsigned n) { fp_report_game_frame(n); }\n")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/fp_boot_main.cpp" "${FP_BOOT_MAIN}")
    get_target_property(FP_GAME_SOURCES MyGameRecomp SOURCES)
    list(REMOVE_ITEM FP_GAME_SOURCES main.cpp)
    set_property(TARGET MyGameRecomp PROPERTY SOURCES "${FP_GAME_SOURCES}")
    target_sources(MyGameRecomp PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/fp_boot_main.cpp")
  endif()
  set_target_properties(MyGameRecomp PROPERTIES OUTPUT_NAME fp_native
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()
