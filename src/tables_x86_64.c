// Cross-arch table access for x86_64.
// Always compiled regardless of host architecture.

#include "target_tables_x86_64.h"
#include "cpu_aliases.h"
#include "cross_arch.h"
#include <string.h>

static const CPUEntry *tables_x86_find_cpu(const char *name) {
    return _find_cpu_exact(resolve_cpu_alias(name));
}

int tp_x86_64_lookup_cpu(const char *name, CrossFeatureBits *out) {
    const CPUEntry *c = tables_x86_find_cpu(name);
    int i;
    if (!c) return 0;
    memset(out, 0, sizeof(*out));
    out->num_words = TARGET_FEATURE_WORDS;
    for (i = 0; i < TARGET_FEATURE_WORDS; i++)
        out->bits[i] = c->features.bits[i] & hw_feature_mask.bits[i];
    return 1;
}

unsigned tp_x86_64_feature_words(void) { return TARGET_FEATURE_WORDS; }
unsigned tp_x86_64_nfeatures(void) { return num_features; }
unsigned tp_x86_64_ncpus(void) { return num_cpus; }

const char *tp_x86_64_feature_name_at(unsigned idx) {
    return idx < num_features ? feature_table[idx].name : NULL;
}

int tp_x86_64_feature_bit_at(unsigned idx) {
    return idx < num_features ? (int)feature_table[idx].bit : -1;
}

int tp_x86_64_feature_bit_by_name(const char *name) {
    const FeatureEntry *fe = find_feature(name);
    return fe ? (int)fe->bit : -1;
}

int tp_x86_64_feature_is_hw_by_name(const char *name) {
    const FeatureEntry *fe = find_feature(name);
    return fe && fe->is_hw;
}

const char *tp_x86_64_cpu_name_at(unsigned idx) {
    return idx < num_cpus ? cpu_table[idx].name : NULL;
}

unsigned tp_x86_64_tables_version_major(void) {
#ifdef TARGET_TABLES_LLVM_VERSION_MAJOR
    return TARGET_TABLES_LLVM_VERSION_MAJOR;
#else
    return 0;
#endif
}
