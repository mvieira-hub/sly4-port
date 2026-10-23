// sly4.exe: native PC port of Sly Cooper: Thieves in Time (BCES01284).
// Loads the decrypted EBOOT into the PS3 address space and, when the build includes the
// recompiled game code (generated/ppu), starts the game's main thread on the phase 3
// kernel; the Vulkan window of phase 0 runs alongside.
//
//   sly4.exe --game <PS3_GAME>       loads <PS3_GAME>\USRDIR\EBOOT.elf
//   sly4.exe --eboot <EBOOT.elf>     loads that file
//   sly4.exe ... --no-window         no window: runs the game until it exits (or --seconds)
//   sly4.exe ... --no-run            loads only, does not start the game
//   sly4.exe ... --seconds N         stops the game after N seconds and prints the summary
//   sly4.exe ... --trace             logs every library call and syscall, not just the first
//   sly4.exe ... --firmware <dir>    decrypted firmware modules run as they are (libsre.prx: SPURS)
//   sly4.exe ... --ring N            prints the last N calls (kept in memory) with the summary
//   sly4.exe ... --spu-cache f.bin   records the SPU blocks run in the interpreter (for sly4_recomp spu-emit)
//   sly4.exe ... --spu-interp        runs every SPU instruction in the interpreter, even when compiled
//   sly4.exe ... --rsx-timeline      logs the RSX's long waits and flips with absolute times
//   sly4.exe ... --thread-timeline NAME  logs the calls of PPU thread NAME with absolute times
//   sly4.exe ... --call-times        time spent inside each HLE call and syscall, per thread
//   sly4.exe ... --spu-profile       samples the SPUs' PCs and prints the hottest with the summary
//   sly4.exe ... --ppu-profile NAME  samples where the PPU thread NAME runs in sly4.exe (resolve with nm)
//   sly4.exe ... --verify-zlib       checks every block the SPU EDGE zlib task inflates (with --firmware)
//   sly4.exe ... --rsx-capture N,f.txt  writes the RSX commands of frame N (counted in flips)
//   sly4.exe ... --render            with --no-window: renders anyway (Vulkan without a window)
//   sly4.exe ... --press start@30,cross@34  presses controller buttons at those seconds (no window)
//   sly4.exe ... --watch 0xADDR       logs every change of the 128-byte guest line at ADDR (debugging)
//   sly4.exe ... --watch-line 0xADDR[,0xADDR...]  logs every atomic/DMA write to those lines, with who and what (debugging)
//   sly4.exe ... --dump-frames dir[,N]  writes every Nth displayed frame (default 60) as PNG
//   sly4.exe ... --data <dir>        where /dev_hdd0 and /dev_hdd1 live (default %LOCALAPPDATA%\sly4-port)
//   sly4.exe ... --dump-imports f.csv  writes module,nid,name,slot of every import
//   sly4.exe ... --dump-mem addr,size,f.bin  writes that guest memory to a file when the game stops
//   sly4.exe --nid-names <file>      NID name candidates (default tools\data\ps3_nid_candidates.txt)
//   sly4.exe --frames 300            presents 300 frames and exits (exit code 0 = ok)
//   sly4.exe --validation            enables VK_LAYER_KHRONOS_validation when installed
//   sly4.exe --no-vsync              MAILBOX/IMMEDIATE present mode when available
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cpu/guest_atomic.h"
#include "cpu/ppu_rt.h"
#include "gpu/rsx_vk.h"
#include "gpu/vulkan_context.h"
#include "kernel/hle.h"
#include "kernel/kernel.h"
#include "kernel/spu_thread.h"
#include "loader/elf_loader.h"
#include "loader/image_report.h"
#include "loader/nid.h"
#include "memory/guest_memory.h"
#include "rsx/rsx.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "core/host_sleep.h"
#include "core/log.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifdef SLY4_HAS_GEN
namespace sly4::gen {
void register_functions();           // generated/ppu/table.cpp
void register_firmware_functions();  // gen_firmware.cpp (one call per generated/fw_*)
void register_spu_code();             // gen_firmware.cpp (generated/spu, when present)
}
#endif

