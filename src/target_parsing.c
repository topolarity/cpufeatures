// Standalone target parsing library implementation.
// No LLVM runtime dependency - uses pre-generated tables.

// Include generated tables FIRST (defines FeatureBits etc.)
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include "target_tables_x86_64.h"
#elif defined(__aarch64__) || defined(_M_ARM64)
#include "target_tables_aarch64.h"
#elif defined(__riscv) && __riscv_xlen == 64
#include "target_tables_riscv64.h"
#else
#error "Unsupported architecture - generate tables with gen_target_tables"
#endif

#include "target_parsing.h"

#include <string.h>
#include <stdio.h>

// ============================================================================
// Internal helpers for building arrays during parsing
// ============================================================================

/* Duplicate a tp_sv as a heap-allocated C string */
static char *sv_strdup(tp_sv sv) {
    char *s = (char *)malloc(sv.len + 1);
    memcpy(s, sv.data, sv.len);
    s[sv.len] = '\0';
    return s;
}

/* Convert a tp_vec of char* into a NULL-terminated char** array.
   Consumes (frees) the vec. */
static char **vec_to_strarray(tp_vec *v) {
    char **arr = (char **)malloc((v->count + 1) * sizeof(char *));
    size_t i;
    for (i = 0; i < v->count; i++)
        arr[i] = *(char **)tp_vec_at(v, i);
    arr[v->count] = NULL;
    tp_vec_free(v);
    return arr;
}

// ============================================================================
// Target string parsing
// ============================================================================

ParsedTarget **tp_parse_target_string(const char *target_str) {
    tp_vec result = tp_vec_new(sizeof(ParsedTarget));

    if (!target_str || !*target_str) {
        ParsedTarget t;
        memset(&t, 0, sizeof(t));
        t.cpu_name = strdup("native");
        t.base = -1;
        t.extra_features = (char **)calloc(1, sizeof(char *)); // NULL terminator
        tp_vec_push(&result, &t);
        return (ParsedTarget **)tp_vec_to_null_terminated(&result);
    }

    tp_sv full = tp_sv_from(target_str);
    tp_vec targets = tp_split(full, ';');
    size_t ti;

    for (ti = 0; ti < targets.count; ti++) {
        tp_sv target_sv = *(tp_sv *)tp_vec_at(&targets, ti);
        ParsedTarget t;
        memset(&t, 0, sizeof(t));
        t.base = -1;

        tp_vec tokens = tp_split(target_sv, ',');
        if (tokens.count == 0) { tp_vec_free(&tokens); continue; }

        tp_sv tok0 = *(tp_sv *)tp_vec_at(&tokens, 0);
        t.cpu_name = sv_strdup(tok0);

        // Collect extra features in a temp vec of char*
        tp_vec extras = tp_vec_new(sizeof(char *));

        size_t i;
        for (i = 1; i < tokens.count; i++) {
            tp_sv tok = *(tp_sv *)tp_vec_at(&tokens, i);
            if (tp_sv_eq(tok, "clone_all"))       t.flags |= TF_CLONE_ALL;
            else if (tp_sv_eq(tok, "-clone_all")) t.flags &= ~TF_CLONE_ALL;
            else if (tp_sv_eq(tok, "opt_size"))   t.flags |= TF_OPTSIZE;
            else if (tp_sv_eq(tok, "min_size"))   t.flags |= TF_MINSIZE;
            else if (tok.len > 5 && memcmp(tok.data, "base(", 5) == 0 && tok.data[tok.len-1] == ')') {
                char *num_str = sv_strdup((tp_sv){tok.data + 5, tok.len - 6});
                t.base = atoi(num_str);
                free(num_str);
            } else if (tok.len > 0 && (tok.data[0] == '+' || tok.data[0] == '-')) {
                char *feat = sv_strdup(tok);
                tp_vec_push(&extras, &feat);
            }
        }

        t.extra_features = vec_to_strarray(&extras);

        tp_vec_free(&tokens);
        tp_vec_push(&result, &t);
    }

    tp_vec_free(&targets);
    return (ParsedTarget **)tp_vec_to_null_terminated(&result);
}

