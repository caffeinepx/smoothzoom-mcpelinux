#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "Zoom", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "Zoom", __VA_ARGS__)

static constexpr int kKeyC = 67;
static constexpr int kActionPress = 0;
static constexpr int kActionRepeat = 1;
static constexpr int kActionRelease = 2;
static constexpr float kMinFov = 0.04f;
static constexpr float kMaxFov = 0.95f;

static unsigned long pack_fov(float value) {
    unsigned long packed = 0;
    std::memcpy(&packed, &value, sizeof(value));
    packed |= (1ull << 32);
    return packed;
}

static bool unpack_fov(unsigned long packed, float* out) {
    if (((packed >> 32) & 0xffu) == 0) {
        return false;
    }
    std::memcpy(out, &packed, sizeof(float));
    return std::isfinite(*out) && *out > 0.001f && *out < 8.f;
}

static std::string config_path() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&config_path), &info) && info.dli_fname) {
        std::string path = info.dli_fname;
        auto slash = path.find_last_of('/');
        if (slash != std::string::npos) {
            return path.substr(0, slash + 1) + "zoom.conf";
        }
    }
    return "/data/data/com.mojang.minecraftpe/zoom.conf";
}

static std::string key_name(int key) {
    if ((key >= 65 && key <= 90) || (key >= 48 && key <= 57)) {
        return std::string(1, static_cast<char>(key));
    }
    return "KEY_" + std::to_string(key);
}

struct MenuEntryABI {
    const char* name;
    void* user;
    bool (*selected)(void* user);
    void (*click)(void* user);
    size_t length;
    MenuEntryABI* subentries;
};

struct Control {
    int type;
    union {
        struct {
            const char* label;
            void* user;
            void (*onClick)(void* user);
        } button;
        struct {
            const char* label;
            int min;
            int def;
            int max;
            void* user;
            void (*onChange)(void* user, int value);
        } sliderint;
        struct {
            const char* label;
            float min;
            float def;
            float max;
            void* user;
            void (*onChange)(void* user, float value);
        } sliderfloat;
        struct {
            char* label;
            int size;
        } text;
    } data;
};

struct MemRange {
    std::byte* data;
    size_t size;
};

static bool (*game_window_is_mouse_locked)(void* window);
static void (*game_window_add_window_creation_callback)(void* user, void (*callback)(void* user));
static void* (*game_window_get_primary_window)();
static void (*game_window_add_keyboard_callback)(void* window, void* user,
                                                 bool (*callback)(void* user, int keyCode, int action));
static void (*game_window_add_mouse_scroll_callback)(
    void* window, void* user, bool (*callback)(void* user, double x, double y, double dx, double dy));
static void (*game_window_add_swap_buffers_callback)(void* user,
                                                     void (*callback)(void* user, EGLDisplay display, EGLSurface surface));

static void (*mcpelauncher_show_window)(const char* title, int isModal, void* user, void (*onClose)(void* user),
                                        int count, Control* controls);
static void (*mcpelauncher_close_window)(const char* title);
static void (*mcpelauncher_addmenu)(size_t length, MenuEntryABI* entries);

static unsigned long (*CameraAPI_tryGetFOV_orig)(void*);

static int g_zoom_key = kKeyC;
static float g_default_fov = 0.30f;
static float g_bar_height = 0.16f;
static float g_smooth_speed = 10.0f;
static bool g_bars_on = true;

static bool g_holding = false;
static bool g_rebinding = false;
static bool g_have_rest = false;
static float g_session_fov = 0.30f;
static float g_current_fov = 0.70f;
static float g_rest_fov = 0.70f;
static float g_zoom_t = 0.0f;
static float g_wheel = 0.0f;
static char g_key_label[128];

static std::vector<MemRange> g_readable;
static std::vector<MemRange> g_writable;

static double now_sec() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

static float exp_approach(float current, float target, float speed, float dt) {
    float a = 1.0f - std::exp(-speed * dt);
    return current + (target - current) * a;
}

static bool playing() {
    void* window = game_window_get_primary_window ? game_window_get_primary_window() : nullptr;
    return window && game_window_is_mouse_locked && game_window_is_mouse_locked(window);
}

