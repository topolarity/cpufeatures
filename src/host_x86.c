// Host CPU detection for x86_64.
// Standalone - no LLVM dependency.
// Uses CPUID to detect CPU name and features.

#include "target_tables_x86_64.h"
#include "target_parsing.h"

#include <string.h>
#include <cpuid.h>

// ============================================================================
// CPUID helpers
// ============================================================================

typedef struct {
    unsigned eax, ebx, ecx, edx;
} CPUIDResult;

static CPUIDResult do_cpuid(unsigned leaf, unsigned subleaf) {
    CPUIDResult r = {0, 0, 0, 0};
    __cpuid_count(leaf, subleaf, r.eax, r.ebx, r.ecx, r.edx);
    return r;
}

static unsigned cpuid_max_leaf(void) {
    return do_cpuid(0, 0).eax;
}

static unsigned cpuid_max_ext_leaf(void) {
    return do_cpuid(0x80000000, 0).eax;
}

// ============================================================================
// CPU vendor/family/model detection
// ============================================================================

enum Vendor { VENDOR_INTEL, VENDOR_AMD, VENDOR_OTHER };

static enum Vendor get_vendor(void) {
    CPUIDResult r = do_cpuid(0, 0);
    if (r.ebx == 0x756e6547 && r.edx == 0x49656e69 && r.ecx == 0x6c65746e)
        return VENDOR_INTEL;
    if (r.ebx == 0x68747541 && r.edx == 0x69746e65 && r.ecx == 0x444d4163)
        return VENDOR_AMD;
    return VENDOR_OTHER;
}

typedef struct {
    unsigned family;
    unsigned model;
    unsigned stepping;
} CPUModel;

static CPUModel get_cpu_model(void) {
    CPUIDResult r = do_cpuid(1, 0);
    CPUModel m;
    m.stepping = r.eax & 0xf;
    m.model = (r.eax >> 4) & 0xf;
    m.family = (r.eax >> 8) & 0xf;

    if (m.family == 6 || m.family == 0xf) {
        m.model += ((r.eax >> 16) & 0xf) << 4;
    }
    if (m.family == 0xf) {
        m.family += (r.eax >> 20) & 0xff;
    }
    return m;
}

// ============================================================================
// Host CPU name detection
// ============================================================================

static const char *detect_intel_cpu(const CPUModel *m) {
    if (m->family != 6) return "generic";

    switch (m->model) {
    case 0x0f:
    case 0x16: return "core2";
    case 0x17:
    case 0x1d: return "core2";
    case 0x1a:
    case 0x1e:
    case 0x1f:
    case 0x2e: return "nehalem";
    case 0x25:
    case 0x2c:
    case 0x2f: return "westmere";
    case 0x2a:
    case 0x2d: return "sandybridge";
    case 0x3a:
    case 0x3e: return "ivybridge";
    case 0x3c:
    case 0x3f:
    case 0x45:
    case 0x46: return "haswell";
    case 0x3d:
    case 0x47:
    case 0x4f:
    case 0x56: return "broadwell";
    case 0x4e:
    case 0x5e:
    case 0x8e:
    case 0x9e: return "skylake";
    case 0x55: {
        if (m->stepping >= 5) return "cascadelake";
        return "skylake-avx512";
    }
    case 0x66: return "cannonlake";
    case 0x6a:
    case 0x6c: return "icelake-server";
    case 0x7d:
    case 0x7e: return "icelake-client";
    case 0x8c:
    case 0x8d: return "tigerlake";
    case 0x8f: return "sapphirerapids";
    case 0x97:
    case 0x9a: return "alderlake";
    case 0xb7:
    case 0xba:
    case 0xbf: return "raptorlake";
    case 0xaa:
    case 0xac: return "meteorlake";
    case 0xbd: return "lunarlake";
    case 0xc5: return "arrowlake";
    case 0xc6: return "arrowlake-s";
    case 0xcf: return "emeraldrapids";
    case 0xad:
    case 0xae: return "graniterapids";
    default: return "generic";
    }
}