// ============================================================================
// Target resolution
// ============================================================================

ResolvedTarget **tp_resolve_targets(
        ParsedTarget *const *parsed,
        const FeatureBits *host_features,
        const char *host_cpu) {

    tp_vec result = tp_vec_new(sizeof(ResolvedTarget));
    size_t i;

    for (i = 0; parsed[i]; i++) {
        const ParsedTarget *pt = parsed[i];
        ResolvedTarget rt;
        memset(&rt, 0, sizeof(rt));
        rt.flags = pt->flags;
        rt.base = pt->base >= 0 ? pt->base : (i > 0 ? 0 : -1);

        const char *name = pt->cpu_name;

        if (strcmp(name, "native") == 0 || !*name) {
            if (host_cpu && *host_cpu)
                rt.cpu_name = strdup(host_cpu);
            else
                rt.cpu_name = strdup(tp_get_host_cpu_name());

            if (host_features)
                rt.features = *host_features;
            else
                rt.features = tp_get_host_features();
        } else {
            rt.cpu_name = strdup(name);
            const CPUEntry *cpu = find_cpu(name);
            if (cpu) {
                rt.features = cpu->features;
            } else {
                fprintf(stderr, "target_parsing: unknown CPU '%s'\n", name);
                rt.flags |= TF_UNKNOWN_NAME;
                const CPUEntry *gen = find_cpu("generic");
                if (gen) rt.features = gen->features;
            }
        }

        tp_str ext = tp_str_new();

        char **f;
        for (f = pt->extra_features; *f; f++) {
            const char *feat = *f;
            int enable = (feat[0] == '+');
            const char *fname = feat + 1;

            const FeatureEntry *fe = find_feature(fname);
            if (fe) {
                if (enable) {
                    feature_set(&rt.features, fe->bit);
                    feature_or(&rt.features, &fe->implies);
                    expand_implied(&rt.features);
                } else {
                    unsigned k;
                    feature_clear(&rt.features, fe->bit);
                    for (k = 0; k < num_features; k++) {
                        if (feature_test(&feature_table[k].implies, fe->bit))
                            feature_clear(&rt.features, feature_table[k].bit);
                    }
                }
            } else {
                if (ext.len > 0)
                    tp_str_append_char(&ext, ',');
                tp_str_append(&ext, feat);
            }
        }

        rt.ext_features = tp_str_take(&ext);
        tp_vec_push(&result, &rt);
    }

    return (ResolvedTarget **)tp_vec_to_null_terminated(&result);
}

// ============================================================================
// Feature diff computation
// ============================================================================

FeatureDiff tp_compute_feature_diff(const FeatureBits *base,
                                     const FeatureBits *derived) {
    FeatureBits diff;
    feature_andnot(&diff, derived, base);

    FeatureDiff result;
    memset(&result, 0, sizeof(result));

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    result.has_new_math = tp_has_feature(&diff, "fma") || tp_has_feature(&diff, "fma4");
    result.has_new_simd = tp_has_feature(&diff, "avx") || tp_has_feature(&diff, "avx2") ||
                          tp_has_feature(&diff, "avx512f") || tp_has_feature(&diff, "sse4.1");
    result.has_new_float16 = tp_has_feature(&diff, "avx512fp16");
    result.has_new_bfloat16 = tp_has_feature(&diff, "avx512bf16");
#elif defined(__aarch64__) || defined(_M_ARM64)
    result.has_new_simd = tp_has_feature(&diff, "sve") || tp_has_feature(&diff, "sve2");
    result.has_new_float16 = tp_has_feature(&diff, "fullfp16");
    result.has_new_bfloat16 = tp_has_feature(&diff, "bf16");
#elif defined(__riscv)
    result.has_new_simd = tp_has_feature(&diff, "v") || tp_has_feature(&diff, "zve32x") ||
                          tp_has_feature(&diff, "zve64d");
    result.has_new_float16 = tp_has_feature(&diff, "zfh");
    result.has_new_bfloat16 = tp_has_feature(&diff, "zvfbfmin");
#endif

    return result;
}

// ============================================================================
// Vector register size
// ============================================================================

