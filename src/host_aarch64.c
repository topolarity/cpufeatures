// Host CPU detection for AArch64.
// Standalone - no LLVM dependency.
// Supports Linux (/proc/cpuinfo), macOS (sysctlbyname), Windows (stubs).

#include "target_tables_aarch64.h"
#include "target_parsing.h"

#include <string.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <stdio.h>
#endif

// ============================================================================
// macOS: CPU detection via sysctlbyname
// ============================================================================

#if defined(__APPLE__)

#define CPUFAMILY_ARM_FIRESTORM_ICESTORM 0x1b588bb3 // M1/A14
#define CPUFAMILY_ARM_BLIZZARD_AVALANCHE 0xda33d83d // M2/A15
#define CPUFAMILY_ARM_EVEREST_SAWTOOTH   0x8765edea // A16
#define CPUFAMILY_ARM_IBIZA              0xfa33415e // M3
#define CPUFAMILY_ARM_LOBOS              0x5f4dea93 // M3 Pro
#define CPUFAMILY_ARM_PALMA              0x72015832 // M3 Max
#define CPUFAMILY_ARM_COLL               0x2876f5b5 // A17 Pro
#define CPUFAMILY_ARM_DONAN              0x6f5129ac // M4
#define CPUFAMILY_ARM_BRAVA              0x17d5b93a // M4 Pro/Max
#define CPUFAMILY_ARM_TAHITI             0x75d4acb9 // A18 Pro
#define CPUFAMILY_ARM_TUPAI              0x204526d0 // A18
#define CPUFAMILY_ARM_HIDRA              0x1d5a87e8 // M5
#define CPUFAMILY_ARM_SOTRA              0xf76c5b1a // M5 Pro/Max
#define CPUFAMILY_ARM_THERA              0xab345f09 // A19 Pro
#define CPUFAMILY_ARM_TILOS              0x01d7a72b // A19

const char *tp_get_host_cpu_name(void) {
    static const char *cpu_name = NULL;
    if (cpu_name) return cpu_name;

    uint32_t family = 0;
    size_t len = sizeof(family);
    sysctlbyname("hw.cpufamily", &family, &len, NULL, 0);

    const char *name;
    switch (family) {
    case CPUFAMILY_ARM_FIRESTORM_ICESTORM: name = "apple-m1"; break;
    case CPUFAMILY_ARM_BLIZZARD_AVALANCHE: name = "apple-m2"; break;
    case CPUFAMILY_ARM_EVEREST_SAWTOOTH: name = "apple-a16"; break;
    case CPUFAMILY_ARM_IBIZA:
    case CPUFAMILY_ARM_PALMA:
    case CPUFAMILY_ARM_LOBOS:
        name = "apple-m3"; break;
    case CPUFAMILY_ARM_COLL:
        name = "apple-a17"; break;
    case CPUFAMILY_ARM_DONAN:
    case CPUFAMILY_ARM_BRAVA:
        name = "apple-m4"; break;
    case CPUFAMILY_ARM_TAHITI:
    case CPUFAMILY_ARM_TUPAI:
        name = "apple-a18"; break;
    default:
        name = "apple-m4"; break;
    }

    // Resolve alias and verify the CPU exists in the table.
    // If not, try progressively older CPUs as fallback.
    name = resolve_cpu_alias(name);
    if (!_find_cpu_exact(name)) {
        static const struct { const char *from; const char *fallback; } fallbacks[] = {
            {"apple-m5", "apple-m4"},
            {"apple-m4", "apple-a17"},
            {"apple-a17", "apple-a16"},
            {NULL, NULL}
        };
        const struct { const char *from; const char *fallback; } *f;
        for (f = fallbacks; f->from; f++) {
            if (strcmp(name, f->from) == 0) {
                const char *resolved = resolve_cpu_alias(f->fallback);
                if (_find_cpu_exact(resolved)) { name = resolved; break; }
            }
        }
        if (!_find_cpu_exact(name))
            name = "generic";
    }

    cpu_name = name;
    return cpu_name;
}

FeatureBits tp_get_host_features(void) {
    FeatureBits features;
    memset(&features, 0, sizeof(features));

    const char *cpu = tp_get_host_cpu_name();
    const CPUEntry *entry = _find_cpu_exact(cpu);
    if (entry)
        features = entry->features;

    return features;
}