static const char *detect_amd_cpu(const CPUModel *m) {
    switch (m->family) {
    case 0x10: return "amdfam10";
    case 0x14: return "btver1";
    case 0x15:
        if (m->model >= 0x60) return "bdver4";
        if (m->model >= 0x30) return "bdver3";
        if (m->model >= 0x02) return "bdver2";
        return "bdver1";
    case 0x16: return "btver2";
    case 0x17:
        if (m->model >= 0x30) return "znver2";
        return "znver1";
    case 0x19:
        if (m->model >= 0x10) return "znver4";
        return "znver3";
    case 0x1a: return "znver5";
    default: return "generic";
    }
}

const char *tp_get_host_cpu_name(void) {
    static const char *cpu_name = NULL;
    if (cpu_name) return cpu_name;

    enum Vendor v = get_vendor();
    CPUModel m = get_cpu_model();

    const char *name;
    if (v == VENDOR_INTEL)
        name = detect_intel_cpu(&m);
    else if (v == VENDOR_AMD)
        name = detect_amd_cpu(&m);
    else
        name = "generic";

    if (!find_cpu(name))
        name = "generic";

    cpu_name = name;
    return cpu_name;
}

// ============================================================================
// Table-driven host feature detection via CPUID
// ============================================================================

typedef struct {
    unsigned leaf;
    unsigned subleaf;
    enum { REG_EAX, REG_EBX, REG_ECX, REG_EDX } reg;
    unsigned bit;
    const char *feature_name;
} CPUIDBitMapping;