int tp_max_vector_size(const FeatureBits *bits) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    if (tp_has_feature(bits, "avx512f")) {
        if (!find_feature("evex512") || tp_has_feature(bits, "evex512"))
            return 64;
        return 32;
    }
    if (tp_has_feature(bits, "avx"))
        return 32;
    return 16;
#elif defined(__aarch64__) || defined(_M_ARM64)
    if (tp_has_feature(bits, "sve"))
        return 256; // SVE is scalable, use large number
    return 16;
#elif defined(__riscv)
    if (tp_has_feature(bits, "v") || tp_has_feature(bits, "zve64d"))
        return 128; // RVV scalable
    if (tp_has_feature(bits, "zve32x"))
        return 32;
    return 0;
#else
    (void)bits;
    return 16;
#endif
}

// ============================================================================
// hw_feature_mask access
// ============================================================================

const FeatureBits *tp_get_hw_feature_mask(void) {
    return &hw_feature_mask;
}

// ============================================================================
// Feature string generation
// ============================================================================

char *tp_build_feature_string(const FeatureBits *features,
                               const FeatureBits *baseline) {
    tp_str result = tp_str_new();
    unsigned i;
    for (i = 0; i < num_features; i++) {
        int in_feat = feature_test(features, feature_table[i].bit);
        int in_base = baseline ? feature_test(baseline, feature_table[i].bit) : 0;

        if (in_feat && !in_base) {
            if (result.len > 0) tp_str_append_char(&result, ',');
            tp_str_append_char(&result, '+');
            tp_str_append(&result, feature_table[i].name);
        } else if (!in_feat && in_base) {
            if (result.len > 0) tp_str_append_char(&result, ',');
            tp_str_append_char(&result, '-');
            tp_str_append(&result, feature_table[i].name);
        }
    }
    return tp_str_take(&result);
}

// Build LLVM feature string: hw-only, with baseline features appended
char *tp_build_llvm_feature_string(const FeatureBits *enabled,
                                    const FeatureBits *disabled) {
    tp_str result = tp_str_new();
    unsigned i;
    for (i = 0; i < num_features; i++) {
        if (!feature_test(&hw_feature_mask, feature_table[i].bit))
            continue;
        int in_en = feature_test(enabled, feature_table[i].bit);
        int in_dis = feature_test(disabled, feature_table[i].bit);
        if (in_en) {
            if (result.len > 0) tp_str_append_char(&result, ',');
            tp_str_append_char(&result, '+');
            tp_str_append(&result, feature_table[i].name);
        } else if (in_dis) {
            if (result.len > 0) tp_str_append_char(&result, ',');
            tp_str_append_char(&result, '-');
            tp_str_append(&result, feature_table[i].name);
        }
    }

    // Arch-specific baseline features always required
#if defined(__x86_64__) || defined(_M_X64)
    tp_str_append(&result, ",+sse2,+mmx,+fxsr,+64bit,+cx8");
#elif defined(__i386__) || defined(_M_IX86)
    tp_str_append(&result, ",+sse2,+mmx,+fxsr,+cx8");
#endif

    return tp_str_take(&result);
}

// Normalize CPU name for LLVM (-mcpu value)
static char *normalize_cpu_for_llvm(const char *name) {
#if defined(__x86_64__) || defined(_M_X64)
    if (strcmp(name, "generic") == 0 || strcmp(name, "x86-64") == 0 || strcmp(name, "x86_64") == 0)
        return strdup("x86-64");
#elif defined(__i386__) || defined(_M_IX86)
    if (strcmp(name, "generic") == 0 || strcmp(name, "i686") == 0 || strcmp(name, "pentium4") == 0)
        return strdup("pentium4");
#endif
    return strdup(name);
}

// Strip features that LLVM doesn't use for codegen and rr disables
static void strip_nondeterministic_features(FeatureBits *features) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    static const char *features_to_strip[] = {
        "rdrnd", "rdseed", "rtm", "xsaveopt", NULL
    };
    const char **f;
    for (f = features_to_strip; *f; f++) {
        const FeatureEntry *fe = find_feature(*f);
        if (fe) feature_clear(features, fe->bit);
    }
#else
    (void)features;