// ============================================================================
// Windows AArch64: CPU detection
// ============================================================================

#elif defined(_WIN32)

const char *tp_get_host_cpu_name(void) {
    return "generic";
}

FeatureBits tp_get_host_features(void) {
    FeatureBits features;
    memset(&features, 0, sizeof(features));

    const FeatureEntry *fe;

    if ((fe = find_feature("neon"))) feature_set(&features, fe->bit);
    if ((fe = find_feature("fp-armv8"))) feature_set(&features, fe->bit);

    #ifndef PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE
    #define PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE 31
    #endif
    if (IsProcessorFeaturePresent(PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE)) {
        if ((fe = find_feature("crc"))) feature_set(&features, fe->bit);
    }

    #ifndef PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE
    #define PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE 30
    #endif
    if (IsProcessorFeaturePresent(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE)) {
        if ((fe = find_feature("aes"))) feature_set(&features, fe->bit);
        if ((fe = find_feature("sha2"))) feature_set(&features, fe->bit);
    }

    #ifndef PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE
    #define PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE 34
    #endif
    if (IsProcessorFeaturePresent(PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE)) {
        if ((fe = find_feature("lse"))) feature_set(&features, fe->bit);
    }

    #ifndef PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE
    #define PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE 43
    #endif
    if (IsProcessorFeaturePresent(PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE)) {
        if ((fe = find_feature("dotprod"))) feature_set(&features, fe->bit);
    }

    expand_implied(&features);
    return features;
}

// ============================================================================
// Linux AArch64: CPU detection via /proc/cpuinfo
// ============================================================================

#else // Linux

typedef struct {
    unsigned implementer;
    unsigned part;
    const char *name;
} ArmCPUInfo;

static const ArmCPUInfo arm_cpus[] = {
    // ARM Ltd. (0x41)
    {0x41, 0xd03, "cortex-a53"},
    {0x41, 0xd04, "cortex-a35"},
    {0x41, 0xd05, "cortex-a55"},
    {0x41, 0xd06, "cortex-a65"},
    {0x41, 0xd07, "cortex-a57"},
    {0x41, 0xd08, "cortex-a72"},
    {0x41, 0xd09, "cortex-a73"},
    {0x41, 0xd0a, "cortex-a75"},
    {0x41, 0xd0b, "cortex-a76"},
    {0x41, 0xd0c, "neoverse-n1"},
    {0x41, 0xd0d, "cortex-a77"},
    {0x41, 0xd40, "neoverse-v1"},
    {0x41, 0xd41, "cortex-a78"},
    {0x41, 0xd44, "cortex-x1"},
    {0x41, 0xd46, "cortex-a510"},
    {0x41, 0xd47, "cortex-a710"},
    {0x41, 0xd48, "cortex-x2"},
    {0x41, 0xd49, "neoverse-n2"},
    {0x41, 0xd4d, "cortex-a715"},
    {0x41, 0xd4e, "cortex-x3"},
    {0x41, 0xd4f, "neoverse-v2"},
    {0x41, 0xd80, "cortex-a520"},
    {0x41, 0xd81, "cortex-a720"},
    {0x41, 0xd82, "cortex-x4"},
    {0x41, 0xd84, "neoverse-v3"},
    {0x41, 0xd85, "cortex-x925"},
    {0x41, 0xd87, "cortex-a725"},
    // Broadcom / Cavium (0x42/0x43)
    {0x42, 0x516, "thunderx2t99"},
    {0x42, 0x0af, "thunderx2t99"},
    {0x42, 0x0a1, "thunderxt88"},
    {0x43, 0x516, "thunderx2t99"},
    {0x43, 0x0af, "thunderx2t99"},
    {0x43, 0x0a1, "thunderxt88"},
    // Fujitsu (0x46)
    {0x46, 0x001, "a64fx"},
    {0x46, 0x003, "fujitsu-monaka"},
    // HiSilicon (0x48)
    {0x48, 0xd01, "tsv110"},
    // NVIDIA (0x4e)
    {0x4e, 0x004, "carmel"},
    // Qualcomm (0x51)
    {0x51, 0x001, "oryon-1"},
    {0x51, 0x800, "cortex-a73"},
    {0x51, 0x801, "cortex-a73"},
    {0x51, 0x802, "cortex-a75"},
    {0x51, 0x803, "cortex-a75"},
    {0x51, 0x804, "cortex-a76"},
    {0x51, 0x805, "cortex-a76"},
    {0x51, 0xc00, "falkor"},
    {0x51, 0xc01, "saphira"},
    // Apple (0x61, on Linux/Asahi)
    {0x61, 0x020, "apple-m1"},
    {0x61, 0x021, "apple-m1"},
    {0x61, 0x022, "apple-m1"},
    {0x61, 0x023, "apple-m1"},
    {0x61, 0x024, "apple-m1"},
    {0x61, 0x025, "apple-m1"},
    {0x61, 0x028, "apple-m1"},
    {0x61, 0x029, "apple-m1"},
    {0x61, 0x030, "apple-m2"},
    {0x61, 0x031, "apple-m2"},
    {0x61, 0x032, "apple-m2"},
    {0x61, 0x033, "apple-m2"},
    {0x61, 0x034, "apple-m2"},
    {0x61, 0x035, "apple-m2"},
    {0x61, 0x038, "apple-m2"},
    {0x61, 0x039, "apple-m2"},
    {0x61, 0x048, "apple-m3"},
    {0x61, 0x049, "apple-m3"},
    // Microsoft (0x6d)
    {0x6d, 0xd49, "neoverse-n2"},
    // Ampere (0xc0)
    {0xc0, 0xac3, "ampere1"},
    {0xc0, 0xac4, "ampere1a"},
    {0xc0, 0xac5, "ampere1b"},
    {0, 0, NULL}
};