static const CPUIDBitMapping cpuid_features[] = {
    // Leaf 1, ECX
    {1, 0, REG_ECX,  0, "sse3"},
    {1, 0, REG_ECX,  1, "pclmul"},
    {1, 0, REG_ECX,  9, "ssse3"},
    {1, 0, REG_ECX, 12, "fma"},
    {1, 0, REG_ECX, 13, "cx16"},
    {1, 0, REG_ECX, 19, "sse4.1"},
    {1, 0, REG_ECX, 20, "sse4.2"},
    {1, 0, REG_ECX, 20, "crc32"},
    {1, 0, REG_ECX, 22, "movbe"},
    {1, 0, REG_ECX, 23, "popcnt"},
    {1, 0, REG_ECX, 25, "aes"},
    {1, 0, REG_ECX, 26, "xsave"},
    {1, 0, REG_ECX, 28, "avx"},
    {1, 0, REG_ECX, 29, "f16c"},
    {1, 0, REG_ECX, 30, "rdrnd"},

    // Leaf 7 sub 0, EBX
    {7, 0, REG_EBX,  0, "fsgsbase"},
    {7, 0, REG_EBX,  3, "bmi"},
    {7, 0, REG_EBX,  5, "avx2"},
    {7, 0, REG_EBX,  8, "bmi2"},
    {7, 0, REG_EBX, 10, "invpcid"},
    {7, 0, REG_EBX, 16, "avx512f"},
    {7, 0, REG_EBX, 17, "avx512dq"},
    {7, 0, REG_EBX, 18, "rdseed"},
    {7, 0, REG_EBX, 19, "adx"},
    {7, 0, REG_EBX, 21, "avx512ifma"},
    {7, 0, REG_EBX, 23, "clflushopt"},
    {7, 0, REG_EBX, 24, "clwb"},
    {7, 0, REG_EBX, 28, "avx512cd"},
    {7, 0, REG_EBX, 29, "sha"},
    {7, 0, REG_EBX, 30, "avx512bw"},
    {7, 0, REG_EBX, 31, "avx512vl"},

    // Leaf 7 sub 0, ECX
    {7, 0, REG_ECX,  1, "avx512vbmi"},
    {7, 0, REG_ECX,  4, "pku"},
    {7, 0, REG_ECX,  5, "waitpkg"},
    {7, 0, REG_ECX,  6, "avx512vbmi2"},
    {7, 0, REG_ECX,  7, "shstk"},
    {7, 0, REG_ECX,  8, "gfni"},
    {7, 0, REG_ECX,  9, "vaes"},
    {7, 0, REG_ECX, 10, "vpclmulqdq"},
    {7, 0, REG_ECX, 11, "avx512vnni"},
    {7, 0, REG_ECX, 12, "avx512bitalg"},
    {7, 0, REG_ECX, 14, "avx512vpopcntdq"},
    {7, 0, REG_ECX, 22, "rdpid"},
    {7, 0, REG_ECX, 25, "cldemote"},
    {7, 0, REG_ECX, 27, "movdiri"},
    {7, 0, REG_ECX, 28, "movdir64b"},
    {7, 0, REG_ECX, 29, "enqcmd"},

    // Leaf 7 sub 0, EDX
    {7, 0, REG_EDX,  5, "uintr"},
    {7, 0, REG_EDX,  8, "avx512vp2intersect"},
    {7, 0, REG_EDX, 14, "serialize"},
    {7, 0, REG_EDX, 16, "tsxldtrk"},
    {7, 0, REG_EDX, 18, "pconfig"},
    {7, 0, REG_EDX, 22, "amx-bf16"},
    {7, 0, REG_EDX, 23, "avx512fp16"},
    {7, 0, REG_EDX, 24, "amx-tile"},
    {7, 0, REG_EDX, 25, "amx-int8"},

    // Leaf 7 sub 1, EAX
    {7, 1, REG_EAX,  0, "sha512"},
    {7, 1, REG_EAX,  1, "sm3"},
    {7, 1, REG_EAX,  2, "sm4"},
    {7, 1, REG_EAX,  4, "avxvnni"},
    {7, 1, REG_EAX,  5, "avx512bf16"},
    {7, 1, REG_EAX,  7, "cmpccxadd"},
    {7, 1, REG_EAX, 21, "amx-fp16"},
    {7, 1, REG_EAX, 23, "avxifma"},

    // Leaf 7 sub 1, EBX
    {7, 1, REG_EBX,  4, "avxvnniint8"},
    {7, 1, REG_EBX,  5, "avxneconvert"},
    {7, 1, REG_EBX,  8, "amx-complex"},
    {7, 1, REG_EBX, 10, "avxvnniint16"},
    {7, 1, REG_EBX, 14, "prefetchi"},

    // Extended 0x80000001, ECX
    {0x80000001, 0, REG_ECX,  0, "sahf"},
    {0x80000001, 0, REG_ECX,  5, "lzcnt"},
    {0x80000001, 0, REG_ECX,  6, "sse4a"},
    {0x80000001, 0, REG_ECX,  8, "prfchw"},
    {0x80000001, 0, REG_ECX, 11, "xop"},
    {0x80000001, 0, REG_ECX, 16, "fma4"},
    {0x80000001, 0, REG_ECX, 21, "tbm"},
    {0x80000001, 0, REG_ECX, 29, "mwaitx"},

    // Leaf 0xD sub 1, EAX (XSAVE)
    {0xd, 1, REG_EAX, 0, "xsaveopt"},
    {0xd, 1, REG_EAX, 1, "xsavec"},
    {0xd, 1, REG_EAX, 3, "xsaves"},

    // Extended 0x80000008, EBX
    {0x80000008, 0, REG_EBX, 0, "clzero"},
    {0x80000008, 0, REG_EBX, 4, "rdpru"},
    {0x80000008, 0, REG_EBX, 9, "wbnoinvd"},
};

static void disable_feature(FeatureBits *features, const char *name) {
    const FeatureEntry *fe = find_feature(name);
    if (fe) feature_clear(features, fe->bit);
}