#endif
}

// ============================================================================
// High-level: tp_resolve_targets_for_llvm
// ============================================================================

LLVMTargetSpec **tp_resolve_targets_for_llvm(
        const char *target_str,
        const ResolveOptions *opts) {

    ResolveOptions default_opts;
    if (!opts) {
        default_opts = resolve_options_default();
        opts = &default_opts;
    }

    // 1. Parse
    ParsedTarget **parsed = tp_parse_target_string(target_str);

    // 2. Resolve against CPU database
    ResolvedTarget **resolved = tp_resolve_targets(
        (ParsedTarget *const *)parsed, opts->host_features, opts->host_cpu);

    // Count resolved targets
    size_t num_resolved = 0;
    while (resolved[num_resolved]) num_resolved++;

    // 3. Post-process each target
    size_t i;
    for (i = 0; i < num_resolved; i++) {
        ResolvedTarget *rt = resolved[i];

        // Strip nondeterministic features (rdrnd etc.)
        if (opts->strip_nondeterministic)
            strip_nondeterministic_features(&rt->features);

        expand_implied(&rt->features);
    }

    // 4. Build LLVM specs with diffs
    tp_vec result = tp_vec_new(sizeof(LLVMTargetSpec));

    for (i = 0; i < num_resolved; i++) {
        const ResolvedTarget *rt = resolved[i];
        LLVMTargetSpec spec;
        memset(&spec, 0, sizeof(spec));

        spec.cpu_name = normalize_cpu_for_llvm(rt->cpu_name);
        spec.flags = rt->flags;
        spec.base = rt->base;
        spec.ext_features = strdup(rt->ext_features);

        // Compute hw-masked enabled and disabled features
        int w;
        for (w = 0; w < TARGET_FEATURE_WORDS; w++) {
            spec.en_features.bits[w] = rt->features.bits[w] & hw_feature_mask.bits[w];
            spec.dis_features.bits[w] = hw_feature_mask.bits[w] & ~rt->features.bits[w];
        }

        // Build LLVM feature string
        tp_str feat_str = tp_str_new();
        char *base_feats = tp_build_llvm_feature_string(&spec.en_features, &spec.dis_features);
        tp_str_append(&feat_str, base_feats);
        free(base_feats);

        // Append ext_features
        if (rt->ext_features[0]) {
            tp_str_append_char(&feat_str, ',');
            tp_str_append(&feat_str, rt->ext_features);
        }
        spec.cpu_features = tp_str_take(&feat_str);

        // Compute diff from base target
        if (i > 0) {
            int base_idx = rt->base >= 0 ? rt->base : 0;
            spec.diff = tp_compute_feature_diff(&resolved[base_idx]->features, &rt->features);
        }

        tp_vec_push(&result, &spec);
    }

    tp_parsed_targets_destroy(parsed);
    tp_resolved_targets_destroy(resolved);
    return (LLVMTargetSpec **)tp_vec_to_null_terminated(&result);
}

// ============================================================================
// Sysimage serialization
// ============================================================================

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} tp_byte_buf_internal;

static tp_byte_buf_internal bb_new(void) {
    tp_byte_buf_internal b = {NULL, 0, 0};
    return b;
}

static void bb_emit(tp_byte_buf_internal *b, const void *data, size_t sz) {
    if (b->len + sz > b->cap) {
        size_t newcap = b->cap ? b->cap * 2 : 256;
        while (newcap < b->len + sz) newcap *= 2;
        b->data = (uint8_t *)realloc(b->data, newcap);
        b->cap = newcap;
    }
    memcpy(b->data + b->len, data, sz);
    b->len += sz;
}

static void bb_emit_u32(tp_byte_buf_internal *b, uint32_t v) {
    bb_emit(b, &v, 4);
}

static void bb_emit_str(tp_byte_buf_internal *b, const char *s) {
    uint32_t slen = (uint32_t)strlen(s);
    bb_emit_u32(b, slen);
    bb_emit(b, s, slen);
}