static void save_config() {
    FILE* out = fopen(config_path().c_str(), "w");
    if (!out) {
        return;
    }
    std::fprintf(out, "zoomKey=%d\nzoomFov=%.4f\nbarHeight=%.4f\nsmoothSpeed=%.4f\nbars=%d\n", g_zoom_key,
                 g_default_fov, g_bar_height, g_smooth_speed, g_bars_on ? 1 : 0);
    fclose(out);
}

static void load_config() {
    FILE* in = fopen(config_path().c_str(), "r");
    if (!in) {
        return;
    }
    char line[128];
    while (fgets(line, sizeof(line), in)) {
        if (!std::strncmp(line, "zoomKey=", 8)) {
            g_zoom_key = std::atoi(line + 8);
        } else if (!std::strncmp(line, "zoomFov=", 8)) {
            g_default_fov = std::strtof(line + 8, nullptr);
        } else if (!std::strncmp(line, "barHeight=", 10)) {
            g_bar_height = std::strtof(line + 10, nullptr);
        } else if (!std::strncmp(line, "smoothSpeed=", 12)) {
            g_smooth_speed = std::strtof(line + 12, nullptr);
        } else if (!std::strncmp(line, "bars=", 5)) {
            g_bars_on = std::atoi(line + 5) != 0;
        }
    }
    fclose(in);
    g_default_fov = std::clamp(g_default_fov, kMinFov, kMaxFov);
    g_bar_height = std::clamp(g_bar_height, 0.0f, 0.35f);
    g_smooth_speed = std::clamp(g_smooth_speed, 2.0f, 30.0f);
    if (g_zoom_key <= 0) {
        g_zoom_key = kKeyC;
    }
}

static void tick_zoom(float dt) {
    if (g_holding && g_wheel != 0.0f) {
        float step = std::clamp(g_wheel * dt * 14.0f, -0.35f, 0.35f);
        g_session_fov *= std::exp(-step * 0.55f);
        g_session_fov = std::clamp(g_session_fov, kMinFov, kMaxFov);
        g_wheel = exp_approach(g_wheel, 0.0f, 18.0f, dt);
        if (std::fabs(g_wheel) < 0.02f) {
            g_wheel = 0.0f;
        }
    } else {
        g_wheel = exp_approach(g_wheel, 0.0f, 18.0f, dt);
    }

    float target_t = (g_holding && playing()) ? 1.0f : 0.0f;
    float target_fov = g_holding ? g_session_fov : g_rest_fov;
    g_zoom_t = exp_approach(g_zoom_t, target_t, g_smooth_speed, dt);
    g_current_fov = exp_approach(g_current_fov, target_fov, g_smooth_speed, dt);
    if (!g_holding && g_zoom_t < 0.01f) {
        g_zoom_t = 0.0f;
        g_current_fov = g_rest_fov;
    }
}

static void tick_if_needed() {
    static double last_tick = 0.0;
    double t = now_sec();
    float dt = last_tick > 0.0 ? static_cast<float>(t - last_tick) : (1.0f / 60.0f);
    if (dt < 0.002f) {
        return;
    }
    last_tick = t;
    tick_zoom(std::clamp(dt, 0.002f, 0.05f));
}

static unsigned long CameraAPI_tryGetFOV_hook(void* self) {
    unsigned long original = CameraAPI_tryGetFOV_orig(self);
    tick_if_needed();

    float orig_fov = 0.f;
    bool has_orig = unpack_fov(original, &orig_fov);
    if (!g_holding && g_zoom_t < 0.01f) {
        if (has_orig) {
            g_rest_fov = orig_fov;
            g_current_fov = orig_fov;
            g_have_rest = true;
        }
        return original;
    }
    if (!g_have_rest) {
        return original;
    }
    float fov = std::clamp(g_current_fov, kMinFov, 4.0f);
    return pack_fov(fov);
}

static bool on_scroll(void* /*user*/, double /*x*/, double /*y*/, double /*dx*/, double dy) {
    if (!g_holding || !playing()) {
        return false;
    }
    g_wheel += static_cast<float>(dy);
    g_wheel = std::clamp(g_wheel, -8.0f, 8.0f);
    return true;
}

