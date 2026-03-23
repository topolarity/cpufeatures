// Standalone target parsing library.
// Uses CPU/feature tables generated at build time from LLVM's TableGen data.
// Zero LLVM runtime dependency.
//
// Usage: #include the generated table header first, then this header.
//   #include "target_tables_x86_64.h"
//   #include "target_parsing.h"

#ifndef TARGET_PARSING_H
#define TARGET_PARSING_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Verify that a generated table header was included first
#ifndef TARGET_FEATURE_WORDS
#error "Include the generated target table header before target_parsing.h"
#endif

// CPU name alias resolution + find_cpu wrapper.
#include "cpu_aliases.h"

static inline const CPUEntry *find_cpu(const char *name) {
    return _find_cpu_exact(resolve_cpu_alias(name));
}

// ============================================================================
// Dynamic string helper (simple, allocation-based)
// ============================================================================

// Dynamic string (owns its memory, always null-terminated).
// Used internally for building strings. Public API returns char*.
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} tp_str;

/* Initialize an empty string */
static inline tp_str tp_str_new(void) {
    tp_str s = {NULL, 0, 0};
    return s;
}

/* Free a dynamic string's backing memory */
static inline void tp_str_free(tp_str *s) {
    free(s->data);
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
}

/* Ensure capacity for at least `needed` bytes plus null terminator */
static inline void tp_str_grow(tp_str *s, size_t needed) {
    if (needed + 1 <= s->cap) return;
    size_t newcap = s->cap ? s->cap * 2 : 32;
    while (newcap < needed + 1) newcap *= 2;
    s->data = (char *)realloc(s->data, newcap);
    s->cap = newcap;
}

/* Create from a C string */
static inline tp_str tp_str_from(const char *c) {
    tp_str s;
    s.len = strlen(c);
    s.cap = s.len + 1;
    s.data = (char *)malloc(s.cap);
    memcpy(s.data, c, s.cap);
    return s;
}

/* Create from pointer + length */
static inline tp_str tp_str_from_n(const char *c, size_t n) {
    tp_str s;
    s.len = n;
    s.cap = n + 1;
    s.data = (char *)malloc(s.cap);
    memcpy(s.data, c, n);
    s.data[n] = '\0';
    return s;
}

/* Append a C string */
static inline void tp_str_append(tp_str *s, const char *c) {
    size_t clen = strlen(c);
    tp_str_grow(s, s->len + clen);
    memcpy(s->data + s->len, c, clen + 1);
    s->len += clen;
}

/* Append a single character */
static inline void tp_str_append_char(tp_str *s, char c) {
    tp_str_grow(s, s->len + 1);
    s->data[s->len++] = c;
    s->data[s->len] = '\0';
}

/* Return C string (never NULL for initialized strings) */
static inline const char *tp_str_cstr(const tp_str *s) {
    return s->data ? s->data : "";
}

/* Detach the backing buffer so the caller owns it. Resets the tp_str. */
static inline char *tp_str_take(tp_str *s) {
    char *p = s->data;
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
    return p ? p : (char *)calloc(1, 1);
}

// ============================================================================
// Non-owning string view
// ============================================================================

typedef struct {
    const char *data;
    size_t len;
} tp_sv;

static inline tp_sv tp_sv_from(const char *c) {
    tp_sv sv;
    sv.data = c;
    sv.len = strlen(c);
    return sv;
}

static inline int tp_sv_eq(tp_sv a, const char *b) {
    size_t blen = strlen(b);
    return a.len == blen && memcmp(a.data, b, blen) == 0;
}

static inline tp_sv tp_trim(tp_sv sv) {
    while (sv.len > 0 && (sv.data[0] == ' ' || sv.data[0] == '\t')) {
        sv.data++; sv.len--;
    }
    while (sv.len > 0 && (sv.data[sv.len-1] == ' ' || sv.data[sv.len-1] == '\t'))
        sv.len--;
    return sv;
}

// ============================================================================
// Generic dynamic array (internal helper)
// ============================================================================

// Used internally for building arrays. Public API returns NULL-terminated
// arrays of pointers.
typedef struct {
    void *items;
    size_t count;
    size_t cap;
    size_t elem_size;
} tp_vec;

/* Create a new empty vec for elements of the given size */
static inline tp_vec tp_vec_new(size_t elem_size) {
    tp_vec v = {NULL, 0, 0, elem_size};
    return v;
}

/* Push a shallow copy of *elem onto the vec */
static inline void tp_vec_push(tp_vec *v, const void *elem) {
    if (v->count >= v->cap) {
        v->cap = v->cap ? v->cap * 2 : 4;
        v->items = realloc(v->items, v->cap * v->elem_size);
    }
    memcpy((char *)v->items + v->count * v->elem_size, elem, v->elem_size);
    v->count++;
}