namespace {

constexpr const char* kDefaultNidNames = "tools/data/ps3_nid_candidates.txt";

struct Args {
    long frames = 0;  // 0 = until the window is closed
    long seconds = 0;  // 0 = no limit for the game
    long ring = 0;     // last calls printed with the summary
    std::string dump_mem;  // "addr,size,file" written at the end
    std::string rsx_capture;  // "frame,file"
    std::string watch;        // guest address of a 128-byte line whose changes are logged
    std::string ppu_profile;  // PPU thread name sampled by --ppu-profile
    std::string press;        // --press: scripted controller buttons
    std::string thread_timeline;  // --thread-timeline NAME
    std::string watch_line;   // guest addresses (comma-separated) of 128-byte lines whose every write is logged
    std::string game_dir, eboot, nid_names, dump_imports, data_dir, firmware_dir, spu_cache;
    bool window = true, run = true, trace = false, spu_interp = false, spu_profile = false, verify_zlib = false, call_times = false;
    bool render = false;      // render without a window (Vulkan headless)
    std::string dump_frames;  // "dir[,every]": PNG of every Nth flip
    sly4::VulkanOptions vulkan;
};

bool parse_args(int argc, char** argv, Args& out) {
    for (int i = 1; i < argc; ++i) {
        auto value = [&](std::string& dst) {
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };
        bool ok = true;
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            out.frames = std::strtol(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--game") == 0) {
            ok = value(out.game_dir);
        } else if (std::strcmp(argv[i], "--eboot") == 0) {
            ok = value(out.eboot);
        } else if (std::strcmp(argv[i], "--nid-names") == 0) {
            ok = value(out.nid_names);
        } else if (std::strcmp(argv[i], "--dump-imports") == 0) {
            ok = value(out.dump_imports);
        } else if (std::strcmp(argv[i], "--dump-mem") == 0) {
            ok = value(out.dump_mem);
        } else if (std::strcmp(argv[i], "--rsx-capture") == 0) {
            ok = value(out.rsx_capture);
        } else if (std::strcmp(argv[i], "--watch") == 0) {
            ok = value(out.watch);
        } else if (std::strcmp(argv[i], "--watch-line") == 0) {
            ok = value(out.watch_line);
        } else if (std::strcmp(argv[i], "--no-window") == 0) {
            out.window = false;
        } else if (std::strcmp(argv[i], "--no-run") == 0) {
            out.run = false;
        } else if (std::strcmp(argv[i], "--data") == 0) {
            ok = value(out.data_dir);
        } else if (std::strcmp(argv[i], "--firmware") == 0) {
            ok = value(out.firmware_dir);
        } else if (std::strcmp(argv[i], "--spu-cache") == 0) {
            ok = value(out.spu_cache);
        } else if (std::strcmp(argv[i], "--spu-interp") == 0) {
            out.spu_interp = true;
        } else if (std::strcmp(argv[i], "--press") == 0) {
            ok = value(out.press);
        } else if (std::strcmp(argv[i], "--ppu-profile") == 0) {
            ok = value(out.ppu_profile);
        } else if (std::strcmp(argv[i], "--rsx-timeline") == 0) {
            sly4::rsx::set_timeline(true);
            sly4::spu_timeline(true);
        } else if (std::strcmp(argv[i], "--thread-timeline") == 0) {
            ok = value(out.thread_timeline);
        } else if (std::strcmp(argv[i], "--call-times") == 0) {
            out.call_times = true;
        } else if (std::strcmp(argv[i], "--spu-profile") == 0) {
            out.spu_profile = true;
        } else if (std::strcmp(argv[i], "--verify-zlib") == 0) {
            out.verify_zlib = true;
        } else if (std::strcmp(argv[i], "--render") == 0) {
            out.render = true;
        } else if (std::strcmp(argv[i], "--dump-frames") == 0) {
            ok = value(out.dump_frames);
        } else if (std::strcmp(argv[i], "--trace") == 0) {
            out.trace = true;
        } else if (std::strcmp(argv[i], "--ring") == 0 && i + 1 < argc) {
            out.ring = std::strtol(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            out.seconds = std::strtol(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--validation") == 0) {
            out.vulkan.validation = true;
        } else if (std::strcmp(argv[i], "--no-vsync") == 0) {
            out.vulkan.vsync = false;
        } else {
            ok = false;
        }
        if (!ok) {
            SLY4_ERROR("opÃ§Ã£o desconhecida ou sem valor: %s", argv[i]);
            return false;
        }
    }
    if (out.eboot.empty() && !out.game_dir.empty())
        out.eboot = (std::filesystem::path(out.game_dir) / "USRDIR" / "EBOOT.elf").string();
    return true;
}

const char* present_mode_name(VkPresentModeKHR m) {
    switch (m) {
        case VK_PRESENT_MODE_FIFO_KHR: return "FIFO (vsync)";
        case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
        case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
        default: return "outro";
    }
}

// Phase 1: PS3 address space + executable. Returns false if loading failed.
bool load_executable(const Args& args, sly4::GuestMemory& mem, sly4::LoadedImage& img, sly4::NidNames& names) {
    if (!mem.reserve()) return false;
    sly4::install_guest_fault_handler(mem);

    const std::string nid_file = args.nid_names.empty() ? kDefaultNidNames : args.nid_names;
    if (names.load_file(nid_file))
        SLY4_INFO("%zu nomes de NID carregados de %s", names.size(), nid_file.c_str());
    else if (!args.nid_names.empty())
        SLY4_WARN("lista de NIDs nÃ£o encontrada: %s", nid_file.c_str());

    std::string error;
    const auto t0 = std::chrono::steady_clock::now();
    if (!sly4::load_elf(args.eboot, mem, img, error)) {
        SLY4_ERROR("falha ao carregar %s: %s", args.eboot.c_str(), error.c_str());
        return false;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    SLY4_INFO("%s carregado em %.1f ms; %.1f MiB confirmados na memÃ³ria do PS3", args.eboot.c_str(), ms,
              mem.committed_bytes() / 1048576.0);
    sly4::log_image_summary(img, names);
    if (!args.dump_imports.empty()) {
        if (sly4::write_imports_csv(img, names, args.dump_imports))
            SLY4_INFO("importaÃ§Ãµes gravadas em %s", args.dump_imports.c_str());
        else
            SLY4_ERROR("nÃ£o foi possÃ­vel gravar %s", args.dump_imports.c_str());
    }
    return true;
}

// ---------------------------------------------------------------- phase 3: the game
sly4::Kernel* g_kernel = nullptr;
std::unique_ptr<sly4::rsx::VulkanRenderer> g_renderer;
sly4::GuestMemory* g_mem = nullptr;
std::string g_dump_mem;
// The game is over once any guest thread calls sys_process_exit (or main returns).
bool game_over() { return g_kernel && g_kernel->exited(); }
int game_status() { return g_kernel ? g_kernel->exit_status() : 0; }

// --dump-mem addr,size,file (numbers in hex or decimal).
void dump_memory() {
    if (!g_mem || g_dump_mem.empty()) return;
    char* end = nullptr;
    const unsigned long addr = std::strtoul(g_dump_mem.c_str(), &end, 0);
    const unsigned long size = *end == ',' ? std::strtoul(end + 1, &end, 0) : 0;
    if (*end != ',' || !size) {
        SLY4_ERROR("--dump-mem: use endereÃ§o,tamanho,arquivo");
        return;
    }
    FILE* f = std::fopen(end + 1, "wb");
    if (!f) {
        SLY4_ERROR("--dump-mem: nÃ£o foi possÃ­vel gravar %s", end + 1);
        return;
    }
    std::fwrite(g_mem->host(uint32_t(addr)), 1, size, f);
    std::fclose(f);
    SLY4_INFO("memÃ³ria 0x%08lX (+0x%lX) gravada em %s", addr, size, end + 1);
}

// --watch: a host thread compares the line with its last copy every ~250 us.
void start_watch(sly4::GuestMemory& mem, const std::string& spec) {
    const uint32_t line = uint32_t(std::strtoul(spec.c_str(), nullptr, 0)) & ~127u;
    std::thread([&mem, line] {
        while (!game_over() && !mem.is_committed(line, 128)) sly4::host_sleep_us(1000);  // not allocated yet
        uint32_t last[32];
        std::memcpy(last, mem.host(line), 128);
        const auto t0 = std::chrono::steady_clock::now();
        while (!game_over()) {
            sly4::host_sleep_us(250);
            uint32_t now[32];
            std::memcpy(now, mem.host(line), 128);
            for (int i = 0; i < 32; ++i)
                if (now[i] != last[i]) {
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    SLY4_INFO("watch %9.3f ms: 0x%08X = %08X (era %08X)", ms, line + 4 * i, __builtin_bswap32(now[i]),
                              __builtin_bswap32(last[i]));
                }
            std::memcpy(last, now, 128);
        }
    }).detach();
}

// --press start@30,cross@34: presses controller buttons at those seconds of the run, each
// for 250 ms, to get through menus without a window (the window's input is phase 6).
bool start_presses(const std::string& spec) {
    struct Button {
        const char* name;
        uint16_t digital1, digital2;
        int pressure;  // index in PadState::pressure, -1 for none
    };
    static constexpr Button kButtons[] = {
        {"select", 0x01, 0, -1}, {"l3", 0x02, 0, -1},    {"r3", 0x04, 0, -1},       {"start", 0x08, 0, -1},
        {"up", 0x10, 0, 2},      {"right", 0x20, 0, 0},  {"down", 0x40, 0, 3},      {"left", 0x80, 0, 1},
        {"l2", 0, 0x01, 10},     {"r2", 0, 0x02, 11},    {"l1", 0, 0x04, 8},        {"r1", 0, 0x08, 9},
        {"triangle", 0, 0x10, 4}, {"circle", 0, 0x20, 5}, {"cross", 0, 0x40, 6},    {"square", 0, 0x80, 7},
    };
    struct Press {
        const Button* button;
        double at;
    };
    std::vector<Press> presses;
    for (size_t i = 0; i < spec.size();) {
        const size_t end = std::min(spec.find(',', i), spec.size());
        const std::string item = spec.substr(i, end - i);
        i = end + 1;
        const size_t at = item.find('@');
        const Button* b = nullptr;
        for (const Button& k : kButtons)
            if (at != std::string::npos && item.compare(0, at, k.name) == 0 && std::strlen(k.name) == at) b = &k;
        if (!b) {
            SLY4_ERROR("--press: botão inválido em '%s' (use nome@segundos)", item.c_str());
            return false;
        }
        presses.push_back({b, std::strtod(item.c_str() + at + 1, nullptr)});
    }
    std::thread([presses] {
        const auto t0 = std::chrono::steady_clock::now();
        auto wait_until = [&](double s) {
            while (!game_over() && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < s)
                sly4::host_sleep_us(5000);
        };
        for (const Press& p : presses) {
            wait_until(p.at);
            if (game_over()) return;
            sly4::PadState s;
            s.digital1 = p.button->digital1;
            s.digital2 = p.button->digital2;
            if (p.button->pressure >= 0) s.pressure[p.button->pressure] = 255;
            sly4::set_pad_state(s);
            SLY4_INFO("controle: %s (%.1f s)", p.button->name, p.at);
            wait_until(p.at + 0.25);
            sly4::set_pad_state(sly4::PadState{});
        }
    }).detach();
    return true;
}

[[noreturn]] void stop_with_summary(int code) {
    if (g_kernel) g_kernel->print_summary();
    if (g_renderer) g_renderer->summary();
    dump_memory();
    std::fflush(stderr);
    std::_Exit(code);
}

#ifdef SLY4_HAS_GEN
void on_guest_fault(sly4::PPUContext& c, sly4::GuestMemory&, const char* what) {
    SLY4_ERROR("PPU: %s em 0x%08X (LR 0x%08llX)", what, c.cia, static_cast<unsigned long long>(c.lr));
    stop_with_summary(4);
}

LONG WINAPI on_unhandled(EXCEPTION_POINTERS* e) {
    const sly4::PpuThread* t = sly4::current_thread();
    SLY4_ERROR("exceÃ§Ã£o 0x%08lX no host%s; LR do convidado 0x%08llX", e->ExceptionRecord->ExceptionCode,
               t ? (" na thread " + t->name).c_str() : "", t ? static_cast<unsigned long long>(t->ctx.lr) : 0ull);
    stop_with_summary(5);
}

DWORD WINAPI game_thread(void*) {
    g_kernel->run_main();
    return 0;
}
#endif

// Starts the game's main thread on a host thread with a large stack (recompiled guest
// calls nest as host calls). Returns false if the game cannot run in this build.
bool start_game(const Args& args, sly4::GuestMemory& mem, const sly4::LoadedImage& img, const sly4::NidNames& names) {
#ifdef SLY4_HAS_GEN
    sly4::gen::register_functions();
    sly4::gen::register_firmware_functions();
    if (!args.spu_interp) sly4::gen::register_spu_code();
    static sly4::Kernel kernel(mem, img);
    g_kernel = &kernel;
    g_mem = &mem;
    g_dump_mem = args.dump_mem;
    sly4::KernelOptions opt;
    opt.trace = args.trace;
    opt.firmware_dir = args.firmware_dir;
    opt.ring = size_t(args.ring);
    opt.spu_cache = args.spu_cache;
    opt.spu_profile = args.spu_profile;
    opt.call_times = args.call_times;
    opt.thread_timeline = args.thread_timeline;
    opt.ppu_profile = args.ppu_profile;
    opt.verify_zlib = args.verify_zlib;
    if (!args.rsx_capture.empty()) {
        const size_t comma = args.rsx_capture.find(',');
        sly4::rsx::set_capture(uint32_t(std::strtoul(args.rsx_capture.c_str(), nullptr, 10)),
                               comma == std::string::npos ? "rsx_frame.txt" : args.rsx_capture.substr(comma + 1));
    }
    // PS3_GAME is the folder above USRDIR\EBOOT.elf; saves and caches go to %LOCALAPPDATA%.
    const std::filesystem::path eboot = std::filesystem::absolute(args.eboot);
    opt.game_dir = args.game_dir.empty() ? eboot.parent_path().parent_path().string()
                                         : std::filesystem::absolute(args.game_dir).string();
    const char* local = std::getenv("LOCALAPPDATA");
    opt.data_dir = args.data_dir.empty() ? (std::filesystem::path(local ? local : ".") / "sly4-port").string()
                                         : std::filesystem::absolute(args.data_dir).string();
    if (!kernel.init(names, opt)) return false;
    sly4::rt::hooks().fault = on_guest_fault;
    SetUnhandledExceptionFilter(on_unhandled);
    HANDLE h = CreateThread(nullptr, 256u << 20, game_thread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) return false;
    CloseHandle(h);
    return true;
#else
    SLY4_WARN("este build nÃ£o tem o cÃ³digo do jogo: gere com \"sly4_recomp emit\" e compile de novo");
    return false;
#endif
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);  // log messages are UTF-8 (Portuguese accents)
#endif
    Args args;
    if (!parse_args(argc, argv, args)) return 2;
    SLY4_INFO("sly4 %s - port nativo, fase 3 (kernel e HLE)", "0.0.3");

    sly4::GuestMemory mem;
    sly4::LoadedImage image;
    sly4::NidNames names;
    if (!args.eboot.empty() && !load_executable(args, mem, image, names)) return 3;
    // The GPU comes first: the RSX renderer must exist before the game reaches cellGcmInit.
    SDL_Window* window = nullptr;
    static sly4::VulkanContext gpu;
    bool gpu_ok = false;
    if (args.window) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            SLY4_ERROR("SDL_Init: %s", SDL_GetError());
            return 1;
        }
        window = SDL_CreateWindow("Sly Cooper: Thieves in Time - port nativo", 1280, 720,
                                  SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!window) {
            SLY4_ERROR("SDL_CreateWindow: %s", SDL_GetError());
            SDL_Quit();
            return 1;
        }
        if (!gpu.init(window, args.vulkan)) {
            gpu.shutdown();
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
        gpu_ok = true;
    } else if (args.render || !args.dump_frames.empty()) {
        gpu_ok = gpu.init_headless(args.vulkan);
    }
    if (gpu_ok && args.run && !args.eboot.empty()) {
        sly4::rsx::VulkanRendererOptions ro;
        if (!args.dump_frames.empty()) {
            const size_t comma = args.dump_frames.find(',');
            ro.dump_dir = args.dump_frames.substr(0, comma);
            ro.dump_every =
                comma == std::string::npos ? 60 : uint32_t(std::strtoul(args.dump_frames.c_str() + comma + 1, nullptr, 10));
        }
        g_renderer = sly4::rsx::create_vulkan_renderer(gpu, ro);
        if (g_renderer) sly4::rsx::set_backend(g_renderer.get());
    }
    const bool game = !args.eboot.empty() && args.run && start_game(args, mem, image, names);
    if (game && !args.watch.empty()) start_watch(mem, args.watch);
    if (game && !args.press.empty() && !start_presses(args.press)) return 1;
    for (const char* p = args.watch_line.c_str(); *p;) {  // a comma-separated list
        char* end = nullptr;
        const uint32_t line = uint32_t(std::strtoul(p, &end, 0));
        if (end == p) break;
        sly4::add_debug_line(line);
        p = *end == ',' ? end + 1 : end;
    }
    const auto game_start = std::chrono::steady_clock::now();
    auto game_timed_out = [&] {
        return args.seconds > 0 && std::chrono::steady_clock::now() - game_start >= std::chrono::seconds(args.seconds);
    };
    if (!args.window) {
        if (!game) return 0;
        while (!game_over() && !game_timed_out()) Sleep(10);
        if (!game_over()) SLY4_INFO("tempo esgotado (%ld s): o jogo continua rodando; parando", args.seconds);
        stop_with_summary(game_over() ? game_status() : 0);
    }

    VkExtent2D ext = gpu.swapchain_extent();
    SLY4_INFO("swapchain %ux%u, formato %d, %s", ext.width, ext.height, static_cast<int>(gpu.swapchain_format()),
              present_mode_name(gpu.present_mode()));

    // Sly's blue, until the game shows something.
    const std::array<float, 4> clear = {0.02f, 0.09f, 0.35f, 1.0f};
    const auto start = std::chrono::steady_clock::now();
    long presented = 0;
    bool running = true, ok = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED: running = false; break;
                case SDL_EVENT_KEY_DOWN:
                    if (ev.key.key == SDLK_ESCAPE) running = false;
                    break;
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: gpu.notify_resized(); break;
                default: break;
            }
        }
        if (!running) break;
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(16);
            continue;
        }
        if (!gpu.draw_frame(clear, g_renderer ? g_renderer->front() : sly4::VulkanContext::Source{})) {
            ok = false;
            break;
        }
        ++presented;
        if (args.frames > 0 && presented >= args.frames) break;
        if (game && (game_over() || game_timed_out())) break;
    }

    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    SLY4_INFO("%ld quadros em %.2f s (%.1f fps) em %s", presented, secs, secs > 0 ? presented / secs : 0.0,
              gpu.device_name().c_str());

    // Guest threads (and the RSX thread drawing with the device) cannot be interrupted:
    // leave without waiting for them.
    if (game) stop_with_summary(ok ? game_status() : 1);
    gpu.shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return ok ? 0 : 1;
}
