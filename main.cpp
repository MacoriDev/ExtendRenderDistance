#include <pthread.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <new>
#include <string>
#include <vector>

#include "pl/Gloss.h"
#include "pl/Hook.h"

// =========================
// User config
// =========================
static constexpr uint32_t MAX_RENDER_DISTANCE_CHUNKS = 96;

// =========================
// Don't Use
// =========================
static constexpr uint32_t MIN_RENDER_DISTANCE_CHUNKS = 5;

static constexpr uintptr_t OREUI_GFX_VIEWDISTANCE_LR_RVA = 0x0b1a0d7c;

struct MapRegion {
    uintptr_t start;
    uintptr_t end;
    bool readable;
    bool writable;
    bool executable;
    std::string name;
};

struct LibInfo {
    uintptr_t base = 0;
    std::vector<MapRegion> executable_ranges;
};

struct MemRange {
    uintptr_t start;
    uintptr_t end;
    bool readable;
    bool writable;
};

struct VecInfo {
    uintptr_t obj = 0;
    uintptr_t begin = 0;
    uintptr_t end = 0;
    uintptr_t cap = 0;
    uint32_t max_value = 0;
    size_t count = 0;
};

static MemRange g_mem_ranges[4096];
static int g_mem_range_count = 0;

extern "C" {
__attribute__((visibility("default"))) void* g_old_gfx_target = nullptr;
}

static GHook g_target_hook = nullptr;

static uintptr_t g_lib_base = 0;
static uintptr_t g_call_target = 0;

static uintptr_t untag(uintptr_t p) {
    return p & 0x00FFFFFFFFFFFFFFull;
}

static uintptr_t preserve_pointer_style(uintptr_t old_ptr, uintptr_t new_ptr) {
    if ((old_ptr & 0xFF00000000000000ull) == 0) {
        return untag(new_ptr);
    }

    return new_ptr;
}

static uint32_t read_u32_unsafe(uintptr_t addr) {
    uint32_t v = 0;
    memcpy(&v, reinterpret_cast<const void*>(addr), sizeof(v));
    return v;
}

static std::vector<MapRegion> read_maps() {
    std::vector<MapRegion> out;

    std::ifstream f("/proc/self/maps");
    std::string line;

    while (std::getline(f, line)) {
        uintptr_t start = 0;
        uintptr_t end = 0;
        char perms[5] = {};

        if (sscanf(line.c_str(), "%lx-%lx %4s", &start, &end, perms) != 3) {
            continue;
        }

        std::string name;

        size_t slash = line.find('/');
        size_t bracket = line.find('[');

        if (slash != std::string::npos) {
            name = line.substr(slash);
        } else if (bracket != std::string::npos) {
            name = line.substr(bracket);
        }

        out.push_back({
            start,
            end,
            perms[0] == 'r',
            perms[1] == 'w',
            perms[2] == 'x',
            name
        });
    }

    return out;
}

static void refresh_mem_ranges() {
    g_mem_range_count = 0;

    std::ifstream f("/proc/self/maps");
    std::string line;

    while (std::getline(f, line)) {
        if (g_mem_range_count >= 4096) {
            break;
        }

        uintptr_t start = 0;
        uintptr_t end = 0;
        char perms[5] = {};

        if (sscanf(line.c_str(), "%lx-%lx %4s", &start, &end, perms) != 3) {
            continue;
        }

        bool readable = perms[0] == 'r';
        bool writable = perms[1] == 'w';

        if (!readable) {
            continue;
        }

        g_mem_ranges[g_mem_range_count++] = {
            start,
            end,
            readable,
            writable
        };
    }
}

static bool is_readable_cached(uintptr_t ptr, size_t size) {
    if (ptr == 0 || size == 0) {
        return false;
    }

    uintptr_t p = untag(ptr);
    uintptr_t e = p + size;

    if (e < p) {
        return false;
    }

    for (int i = 0; i < g_mem_range_count; i++) {
        const auto& r = g_mem_ranges[i];

        if (!r.readable) {
            continue;
        }

        if (p >= r.start && e <= r.end) {
            return true;
        }
    }

    return false;
}

static bool is_writable_cached(uintptr_t ptr, size_t size) {
    if (ptr == 0 || size == 0) {
        return false;
    }

    uintptr_t p = untag(ptr);
    uintptr_t e = p + size;

    if (e < p) {
        return false;
    }

    for (int i = 0; i < g_mem_range_count; i++) {
        const auto& r = g_mem_ranges[i];

        if (!r.readable || !r.writable) {
            continue;
        }

        if (p >= r.start && e <= r.end) {
            return true;
        }
    }

    return false;
}