/* Get pointer to element at index */
static inline void *tp_vec_at(const tp_vec *v, size_t i) {
    return (char *)v->items + i * v->elem_size;
}

/* Free the vec's backing storage (does NOT free pointed-to data in elements) */
static inline void tp_vec_free(tp_vec *v) {
    free(v->items);
    v->items = NULL;
    v->count = 0;
    v->cap = 0;
}

/* Convert a tp_vec of T into a NULL-terminated array of T*.
   Each element is heap-allocated and copied from the vec.
   The vec is consumed (freed). */
static inline void **tp_vec_to_null_terminated(tp_vec *v) {
    void **arr = (void **)malloc((v->count + 1) * sizeof(void *));
    size_t i;
    for (i = 0; i < v->count; i++) {
        arr[i] = malloc(v->elem_size);
        memcpy(arr[i], (char *)v->items + i * v->elem_size, v->elem_size);
    }
    arr[v->count] = NULL;
    tp_vec_free(v);
    return arr;
}

// ============================================================================
// Split utility
// ============================================================================

/* Split a string view by delimiter, trimming whitespace from each piece.
   Returns a tp_vec of tp_sv (internal use). */
static inline tp_vec tp_split(tp_sv sv, char delim) {
    tp_vec result = tp_vec_new(sizeof(tp_sv));
    while (sv.len > 0) {
        const char *p = (const char *)memchr(sv.data, delim, sv.len);
        if (!p) {
            tp_sv piece = tp_trim(sv);
            if (piece.len > 0) tp_vec_push(&result, &piece);
            break;
        }
        tp_sv piece;
        piece.data = sv.data;
        piece.len = (size_t)(p - sv.data);
        piece = tp_trim(piece);
        if (piece.len > 0) tp_vec_push(&result, &piece);
        size_t consumed = (size_t)(p - sv.data) + 1;
        sv.data += consumed;
        sv.len -= consumed;
    }
    return result;
}

// ============================================================================
// Target parsing flags
// ============================================================================

enum {
    TF_CLONE_ALL    = 1 << 1,
    TF_UNKNOWN_NAME = 1 << 5,
    TF_OPTSIZE      = 1 << 6,
    TF_MINSIZE      = 1 << 7,
};

// ============================================================================
// Public types
// ============================================================================

// A parsed target from the target string.
// All strings are owned (heap-allocated). Free with tp_parsed_targets_free().
typedef struct {
    char *cpu_name;
    uint32_t flags;
    int base;
    char **extra_features;     // NULL-terminated array of "+feat" / "-feat" strings
} ParsedTarget;

// A fully resolved target (features resolved, not yet LLVM-ready)
typedef struct {
    char *cpu_name;
    FeatureBits features;
    uint32_t flags;
    int base;
    char *ext_features;        // comma-separated pass-through features (or "")
} ResolvedTarget;

// What changed between a derived target and its base, in codegen terms
typedef struct {
    int has_new_math;     // FMA/FMA4 (x86)
    int has_new_simd;     // SSE4.1/AVX/AVX2/AVX512, SVE, RVV
    int has_new_float16;  // avx512fp16, fullfp16, zfh
    int has_new_bfloat16; // avx512bf16, bf16, zvfbfmin
} FeatureDiff;

// A fully resolved target ready for LLVM consumption
typedef struct {
    char *cpu_name;            // Normalized for LLVM (-mcpu)
    char *cpu_features;        // "+avx2,-sse4a,..." (-mattr), hw-only with baseline
    FeatureBits en_features;   // Enabled features (hw-masked)
    FeatureBits dis_features;  // Disabled features (hw-masked complement)
    uint32_t flags;
    int base;
    char *ext_features;        // Pass-through features unknown to the library
    FeatureDiff diff;          // What's new vs base target
} LLVMTargetSpec;

// Options for tp_resolve_targets_for_llvm
typedef struct {
    const FeatureBits *host_features; // NULL = auto-detect
    const char *host_cpu;             // NULL = auto-detect
    int strip_nondeterministic;       // strip rdrnd/rdseed/rtm/xsaveopt (x86)
} ResolveOptions;

static inline ResolveOptions resolve_options_default(void) {
    ResolveOptions opts;
    opts.host_features = NULL;
    opts.host_cpu = NULL;
    opts.strip_nondeterministic = 1;
    return opts;
}

// Check if a feature bitset contains a specific feature by name
static inline int tp_has_feature(const FeatureBits *bits, const char *name) {
    const FeatureEntry *fe = find_feature(name);
    return fe && feature_test(bits, fe->bit);
}

// ============================================================================
// Destroy functions for returned NULL-terminated arrays
// ============================================================================