FeatureBits tp_get_host_features(void) {
    FeatureBits features;
    memset(&features, 0, sizeof(features));

    unsigned max_leaf = cpuid_max_leaf();
    unsigned max_ext = cpuid_max_ext_leaf();

    // Always-present on x86_64
    static const char *baseline_features[] = {
        "64bit", "cx8", "cmov", "fxsr", "mmx", "sse", "sse2", "x87"
    };
    unsigned bi;
    for (bi = 0; bi < sizeof(baseline_features)/sizeof(baseline_features[0]); bi++) {
        const FeatureEntry *f = find_feature(baseline_features[bi]);
        if (f) feature_set(&features, f->bit);
    }

    // Walk the table, cache CPUID results to avoid redundant calls
    struct {
        unsigned leaf, subleaf;
        CPUIDResult result;
        int valid;
    } cache[8];
    unsigned cache_count = 0;
    memset(cache, 0, sizeof(cache));

    unsigned ei;
    for (ei = 0; ei < sizeof(cpuid_features)/sizeof(cpuid_features[0]); ei++) {
        const CPUIDBitMapping *entry = &cpuid_features[ei];
        int is_ext = (entry->leaf >= 0x80000000);
        unsigned max = is_ext ? max_ext : max_leaf;
        if (entry->leaf > max) continue;

        CPUIDResult r = {0, 0, 0, 0};
        int found = 0;
        unsigned c;
        for (c = 0; c < cache_count; c++) {
            if (cache[c].leaf == entry->leaf && cache[c].subleaf == entry->subleaf) {
                r = cache[c].result;
                found = 1;
                break;
            }
        }
        if (!found) {
            r = do_cpuid(entry->leaf, entry->subleaf);
            if (cache_count < 8) {
                cache[cache_count].leaf = entry->leaf;
                cache[cache_count].subleaf = entry->subleaf;
                cache[cache_count].result = r;
                cache[cache_count].valid = 1;
                cache_count++;
            }
        }

        unsigned reg_val = 0;
        switch (entry->reg) {
        case REG_EAX: reg_val = r.eax; break;
        case REG_EBX: reg_val = r.ebx; break;
        case REG_ECX: reg_val = r.ecx; break;
        case REG_EDX: reg_val = r.edx; break;
        }

        if (reg_val & (1u << entry->bit)) {
            const FeatureEntry *f = find_feature(entry->feature_name);
            if (f) feature_set(&features, f->bit);
        }
    }

    // XCR0 validation: the OS must enable state save for AVX/AVX-512/AMX.
    // CPUID reports hardware capability, but XCR0 indicates OS support.
    CPUIDResult r1 = do_cpuid(1, 0);
    int has_xsave = (r1.ecx >> 27) & 1;
    int has_avx_save = 0;
    int has_avx512_save = 0;
    int has_amx_save = 0;

    if (has_xsave) {
        // Read XCR0 via XGETBV(0)
        unsigned xcr0_lo, xcr0_hi;
        __asm__ volatile(".byte 0x0f, 0x01, 0xd0"
                         : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));

        has_avx_save = (xcr0_lo & 0x6) == 0x6;  // bits 1,2: SSE + AVX state
#if defined(__APPLE__)
        // Darwin lazily saves AVX-512 context on first use
        has_avx512_save = has_avx_save;
#else
        has_avx512_save = has_avx_save && (xcr0_lo & 0xe0) == 0xe0;  // bits 5,6,7
#endif
        has_amx_save = has_xsave && (xcr0_lo & ((1 << 17) | (1 << 18))) == ((1 << 17) | (1 << 18));
    }

    // Disable features that require OS state save support
    if (!has_avx_save) {
        static const char *avx_features[] = {
            "avx", "avx2", "fma", "f16c", "fma4", "xop",
            "vaes", "vpclmulqdq", "xsave", "xsaveopt", "xsavec", "xsaves",
            NULL
        };
        const char **f;
        for (f = avx_features; *f; f++) disable_feature(&features, *f);
        has_avx512_save = 0;
    }

    if (!has_avx512_save) {
        static const char *avx512_features[] = {
            "avx512f", "avx512dq", "avx512ifma", "avx512cd",
            "avx512bw", "avx512vl", "avx512vbmi", "avx512vpopcntdq",
            "avx512vbmi2", "avx512vnni", "avx512bitalg",
            "avx512vp2intersect", "avx512bf16", "avx512fp16",
            "evex512", NULL
        };
        const char **f;
        for (f = avx512_features; *f; f++) disable_feature(&features, *f);
    }

    if (!has_amx_save) {
        static const char *amx_features[] = {
            "amx-tile", "amx-int8", "amx-bf16", "amx-fp16",
            "amx-complex", "amx-fp8", "amx-transpose", "amx-avx512",
            "amx-tf32", "amx-movrs", NULL
        };
        const char **f;
        for (f = amx_features; *f; f++) disable_feature(&features, *f);
    }

    // AVX-512 implies evex512 (only if not already disabled above)
    const FeatureEntry *avx512f = find_feature("avx512f");
    if (avx512f && feature_test(&features, avx512f->bit)) {
        const FeatureEntry *evex512 = find_feature("evex512");
        if (evex512) feature_set(&features, evex512->bit);
    }

    expand_implied(&features);
    return features;
}