static bool on_key(void* /*user*/, int keyCode, int action) {
    if (g_rebinding) {
        if (action == kActionPress && keyCode != 0) {
            g_zoom_key = keyCode;
            g_rebinding = false;
            save_config();
            std::snprintf(g_key_label, sizeof(g_key_label), "Bound to %s", key_name(g_zoom_key).c_str());
            if (mcpelauncher_close_window) {
                mcpelauncher_close_window("Zoom key");
            }
        }
        return true;
    }
    if (keyCode != g_zoom_key || action == kActionRepeat) {
        return false;
    }
    if (action == kActionPress) {
        if (!playing()) {
            return false;
        }
        g_holding = true;
        g_session_fov = g_default_fov;
        g_wheel = 0.0f;
        if (g_have_rest) {
            g_current_fov = g_rest_fov;
        }
        return true;
    }
    if (action == kActionRelease) {
        g_holding = false;
        g_wheel = 0.0f;
        return true;
    }
    return false;
}

static void draw_letterbox(EGLDisplay dpy, EGLSurface surface) {
    if (!g_bars_on || g_zoom_t <= 0.001f) {
        return;
    }
    if (!dpy || !surface || surface == EGL_NO_SURFACE) {
        return;
    }
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surface, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surface, EGL_HEIGHT, &h);
    if (w <= 1 || h <= 1) {
        return;
    }
    int bar = static_cast<int>(static_cast<float>(h) * g_bar_height * g_zoom_t);
    if (bar < 1) {
        return;
    }

    GLint prev_vp[4] = {};
    GLint old_scissor[4] = {};
    GLfloat clear[4] = {};
    GLboolean mask[4] = {};
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_VIEWPORT, prev_vp);
    glGetIntegerv(GL_SCISSOR_BOX, old_scissor);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);

    glViewport(0, 0, w, h);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_SCISSOR_TEST);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glScissor(0, 0, w, bar);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(0, h - bar, w, bar);
    glClear(GL_COLOR_BUFFER_BIT);

    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    glScissor(old_scissor[0], old_scissor[1], old_scissor[2], old_scissor[3]);
    if (!scissor) {
        glDisable(GL_SCISSOR_TEST);
    }
}

static void on_swap(void* /*user*/, EGLDisplay dpy, EGLSurface surface) {
    tick_if_needed();
    draw_letterbox(dpy, surface);
}

static void show_key_window() {
    if (!mcpelauncher_show_window) {
        return;
    }
    g_rebinding = true;
    std::snprintf(g_key_label, sizeof(g_key_label), "Press a new key (current: %s)", key_name(g_zoom_key).c_str());
    static Control controls[1];
    controls[0].type = 3;
    controls[0].data.text.label = g_key_label;
    controls[0].data.text.size = 0;
    mcpelauncher_show_window(
        "Zoom key", 1, nullptr,
        [](void*) {
            g_rebinding = false;
            save_config();
        },
        1, controls);
}

static void show_settings_window() {
    if (!mcpelauncher_show_window) {
        return;
    }
    static Control c[3];
    c[0].type = 2;
    c[0].data.sliderfloat = {"Default zoom", 0.04f, g_default_fov, 0.80f, nullptr, [](void*, float v) {
                                 g_default_fov = v;
                                 save_config();
                             }};
    c[1].type = 2;
    c[1].data.sliderfloat = {"Bar height", 0.0f, g_bar_height, 0.30f, nullptr, [](void*, float v) {
                                 g_bar_height = v;
                                 save_config();
                             }};
    c[2].type = 2;
    c[2].data.sliderfloat = {"Ease speed", 2.0f, g_smooth_speed, 24.0f, nullptr, [](void*, float v) {
                                 g_smooth_speed = v;
                                 save_config();
                             }};
    mcpelauncher_show_window("Zoom settings", 0, nullptr, [](void*) { save_config(); }, 3, c);
}