/* Load /proc/cpuinfo into a static buffer. Returns pointer to content. */
static const char *load_cpuinfo(size_t *out_len) {
    static char *content = NULL;
    static size_t content_len = 0;
    static int loaded = 0;
    if (!loaded) {
        loaded = 1;
        FILE *f = fopen("/proc/cpuinfo", "r");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0) {
                content = (char *)malloc((size_t)sz + 1);
                content_len = fread(content, 1, (size_t)sz, f);
                content[content_len] = '\0';
            }
            fclose(f);
        }
    }
    if (out_len) *out_len = content_len;
    return content ? content : "";
}

/* Find the first occurrence of a cpuinfo field, return its value as a view */
static tp_sv cpuinfo_field(tp_sv buf, const char *field) {
    tp_sv empty = {NULL, 0};
    size_t field_len = strlen(field);
    size_t pos = 0;
    while (pos < buf.len) {
        const char *found = (const char *)memmem(buf.data + pos, buf.len - pos, field, field_len);
        if (!found) break;
        size_t fpos = (size_t)(found - buf.data);

        if (fpos > 0 && buf.data[fpos - 1] != '\n') {
            pos = fpos + 1;
            continue;
        }

        size_t after = fpos + field_len;
        while (after < buf.len && (buf.data[after] == ' ' || buf.data[after] == '\t'))
            after++;
        if (after >= buf.len || buf.data[after] != ':') {
            pos = fpos + 1;
            continue;
        }
        after++;
        while (after < buf.len && (buf.data[after] == ' ' || buf.data[after] == '\t'))
            after++;

        const char *eol = (const char *)memchr(buf.data + after, '\n', buf.len - after);
        size_t end = eol ? (size_t)(eol - buf.data) : buf.len;
        tp_sv result;
        result.data = buf.data + after;
        result.len = end - after;
        return result;
    }
    return empty;
}