static inline void tp_parsed_target_destroy(ParsedTarget *t) {
    char **f;
    free(t->cpu_name);
    if (t->extra_features) {
        for (f = t->extra_features; *f; f++) free(*f);
        free(t->extra_features);
    }
}

/* Destroy a NULL-terminated array of ParsedTarget* */
static inline void tp_parsed_targets_destroy(ParsedTarget **targets) {
    ParsedTarget **p;
    if (!targets) return;
    for (p = targets; *p; p++) {
        tp_parsed_target_destroy(*p);
        free(*p);
    }
    free(targets);
}

static inline void tp_resolved_target_destroy(ResolvedTarget *t) {
    free(t->cpu_name);
    free(t->ext_features);
}

/* Destroy a NULL-terminated array of ResolvedTarget* */
static inline void tp_resolved_targets_destroy(ResolvedTarget **targets) {
    ResolvedTarget **p;
    if (!targets) return;
    for (p = targets; *p; p++) {
        tp_resolved_target_destroy(*p);
        free(*p);
    }
    free(targets);
}

static inline void tp_llvm_spec_destroy(LLVMTargetSpec *t) {
    free(t->cpu_name);
    free(t->cpu_features);
    free(t->ext_features);
}

/* Destroy a NULL-terminated array of LLVMTargetSpec* */
static inline void tp_llvm_specs_destroy(LLVMTargetSpec **specs) {
    LLVMTargetSpec **p;
    if (!specs) return;
    for (p = specs; *p; p++) {
        tp_llvm_spec_destroy(*p);
        free(*p);
    }
    free(specs);
}

// ============================================================================
// Low-level API (building blocks)
// ============================================================================

// Parse a target string like "haswell;skylake,+avx512f,-sse4a".
// Returns NULL-terminated array of ParsedTarget*.
// Caller must free with tp_parsed_targets_free().
ParsedTarget **tp_parse_target_string(const char *target_str);

// Resolve parsed targets against the CPU/feature database.
// `parsed` is a NULL-terminated array of ParsedTarget*.
// Returns NULL-terminated array of ResolvedTarget*.
// Caller must free with tp_resolved_targets_free().
ResolvedTarget **tp_resolve_targets(
    ParsedTarget *const *parsed,
    const FeatureBits *host_features,
    const char *host_cpu);

// Build a raw feature diff string (for debug, not filtered for LLVM).
// Returns heap-allocated string. Caller must free().
char *tp_build_feature_string(const FeatureBits *features,
                              const FeatureBits *baseline);

// Build LLVM-ready feature string: hw-only features with baseline appended.
// Returns heap-allocated string. Caller must free().
char *tp_build_llvm_feature_string(const FeatureBits *enabled,
                                    const FeatureBits *disabled);

// ============================================================================
// High-level API (one-shot, LLVM-ready)
// ============================================================================

// Target string → LLVM-ready specs with diffs computed.
// Returns NULL-terminated array of LLVMTargetSpec*.
// Caller must free with tp_llvm_specs_free().
LLVMTargetSpec **tp_resolve_targets_for_llvm(
    const char *target_str,
    const ResolveOptions *opts);

// Compute feature diff between a base and derived feature set
FeatureDiff tp_compute_feature_diff(const FeatureBits *base,
                                    const FeatureBits *derived);

// Max vector register size in bytes for a feature set
int tp_max_vector_size(const FeatureBits *features);

// Access the hw_feature_mask from generated tables
const FeatureBits *tp_get_hw_feature_mask(void);

// ============================================================================
// Sysimage serialization and matching
// ============================================================================

// Byte buffer for serialization
typedef struct {
    uint8_t *data;
    size_t len;
} tp_byte_buf;

// Serialize all targets for embedding in sysimages.
// `targets` is a NULL-terminated array of LLVMTargetSpec*.
// Returns heap-allocated buffer. Caller must free buf.data.
tp_byte_buf tp_serialize_targets(LLVMTargetSpec *const *targets);

// Deserialize targets from binary data.
// Returns NULL-terminated array of LLVMTargetSpec*.
// Caller must free with tp_llvm_specs_free().
LLVMTargetSpec **tp_deserialize_targets(const uint8_t *data);

// Result of target matching
typedef struct {
    int best_idx;
    int vreg_size;
} TargetMatch;

// Match a host target against a set of compiled targets.
// `targets` is a NULL-terminated array of LLVMTargetSpec*.
TargetMatch tp_match_targets(LLVMTargetSpec *const *targets,
                             const LLVMTargetSpec *host);

// ============================================================================
// Host detection
// ============================================================================

const char *tp_get_host_cpu_name(void);
FeatureBits tp_get_host_features(void);

#endif // TARGET_PARSING_H