static void init_menu() {
    void* libmenu = dlopen("libmcpelauncher_menu.so", RTLD_NOW);
    if (!libmenu) {
        return;
    }
    mcpelauncher_show_window =
        reinterpret_cast<decltype(mcpelauncher_show_window)>(dlsym(libmenu, "mcpelauncher_show_window"));
    mcpelauncher_close_window =
        reinterpret_cast<decltype(mcpelauncher_close_window)>(dlsym(libmenu, "mcpelauncher_close_window"));
    mcpelauncher_addmenu = reinterpret_cast<decltype(mcpelauncher_addmenu)>(dlsym(libmenu, "mcpelauncher_addmenu"));
    if (!mcpelauncher_addmenu) {
        return;
    }

    static MenuEntryABI items[4]{};
    items[0].name = "Change keybind";
    items[0].click = [](void*) { show_key_window(); };
    items[0].selected = [](void*) { return false; };
    items[1].name = "Settings";
    items[1].click = [](void*) { show_settings_window(); };
    items[1].selected = [](void*) { return false; };
    items[2].name = "Black bars";
    items[2].click = [](void*) {
        g_bars_on = !g_bars_on;
        save_config();
    };
    items[2].selected = [](void*) { return g_bars_on; };
    items[3].name = "Reset defaults";
    items[3].click = [](void*) {
        g_default_fov = 0.30f;
        g_bar_height = 0.16f;
        g_smooth_speed = 10.0f;
        save_config();
    };
    items[3].selected = [](void*) { return false; };

    static MenuEntryABI zoom_menu{};
    zoom_menu.name = "Zoom";
    zoom_menu.length = 4;
    zoom_menu.subentries = items;
    mcpelauncher_addmenu(1, &zoom_menu);
}

static const std::byte* find_bytes(MemRange haystack, const void* needle, size_t needle_size) {
    if (!haystack.data || haystack.size < needle_size) {
        return nullptr;
    }
    return static_cast<const std::byte*>(memmem(haystack.data, haystack.size, needle, needle_size));
}

static const std::byte* find_pointer(MemRange haystack, const void* value) {
    return find_bytes(haystack, &value, sizeof(value));
}

static bool collect_mc_ranges(void* mc_lib) {
    g_readable.clear();
    g_writable.clear();
    auto visit = [&](const dl_phdr_info& info) -> int {
        void* handle = dlopen(info.dlpi_name, RTLD_NOLOAD);
        if (handle) {
            dlclose(handle);
        }
        if (handle != mc_lib) {
            return 0;
        }
        for (int i = 0; i < info.dlpi_phnum; ++i) {
            const auto& ph = info.dlpi_phdr[i];
            if (ph.p_type != PT_LOAD || ph.p_memsz == 0) {
                continue;
            }
            MemRange range{reinterpret_cast<std::byte*>(info.dlpi_addr + ph.p_vaddr), ph.p_memsz};
            g_readable.push_back(range);
            if (ph.p_flags & PF_W) {
                g_writable.push_back(range);
            }
        }
        return 1;
    };
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* data) { return (*static_cast<decltype(visit)*>(data))(*info); }, &visit);
    return !g_readable.empty();
}

static void** find_primary_vtable(const char* typeinfo_name) {
    const std::byte* name_addr = nullptr;
    size_t nlen = std::strlen(typeinfo_name) + 1;
    for (auto range : g_readable) {
        name_addr = find_bytes(range, typeinfo_name, nlen);
        if (name_addr) {
            break;
        }
    }
    if (!name_addr) {
        return nullptr;
    }
    const std::byte* typeinfo = nullptr;
    for (auto range : g_readable) {
        const std::byte* hit = find_pointer(range, name_addr);
        if (hit) {
            typeinfo = hit - sizeof(void*);
            break;
        }
    }
    if (!typeinfo) {
        return nullptr;
    }

    const std::byte* best = nullptr;
    auto consider = [&](MemRange range) {
        const std::byte* start = range.data;
        const std::byte* end = range.data + range.size;
        const auto* needle = reinterpret_cast<const std::byte*>(&typeinfo);
        for (const std::byte* p = start; p + sizeof(void*) <= end; p += sizeof(void*)) {
            if (std::memcmp(p, needle, sizeof(void*)) != 0 || p == typeinfo) {
                continue;
            }
            intptr_t offset_to_top = 0;
            if (p - sizeof(void*) >= start) {
                std::memcpy(&offset_to_top, p - sizeof(void*), sizeof(offset_to_top));
            }
            if (offset_to_top != 0) {
                continue;
            }
            best = p;
            return true;
        }
        return false;
    };
    for (auto range : g_writable) {
        if (consider(range)) {
            break;
        }
    }
    if (!best) {
        for (auto range : g_readable) {
            if (consider(range)) {
                break;
            }
        }
    }
    if (!best) {
        return nullptr;
    }
    return reinterpret_cast<void**>(const_cast<std::byte*>(best) + sizeof(void*));
}