static bool safe_read_u32(uintptr_t addr, uint32_t* out) {
    if (!out) {
        return false;
    }

    uintptr_t p = untag(addr);

    if (!is_readable_cached(p, sizeof(uint32_t))) {
        return false;
    }

    memcpy(out, reinterpret_cast<const void*>(p), sizeof(uint32_t));
    return true;
}

static bool safe_read_u64(uintptr_t addr, uint64_t* out) {
    if (!out) {
        return false;
    }

    uintptr_t p = untag(addr);

    if (!is_readable_cached(p, sizeof(uint64_t))) {
        return false;
    }

    memcpy(out, reinterpret_cast<const void*>(p), sizeof(uint64_t));
    return true;
}

static bool safe_write_u64(uintptr_t addr, uint64_t value) {
    uintptr_t p = untag(addr);

    if (!is_writable_cached(p, sizeof(uint64_t))) {
        return false;
    }

    memcpy(reinterpret_cast<void*>(p), &value, sizeof(uint64_t));
    return true;
}

static bool find_libminecraftpe(LibInfo& out) {
    auto maps = read_maps();

    uintptr_t min_addr = UINTPTR_MAX;

    for (const auto& r : maps) {
        if (r.name.find("libminecraftpe.so") == std::string::npos) {
            continue;
        }

        if (r.start < min_addr) {
            min_addr = r.start;
        }

        if (r.readable && r.executable) {
            out.executable_ranges.push_back(r);
        }
    }

    if (min_addr == UINTPTR_MAX || out.executable_ranges.empty()) {
        return false;
    }

    out.base = min_addr;
    return true;
}

static int32_t sign_extend_26(uint32_t imm26) {
    if (imm26 & (1u << 25)) {
        return static_cast<int32_t>(imm26 | 0xFC000000u);
    }

    return static_cast<int32_t>(imm26);
}

static bool is_bl(uint32_t word) {
    return (word & 0xFC000000u) == 0x94000000u;
}

static uintptr_t decode_bl_target(uintptr_t pc, uint32_t word) {
    uint32_t imm26 = word & 0x03FFFFFFu;
    int32_t signed_imm = sign_extend_26(imm26);
    intptr_t offset = static_cast<intptr_t>(signed_imm) << 2;

    return static_cast<uintptr_t>(static_cast<intptr_t>(pc) + offset);
}

static bool scan_oreui_viewdistance_callsite(const LibInfo& lib) {
    // OreUI gfx_viewdistance callsite pattern.
    static constexpr uint32_t P0 = 0xAA1A03E0u; // mov x0, x26
    static constexpr uint32_t P1 = 0x52800501u; // mov w1, #0x28
    static constexpr uint32_t P2 = 0x52800022u; // mov w2, #0x1
    static constexpr uint32_t P3 = 0x52800203u; // mov w3, #0x10
    static constexpr uint32_t P4 = 0x2A1403E6u; // mov w6, w20
    static constexpr uint32_t P5 = 0xF90007F5u;
    static constexpr uint32_t P6 = 0x390003FFu;

    for (const auto& r : lib.executable_ranges) {
        if (r.end <= r.start || r.end - r.start < 0x40) {
            continue;
        }

        uintptr_t start = (r.start + 3u) & ~(uintptr_t)3u;
        uintptr_t end = r.end - 0x40;

        for (uintptr_t p = start; p <= end; p += 4) {
            if (read_u32_unsafe(p + 0x00) != P0) continue;
            if (read_u32_unsafe(p + 0x04) != P1) continue;
            if (read_u32_unsafe(p + 0x08) != P2) continue;
            if (read_u32_unsafe(p + 0x0c) != P3) continue;
            if (read_u32_unsafe(p + 0x10) != P4) continue;
            if (read_u32_unsafe(p + 0x14) != P5) continue;
            if (read_u32_unsafe(p + 0x18) != P6) continue;

            uint32_t call = read_u32_unsafe(p + 0x1c);

            if (!is_bl(call)) {
                continue;
            }

            uintptr_t callsite = p + 0x1c;
            g_call_target = decode_bl_target(callsite, call);
            return true;
        }
    }

    return false;
}