/* Collect all distinct values of a cpuinfo field. Returns tp_vec of tp_sv. */
static tp_vec cpuinfo_field_all(tp_sv buf, const char *field) {
    tp_vec result = tp_vec_new(sizeof(tp_sv));
    size_t field_len = strlen(field);
    size_t pos = 0;
    while (pos < buf.len) {
        const char *found = (const char *)memmem(buf.data + pos, buf.len - pos, field, field_len);
        if (!found) break;
        size_t fpos = (size_t)(found - buf.data);

        if (fpos > 0 && buf.data[fpos - 1] != '\n') {
            pos = fpos + 1;
            continue;
        }

        size_t after = fpos + field_len;
        while (after < buf.len && (buf.data[after] == ' ' || buf.data[after] == '\t'))
            after++;
        if (after >= buf.len || buf.data[after] != ':') {
            pos = fpos + 1;
            continue;
        }
        after++;
        while (after < buf.len && (buf.data[after] == ' ' || buf.data[after] == '\t'))
            after++;

        const char *eol = (const char *)memchr(buf.data + after, '\n', buf.len - after);
        size_t end = eol ? (size_t)(eol - buf.data) : buf.len;
        tp_sv val;
        val.data = buf.data + after;
        val.len = end - after;

        // Add if not already present
        int dup = 0;
        size_t ri;
        for (ri = 0; ri < result.count; ri++) {
            tp_sv *existing = (tp_sv *)tp_vec_at(&result, ri);
            if (existing->len == val.len && memcmp(existing->data, val.data, val.len) == 0) {
                dup = 1; break;
            }
        }
        if (!dup) tp_vec_push(&result, &val);

        pos = end + 1;
    }
    return result;
}

// Known big.LITTLE pairs: {big_part, little_part, result_name}
typedef struct {
    unsigned big_part;
    unsigned little_part;
    const char *name;
} BigLittlePair;

static const BigLittlePair big_little_pairs[] = {
    {0xd85, 0xd87, "cortex-x925"},
    {0xd82, 0xd80, "cortex-x4"},
    {0xd81, 0xd80, "cortex-a720"},
    {0xd4e, 0xd46, "cortex-x3"},
    {0xd4d, 0xd46, "cortex-a715"},
    {0xd48, 0xd46, "cortex-x2"},
    {0xd47, 0xd46, "cortex-a710"},
    {0xd44, 0xd41, "cortex-x1"},
    {0xd41, 0xd05, "cortex-a78"},
    {0xd0b, 0xd05, "cortex-a76"},
    {0xd0a, 0xd05, "cortex-a75"},
    {0xd08, 0xd03, "cortex-a72"},
    {0xd07, 0xd03, "cortex-a57"},
    {0, 0, NULL}
};

typedef struct {
    const char *linux_name;
    const char *llvm_name;
} FeatureMap;

static const FeatureMap aarch64_feature_map[] = {
    {"asimd", "neon"},
    {"fp", "fp-armv8"},
    {"crc32", "crc"},
    {"atomics", "lse"},
    {"rng", "rand"},
    {"sha3", "sha3"},
    {"sm4", "sm4"},
    {"sve", "sve"},
    {"sve2", "sve2"},
    {"sveaes", "sve-aes"},
    {"svesha3", "sve-sha3"},
    {"svesm4", "sve-sm4"},
    {"dotprod", "dotprod"},
    {"bf16", "bf16"},
    {"i8mm", "i8mm"},
    {"fphp", "fullfp16"},
    {"ssbs", "ssbs"},
    {"sb", "sb"},
    {"dcpop", "rcpc"},
    {"flagm", "flagm"},
    {"dit", "dit"},
    {"bti", "bti"},
    {"paca", "pauth"},
    {NULL, NULL}
};