tp_byte_buf tp_serialize_targets(LLVMTargetSpec *const *targets) {
    tp_byte_buf_internal buf = bb_new();
    size_t count = 0;
    while (targets[count]) count++;
    size_t i;

    bb_emit_u32(&buf, (uint32_t)count);
    for (i = 0; i < count; i++) {
        const LLVMTargetSpec *t = targets[i];
        bb_emit_u32(&buf, t->flags);
        bb_emit_u32(&buf, (uint32_t)t->base);
        bb_emit_u32(&buf, TARGET_FEATURE_WORDS);
        bb_emit(&buf, t->en_features.bits, sizeof(t->en_features.bits));
        bb_emit(&buf, t->dis_features.bits, sizeof(t->dis_features.bits));
        bb_emit_str(&buf, t->cpu_name);
        bb_emit_str(&buf, t->ext_features);
    }

    tp_byte_buf result;
    result.data = buf.data;
    result.len = buf.len;
    return result;
}

LLVMTargetSpec **tp_deserialize_targets(const uint8_t *data) {
    const uint8_t *p = data;

    uint32_t ntargets;
    memcpy(&ntargets, p, 4); p += 4;

    tp_vec result = tp_vec_new(sizeof(LLVMTargetSpec));
    uint32_t i;

    for (i = 0; i < ntargets; i++) {
        LLVMTargetSpec t;
        memset(&t, 0, sizeof(t));

        memcpy(&t.flags, p, 4); p += 4;
        uint32_t base_u32;
        memcpy(&base_u32, p, 4); p += 4;
        t.base = (int)base_u32;

        uint32_t nwords;
        memcpy(&nwords, p, 4); p += 4;

        // Read feature bits, handle size mismatch gracefully
        memset(&t.en_features, 0, sizeof(t.en_features));
        memset(&t.dis_features, 0, sizeof(t.dis_features));
        unsigned words_to_read = nwords < TARGET_FEATURE_WORDS ? nwords : TARGET_FEATURE_WORDS;
        memcpy(t.en_features.bits, p, words_to_read * sizeof(uint64_t));
        p += nwords * sizeof(uint64_t);
        memcpy(t.dis_features.bits, p, words_to_read * sizeof(uint64_t));
        p += nwords * sizeof(uint64_t);

        uint32_t slen;
        memcpy(&slen, p, 4); p += 4;
        t.cpu_name = (char *)malloc(slen + 1);
        memcpy(t.cpu_name, p, slen);
        t.cpu_name[slen] = '\0';
        p += slen;

        memcpy(&slen, p, 4); p += 4;
        t.ext_features = (char *)malloc(slen + 1);
        memcpy(t.ext_features, p, slen);
        t.ext_features[slen] = '\0';
        p += slen;

        t.cpu_features = strdup("");
        tp_vec_push(&result, &t);
    }

    return (LLVMTargetSpec **)tp_vec_to_null_terminated(&result);
}

// ============================================================================
// Sysimage target matching
// ============================================================================

TargetMatch tp_match_targets(LLVMTargetSpec *const *targets,
                              const LLVMTargetSpec *host) {
    TargetMatch match;
    match.best_idx = -1;
    match.vreg_size = 0;
    int match_name = 0;
    int best_feat_count = 0;
    int i;

    for (i = 0; targets[i]; i++) {
        // Check: target must not enable features the host has disabled
        FeatureBits conflict;
        feature_and_out(&conflict, &targets[i]->en_features, &host->dis_features);
        if (feature_any(&conflict))
            continue;

        int name_match = (strcmp(targets[i]->cpu_name, host->cpu_name) == 0);
        int vreg = tp_max_vector_size(&targets[i]->en_features);
        int feat_count = (int)feature_popcount(&targets[i]->en_features);

        if (name_match && !match_name) {
            // First name match resets the search
            match_name = 1;
            match.vreg_size = 0;
            best_feat_count = 0;
        }
        if (match_name && !name_match)
            continue;

        if (vreg > match.vreg_size ||
            (vreg == match.vreg_size && feat_count > best_feat_count) ||
            (vreg == match.vreg_size && feat_count == best_feat_count && i > match.best_idx)) {
            match.best_idx = i;
            match.vreg_size = vreg;
            best_feat_count = feat_count;
        }
    }

    return match;
}