static bool read_vector_from_x7(uintptr_t x7, VecInfo* out) {
    if (!out) {
        return false;
    }

    uintptr_t obj = untag(x7);

    if (!is_readable_cached(obj, 0x18) || !is_writable_cached(obj, 0x18)) {
        return false;
    }

    uint64_t raw_begin = 0;
    uint64_t raw_end = 0;
    uint64_t raw_cap = 0;

    if (!safe_read_u64(obj + 0x00, &raw_begin)) return false;
    if (!safe_read_u64(obj + 0x08, &raw_end)) return false;
    if (!safe_read_u64(obj + 0x10, &raw_cap)) return false;

    uintptr_t begin = static_cast<uintptr_t>(raw_begin);
    uintptr_t end = static_cast<uintptr_t>(raw_end);
    uintptr_t cap = static_cast<uintptr_t>(raw_cap);

    uintptr_t ub = untag(begin);
    uintptr_t ue = untag(end);
    uintptr_t uc = untag(cap);

    if (ub == 0 || ue <= ub || uc < ue) {
        return false;
    }

    if (((ue - ub) % sizeof(uint32_t)) != 0) {
        return false;
    }

    size_t count = (ue - ub) / sizeof(uint32_t);

    if (count < 8 || count > 128) {
        return false;
    }

    if (!is_readable_cached(begin, count * sizeof(uint32_t))) {
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        uint32_t v = 0;

        if (!safe_read_u32(begin + i * sizeof(uint32_t), &v)) {
            return false;
        }

        uint32_t expected = MIN_RENDER_DISTANCE_CHUNKS + static_cast<uint32_t>(i);

        if (v != expected) {
            return false;
        }
    }

    out->obj = obj;
    out->begin = begin;
    out->end = end;
    out->cap = cap;
    out->max_value = MIN_RENDER_DISTANCE_CHUNKS + static_cast<uint32_t>(count) - 1;
    out->count = count;

    return true;
}

static uintptr_t create_chunk_list(uintptr_t old_begin, uint32_t target_max, size_t* out_count) {
    if (target_max < MIN_RENDER_DISTANCE_CHUNKS) {
        return 0;
    }

    size_t count = static_cast<size_t>(target_max - MIN_RENDER_DISTANCE_CHUNKS + 1);
    size_t bytes = count * sizeof(uint32_t);

    auto* values = static_cast<uint32_t*>(::operator new(bytes, std::nothrow));

    if (!values) {
        return 0;
    }

    for (size_t i = 0; i < count; i++) {
        values[i] = MIN_RENDER_DISTANCE_CHUNKS + static_cast<uint32_t>(i);
    }

    if (out_count) {
        *out_count = count;
    }

    uintptr_t raw = reinterpret_cast<uintptr_t>(values);
    return preserve_pointer_style(old_begin, raw);
}

static bool patch_x7_vector(uintptr_t x7) {
    VecInfo vec{};

    if (!read_vector_from_x7(x7, &vec)) {
        return false;
    }

    size_t target_count = MAX_RENDER_DISTANCE_CHUNKS - MIN_RENDER_DISTANCE_CHUNKS + 1;

    if (vec.max_value >= MAX_RENDER_DISTANCE_CHUNKS && vec.count >= target_count) {
        return false;
    }

    size_t new_count = 0;
    uintptr_t new_begin = create_chunk_list(vec.begin, MAX_RENDER_DISTANCE_CHUNKS, &new_count);

    if (!new_begin || new_count == 0) {
        return false;
    }

    uintptr_t new_end = new_begin + new_count * sizeof(uint32_t);
    uintptr_t new_cap = new_end;

    if (!safe_write_u64(vec.obj + 0x00, static_cast<uint64_t>(new_begin))) {
        return false;
    }

    if (!safe_write_u64(vec.obj + 0x08, static_cast<uint64_t>(new_end))) {
        return false;
    }

    if (!safe_write_u64(vec.obj + 0x10, static_cast<uint64_t>(new_cap))) {
        return false;
    }

    return true;
}

extern "C" void hook_patch_context(void* saved_sp, void* original_lr) {
    auto* regs = reinterpret_cast<uintptr_t*>(saved_sp);

    uintptr_t lr = reinterpret_cast<uintptr_t>(original_lr);
    uintptr_t lr_rva = g_lib_base ? (lr - g_lib_base) : 0;

    uintptr_t x1 = regs[1];
    uintptr_t x2 = regs[2];
    uintptr_t x3 = regs[3];
    uintptr_t x7 = regs[7];

    bool is_oreui_viewdistance =
        lr_rva == OREUI_GFX_VIEWDISTANCE_LR_RVA &&
        x1 == 0x28 &&
        x2 == 0x1 &&
        x3 == 0x10;

    if (!is_oreui_viewdistance) {
        return;
    }

    refresh_mem_ranges();
    patch_x7_vector(x7);
}