const char *tp_get_host_cpu_name(void) {
    static const char *cpu_name = NULL;
    if (cpu_name) return cpu_name;

    size_t info_len;
    const char *info_data = load_cpuinfo(&info_len);
    tp_sv info;
    info.data = info_data;
    info.len = info_len;

    // Collect all distinct (implementer, part) pairs from all cores.
    tp_vec impl_all = cpuinfo_field_all(info, "CPU implementer");
    tp_vec part_all = cpuinfo_field_all(info, "CPU part");

    typedef struct { unsigned impl; unsigned part; } CoreInfo;
    tp_vec cores = tp_vec_new(sizeof(CoreInfo));

    unsigned default_impl = 0x41; // ARM Ltd.
    if (impl_all.count > 0) {
        tp_sv *sv = (tp_sv *)tp_vec_at(&impl_all, 0);
        tp_str tmp = tp_str_from_n(sv->data, sv->len);
        default_impl = (unsigned)strtoul(tp_str_cstr(&tmp), NULL, 0);
        tp_str_free(&tmp);
    }

    size_t pi;
    for (pi = 0; pi < part_all.count; pi++) {
        tp_sv *p = (tp_sv *)tp_vec_at(&part_all, pi);
        tp_str tmp = tp_str_from_n(p->data, p->len);
        unsigned part = (unsigned)strtoul(tp_str_cstr(&tmp), NULL, 0);
        tp_str_free(&tmp);
        CoreInfo ci;
        ci.impl = default_impl;
        ci.part = part;
        tp_vec_push(&cores, &ci);
    }

    const char *name = "generic";

    // Check for known big.LITTLE pairs first
    if (cores.count >= 2) {
        const BigLittlePair *bl;
        for (bl = big_little_pairs; bl->name; bl++) {
            int has_big = 0, has_little = 0;
            size_t ci;
            for (ci = 0; ci < cores.count; ci++) {
                CoreInfo *c = (CoreInfo *)tp_vec_at(&cores, ci);
                if (c->part == bl->big_part) has_big = 1;
                if (c->part == bl->little_part) has_little = 1;
            }
            if (has_big && has_little) {
                name = bl->name;
                break;
            }
        }
    }

    // If no big.LITTLE match, look up all cores and pick the one with the
    // most features (i.e. the "big" core on an unknown big.LITTLE system).
    if (strcmp(name, "generic") == 0 && cores.count > 0) {
        unsigned best_popcount = 0;
        size_t ci;
        for (ci = 0; ci < cores.count; ci++) {
            CoreInfo *c = (CoreInfo *)tp_vec_at(&cores, ci);
            const ArmCPUInfo *entry;
            for (entry = arm_cpus; entry->name; entry++) {
                if (entry->implementer == c->impl && entry->part == c->part) {
                    const CPUEntry *cpu = find_cpu(entry->name);
                    if (!cpu) continue;
                    unsigned pc = feature_popcount(&cpu->features);
                    if (pc > best_popcount) {
                        best_popcount = pc;
                        name = entry->name;
                    }
                    break;
                }
            }
        }
    }

    tp_vec_free(&impl_all);
    tp_vec_free(&part_all);
    tp_vec_free(&cores);

    if (!find_cpu(name))
        name = "generic";

    cpu_name = name;
    return cpu_name;
}

FeatureBits tp_get_host_features(void) {
    FeatureBits features;
    memset(&features, 0, sizeof(features));

    // Start with the features from the CPU table lookup.
    const char *cpu = tp_get_host_cpu_name();
    const CPUEntry *entry = find_cpu(cpu);
    if (entry)
        features = entry->features;

    // Layer on additional features detected from /proc/cpuinfo.
    size_t info_len;
    const char *info_data = load_cpuinfo(&info_len);
    tp_sv info;
    info.data = info_data;
    info.len = info_len;
    tp_sv feat_line = cpuinfo_field(info, "Features");
    if (feat_line.len > 0) {
        int has_aes = 0, has_pmull = 0, has_sha1 = 0, has_sha2 = 0;

        tp_vec tokens = tp_split(feat_line, ' ');
        size_t ti;
        for (ti = 0; ti < tokens.count; ti++) {
            tp_sv tok = *(tp_sv *)tp_vec_at(&tokens, ti);
            if (tp_sv_eq(tok, "aes")) has_aes = 1;
            else if (tp_sv_eq(tok, "pmull")) has_pmull = 1;
            else if (tp_sv_eq(tok, "sha1")) has_sha1 = 1;
            else if (tp_sv_eq(tok, "sha2")) has_sha2 = 1;

            const FeatureMap *m;
            for (m = aarch64_feature_map; m->linux_name; m++) {
                if (tp_sv_eq(tok, m->linux_name)) {
                    const FeatureEntry *fe = find_feature(m->llvm_name);
                    if (fe) feature_set(&features, fe->bit);
                    break;
                }
            }
        }
        tp_vec_free(&tokens);

        if (has_aes && has_pmull) {
            const FeatureEntry *fe = find_feature("aes");
            if (fe) feature_set(&features, fe->bit);
        }

        if (has_sha1 && has_sha2) {
            const FeatureEntry *fe = find_feature("sha2");
            if (fe) feature_set(&features, fe->bit);
        }
    }

    expand_implied(&features);
    return features;
}

#endif // platform