static bool hook_camera_api() {
    void** vt = find_primary_vtable("9CameraAPI");
    if (!vt) {
        LOGE("Could not find CameraAPI primary vtable.");
        return false;
    }
    void** fn_slot = vt + 7;
    long page_size = sysconf(_SC_PAGESIZE);
    auto page = reinterpret_cast<uintptr_t>(fn_slot) & ~(static_cast<uintptr_t>(page_size) - 1);
    if (mprotect(reinterpret_cast<void*>(page), page_size, PROT_READ | PROT_WRITE) != 0) {
        LOGE("mprotect on CameraAPI vtable failed.");
        return false;
    }
    CameraAPI_tryGetFOV_orig = reinterpret_cast<decltype(CameraAPI_tryGetFOV_orig)>(*fn_slot);
    if (!CameraAPI_tryGetFOV_orig) {
        LOGE("CameraAPI::tryGetFOV slot was null.");
        return false;
    }
    *fn_slot = reinterpret_cast<void*>(&CameraAPI_tryGetFOV_hook);
    LOGI("Hooked CameraAPI::tryGetFOV orig=%p", reinterpret_cast<void*>(CameraAPI_tryGetFOV_orig));
    return true;
}

static void on_window_created(void* /*user*/) {
    void* window = game_window_get_primary_window();
    if (!window) {
        return;
    }
    game_window_add_keyboard_callback(window, nullptr, on_key);
    game_window_add_mouse_scroll_callback(window, nullptr, on_scroll);
}

extern "C" __attribute__((visibility("default"))) void mod_preinit() {}

extern "C" __attribute__((visibility("default"))) void mod_init() {
    LOGI("Loading zoom.");
    load_config();
    g_session_fov = g_default_fov;

    void* gw = dlopen("libmcpelauncher_gamewindow.so", RTLD_NOW);
    if (!gw) {
        LOGE("dlopen libmcpelauncher_gamewindow.so failed: %s", dlerror());
        return;
    }
    game_window_is_mouse_locked =
        reinterpret_cast<decltype(game_window_is_mouse_locked)>(dlsym(gw, "game_window_is_mouse_locked"));
    game_window_get_primary_window =
        reinterpret_cast<decltype(game_window_get_primary_window)>(dlsym(gw, "game_window_get_primary_window"));
    game_window_add_window_creation_callback = reinterpret_cast<decltype(game_window_add_window_creation_callback)>(
        dlsym(gw, "game_window_add_window_creation_callback"));
    game_window_add_keyboard_callback =
        reinterpret_cast<decltype(game_window_add_keyboard_callback)>(dlsym(gw, "game_window_add_keyboard_callback"));
    game_window_add_mouse_scroll_callback = reinterpret_cast<decltype(game_window_add_mouse_scroll_callback)>(
        dlsym(gw, "game_window_add_mouse_scroll_callback"));
    game_window_add_swap_buffers_callback = reinterpret_cast<decltype(game_window_add_swap_buffers_callback)>(
        dlsym(gw, "game_window_add_swap_buffers_callback"));

    void* mc = dlopen("libminecraftpe.so", RTLD_NOLOAD);
    if (!mc) {
        mc = dlopen("libminecraftpe.so", RTLD_NOW);
    }
    if (!mc || !collect_mc_ranges(mc) || !hook_camera_api()) {
        return;
    }

    init_menu();
    game_window_add_window_creation_callback(nullptr, on_window_created);
    if (game_window_add_swap_buffers_callback) {
        game_window_add_swap_buffers_callback(nullptr, on_swap);
        LOGI("Letterbox via swap-buffers callback.");
    } else {
        LOGI("No swap-buffers callback; bars disabled.");
        g_bars_on = false;
    }
    LOGI("Zoom ready.");
}