extern "C" __attribute__((naked)) void hook_gfx_target_entry() {
    __asm__ volatile(
        "sub sp, sp, #0x180\n"

        "stp x0,  x1,  [sp, #0x000]\n"
        "stp x2,  x3,  [sp, #0x010]\n"
        "stp x4,  x5,  [sp, #0x020]\n"
        "stp x6,  x7,  [sp, #0x030]\n"
        "stp x8,  x9,  [sp, #0x040]\n"
        "stp x10, x11, [sp, #0x050]\n"
        "stp x12, x13, [sp, #0x060]\n"
        "stp x14, x15, [sp, #0x070]\n"
        "stp x16, x17, [sp, #0x080]\n"
        "stp x18, x19, [sp, #0x090]\n"
        "stp x20, x21, [sp, #0x0a0]\n"
        "stp x22, x23, [sp, #0x0b0]\n"
        "stp x24, x25, [sp, #0x0c0]\n"
        "stp x26, x27, [sp, #0x0d0]\n"
        "stp x28, x29, [sp, #0x0e0]\n"
        "str x30, [sp, #0x0f0]\n"

        "stp q0, q1, [sp, #0x100]\n"
        "stp q2, q3, [sp, #0x120]\n"
        "stp q4, q5, [sp, #0x140]\n"
        "stp q6, q7, [sp, #0x160]\n"

        "mov x0, sp\n"
        "ldr x1, [sp, #0x0f0]\n"
        "bl hook_patch_context\n"

        "ldp q0, q1, [sp, #0x100]\n"
        "ldp q2, q3, [sp, #0x120]\n"
        "ldp q4, q5, [sp, #0x140]\n"
        "ldp q6, q7, [sp, #0x160]\n"

        "ldp x0,  x1,  [sp, #0x000]\n"
        "ldp x2,  x3,  [sp, #0x010]\n"
        "ldp x4,  x5,  [sp, #0x020]\n"
        "ldp x6,  x7,  [sp, #0x030]\n"
        "ldp x8,  x9,  [sp, #0x040]\n"
        "ldp x10, x11, [sp, #0x050]\n"
        "ldp x12, x13, [sp, #0x060]\n"
        "ldp x14, x15, [sp, #0x070]\n"
        "ldp x18, x19, [sp, #0x090]\n"
        "ldp x20, x21, [sp, #0x0a0]\n"
        "ldp x22, x23, [sp, #0x0b0]\n"
        "ldp x24, x25, [sp, #0x0c0]\n"
        "ldp x26, x27, [sp, #0x0d0]\n"
        "ldp x28, x29, [sp, #0x0e0]\n"
        "ldr x30, [sp, #0x0f0]\n"

        "adrp x16, :got:g_old_gfx_target\n"
        "ldr x16, [x16, #:got_lo12:g_old_gfx_target]\n"
        "ldr x16, [x16]\n"

        "add sp, sp, #0x180\n"
        "br x16\n"
    );
}

static bool install_hook() {
    if (!g_call_target) {
        return false;
    }

    GlossInit(true);

    g_target_hook = GlossHookAddr(
        reinterpret_cast<void*>(g_call_target),
        reinterpret_cast<void*>(hook_gfx_target_entry),
        reinterpret_cast<void**>(&g_old_gfx_target),
        false,
        I_ARM64
    );

    return g_target_hook != nullptr;
}

static void* worker_thread(void*) {
    LibInfo lib;

    for (int i = 0; i < 60; i++) {
        lib = LibInfo{};

        if (find_libminecraftpe(lib)) {
            g_lib_base = lib.base;
            refresh_mem_ranges();

            if (scan_oreui_viewdistance_callsite(lib)) {
                install_hook();
            }

            return nullptr;
        }

        usleep(500 * 1000);
    }

    return nullptr;
}

__attribute__((constructor))
static void init_extend_render_distance() {
    GlossInit(true);

    pthread_t t;
    pthread_create(&t, nullptr, worker_thread, nullptr);
    pthread_detach(t);
}
