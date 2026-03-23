// Cross-architecture dispatch.
// Routes queries to the appropriate per-arch table wrapper.

#include "cross_arch.h"
#include <string.h>

// Per-arch table accessors (defined in tables_*.c)
int tp_x86_64_lookup_cpu(const char *name, CrossFeatureBits *out);
unsigned tp_x86_64_feature_words(void);
unsigned tp_x86_64_nfeatures(void);
unsigned tp_x86_64_ncpus(void);
const char *tp_x86_64_feature_name_at(unsigned idx);
int tp_x86_64_feature_bit_at(unsigned idx);
int tp_x86_64_feature_bit_by_name(const char *name);
int tp_x86_64_feature_is_hw_by_name(const char *name);
const char *tp_x86_64_cpu_name_at(unsigned idx);
unsigned tp_x86_64_tables_version_major(void);

int tp_aarch64_lookup_cpu(const char *name, CrossFeatureBits *out);
unsigned tp_aarch64_feature_words(void);
unsigned tp_aarch64_nfeatures(void);
unsigned tp_aarch64_ncpus(void);
const char *tp_aarch64_feature_name_at(unsigned idx);
int tp_aarch64_feature_bit_at(unsigned idx);
int tp_aarch64_feature_bit_by_name(const char *name);
int tp_aarch64_feature_is_hw_by_name(const char *name);
const char *tp_aarch64_cpu_name_at(unsigned idx);
unsigned tp_aarch64_tables_version_major(void);

int tp_riscv64_lookup_cpu(const char *name, CrossFeatureBits *out);
unsigned tp_riscv64_feature_words(void);
unsigned tp_riscv64_nfeatures(void);
unsigned tp_riscv64_ncpus(void);
const char *tp_riscv64_feature_name_at(unsigned idx);
int tp_riscv64_feature_bit_at(unsigned idx);
int tp_riscv64_feature_bit_by_name(const char *name);
int tp_riscv64_feature_is_hw_by_name(const char *name);
const char *tp_riscv64_cpu_name_at(unsigned idx);
unsigned tp_riscv64_tables_version_major(void);

// Normalize arch name variants
static const char *normalize_arch(const char *arch) {
    if (!arch) return NULL;
    if (strcmp(arch, "x86_64") == 0 || strcmp(arch, "x86-64") == 0 ||
        strcmp(arch, "i686") == 0 || strcmp(arch, "i386") == 0)
        return "x86_64";
    if (strcmp(arch, "aarch64") == 0 || strcmp(arch, "arm64") == 0)
        return "aarch64";
    if (strcmp(arch, "riscv64") == 0)
        return "riscv64";
    return arch;
}

// Dispatch macros
#define DISPATCH0(arch_str, func) do { \
    const char *a = normalize_arch(arch_str); \
    if (!a) return 0; \
    if (strcmp(a, "x86_64") == 0)  return tp_x86_64_##func(); \
    if (strcmp(a, "aarch64") == 0) return tp_aarch64_##func(); \
    if (strcmp(a, "riscv64") == 0) return tp_riscv64_##func(); \
} while(0)

#define DISPATCH(arch_str, func, ...) do { \
    const char *a = normalize_arch(arch_str); \
    if (!a) return 0; \
    if (strcmp(a, "x86_64") == 0)  return tp_x86_64_##func(__VA_ARGS__); \
    if (strcmp(a, "aarch64") == 0) return tp_aarch64_##func(__VA_ARGS__); \
    if (strcmp(a, "riscv64") == 0) return tp_riscv64_##func(__VA_ARGS__); \
} while(0)

int tp_cross_lookup_cpu(const char *arch, const char *cpu_name,
                        CrossFeatureBits *features_out) {
    memset(features_out, 0, sizeof(*features_out));
    DISPATCH(arch, lookup_cpu, cpu_name, features_out);
    return 0;
}

unsigned tp_cross_feature_words(const char *arch) {
    DISPATCH0(arch, feature_words);
    return 0;
}

unsigned tp_cross_num_features(const char *arch) {
    DISPATCH0(arch, nfeatures);
    return 0;
}

unsigned tp_cross_num_cpus(const char *arch) {
    DISPATCH0(arch, ncpus);
    return 0;
}

const char *tp_cross_feature_name(const char *arch, unsigned idx) {
    DISPATCH(arch, feature_name_at, idx);
    return NULL;
}

int tp_cross_feature_bit_at(const char *arch, unsigned idx) {
    DISPATCH(arch, feature_bit_at, idx);
    return -1;
}

int tp_cross_feature_bit(const char *arch, const char *name) {
    DISPATCH(arch, feature_bit_by_name, name);
    return -1;
}

int tp_cross_feature_is_hw(const char *arch, const char *name) {
    DISPATCH(arch, feature_is_hw_by_name, name);
    return 0;
}

const char *tp_cross_cpu_name(const char *arch, unsigned idx) {
    DISPATCH(arch, cpu_name_at, idx);
    return NULL;
}

unsigned tp_cross_tables_version_major(const char *arch) {
    DISPATCH0(arch, tables_version_major);
    return 0;
}

#undef DISPATCH0
#undef DISPATCH
