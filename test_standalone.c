// Test the standalone target parsing library - NO LLVM dependency!

// Include the right table for the host architecture
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include "target_tables_x86_64.h"
#elif defined(__aarch64__) || defined(_M_ARM64)
#include "target_tables_aarch64.h"
#elif defined(__riscv) && __riscv_xlen == 64
#include "target_tables_riscv64.h"
#endif
#include "target_parsing.h"
#include "cross_arch.h"

#include <stdio.h>

static int failures = 0;

static void check(int cond, const char *msg) {
    if (!cond) { printf("  FAIL: %s\n", msg); failures++; }
}

/* Count elements in a NULL-terminated pointer array */
static size_t ptrlen(void *const *arr) {
    size_t n = 0;
    while (arr[n]) n++;
    return n;
}

static void print_hw_features(const FeatureBits *bits) {
    int first = 1;
    unsigned i;
    for (i = 0; i < num_features; i++) {
        if (feature_test(bits, feature_table[i].bit)) {
            if (!first) printf(", ");
            printf("%s", feature_table[i].name);
            first = 0;
        }
    }
    printf(" (%u total)\n", feature_popcount(bits));
}

static void test_parse(const char *target_str) {
    printf("\n--- Parsing: \"%s\" ---\n", target_str);
    ParsedTarget **parsed = tp_parse_target_string(target_str);
    size_t count = ptrlen((void *const *)parsed);
    printf("  %zu target(s) parsed\n", count);

    size_t i;
    for (i = 0; parsed[i]; i++) {
        printf("  [%zu] cpu=%s", i, parsed[i]->cpu_name);
        if (parsed[i]->flags) printf(" flags=0x%x", parsed[i]->flags);
        if (parsed[i]->base >= 0) printf(" base=%d", parsed[i]->base);
        if (parsed[i]->extra_features[0]) {
            char **f;
            printf(" features={");
            for (f = parsed[i]->extra_features; *f; f++) {
                if (f != parsed[i]->extra_features) printf(",");
                printf("%s", *f);
            }
            printf("}");
        }
        printf("\n");
    }

    // Resolve
    ResolvedTarget **resolved = tp_resolve_targets(
        (ParsedTarget *const *)parsed, NULL, NULL);

    for (i = 0; resolved[i]; i++) {
        printf("  Target %zu: cpu=%s base=%d flags=0x%x\n",
               i, resolved[i]->cpu_name, resolved[i]->base, resolved[i]->flags);
        printf("    features: ");
        print_hw_features(&resolved[i]->features);
        if (resolved[i]->ext_features[0])
            printf("    ext: %s\n", resolved[i]->ext_features);
    }

    tp_resolved_targets_destroy(resolved);
    tp_parsed_targets_destroy(parsed);
}

int main(void) {
    printf("=== Standalone Target Parsing Library Test ===\n");
    printf("No LLVM runtime dependency!\n");
    printf("Database: %u features, %u CPUs\n\n", num_features, num_cpus);

    // Host detection
    printf("Host CPU: %s\n", tp_get_host_cpu_name());
    FeatureBits host_features = tp_get_host_features();
    printf("Host features: ");
    print_hw_features(&host_features);

    // Check a few specific features
    printf("\nHost has AVX2: %s\n", tp_has_feature(&host_features, "avx2") ? "yes" : "no");
    printf("Host has AVX512F: %s\n", tp_has_feature(&host_features, "avx512f") ? "yes" : "no");
    printf("Host has SSE4.2: %s\n", tp_has_feature(&host_features, "sse4.2") ? "yes" : "no");

    // CPU lookups
    printf("\n--- CPU lookup test ---\n");
    {
        const char *test_cpus[] = {"generic", "haswell", "skylake", "znver4", "znver3", "nonexistent"};
        unsigned ci;
        for (ci = 0; ci < sizeof(test_cpus)/sizeof(test_cpus[0]); ci++) {
            const CPUEntry *entry = find_cpu(test_cpus[ci]);
            if (entry) {
                printf("  %s: %u features\n", test_cpus[ci], feature_popcount(&entry->features));
            } else {
                printf("  %s: NOT FOUND\n", test_cpus[ci]);
            }
        }
    }

    // Feature implications
    printf("\n--- Feature implications ---\n");
    {
        const FeatureEntry *avx512f = find_feature("avx512f");
        if (avx512f) {
            unsigned i;
            int first = 1;
            printf("avx512f implies: ");
            for (i = 0; i < num_features; i++) {
                if (feature_test(&avx512f->implies, feature_table[i].bit)) {
                    if (!first) printf(", ");
                    printf("%s", feature_table[i].name);
                    first = 0;
                }
            }
            printf("\n");
        }
    }

    // Target string parsing
    test_parse("native");
    test_parse("haswell");
    test_parse("generic;haswell;skylake-avx512");
    test_parse("haswell,clone_all;skylake,+avx512f,+avx512bw,-sse4a,opt_size");
    test_parse("znver3;znver4,base(0)");

    // High-level API: tp_resolve_targets_for_llvm
    printf("\n--- resolve_targets_for_llvm ---\n");
    {
        FeatureBits host_feats = tp_get_host_features();
        const char *host_cpu = tp_get_host_cpu_name();
        ResolveOptions opts;
        opts.host_features = &host_feats;
        opts.host_cpu = host_cpu;
        opts.strip_nondeterministic = 1;

        LLVMTargetSpec **specs = tp_resolve_targets_for_llvm(
            "generic;haswell;skylake-avx512", &opts);
        size_t nspecs = ptrlen((void *const *)specs);
        printf("  %zu LLVM specs:\n", nspecs);
        {
            size_t i;
            for (i = 0; specs[i]; i++) {
                printf("  [%zu] cpu=%s base=%d\n", i,
                       specs[i]->cpu_name, specs[i]->base);
                printf("       features=%.80s\n", specs[i]->cpu_features);
                if (i > 0) {
                    printf("       diff: math=%d simd=%d fp16=%d bf16=%d\n",
                           specs[i]->diff.has_new_math, specs[i]->diff.has_new_simd,
                           specs[i]->diff.has_new_float16, specs[i]->diff.has_new_bfloat16);
                }
            }
        }

        // Test with specific host to get deterministic results
        {
            const CPUEntry *hsw = find_cpu("haswell");
            if (hsw) {
                ResolveOptions hsw_opts;
                hsw_opts.host_features = &hsw->features;
                hsw_opts.host_cpu = "haswell";
                hsw_opts.strip_nondeterministic = 1;

                LLVMTargetSpec **hsw_specs = tp_resolve_targets_for_llvm(
                    "generic;haswell;skylake-avx512", &hsw_opts);

                printf("\n  With haswell as host:\n");
                {
                    size_t i;
                    for (i = 0; hsw_specs[i]; i++) {
                        printf("  [%zu] cpu=%s vec_size=%d\n", i,
                               hsw_specs[i]->cpu_name,
                               tp_max_vector_size(&hsw_specs[i]->en_features));
                        if (i > 0) {
                            printf("       diff: math=%d simd=%d\n",
                                   hsw_specs[i]->diff.has_new_math,
                                   hsw_specs[i]->diff.has_new_simd);
                        }
                    }
                    printf("  [0] cpu=%s\n", hsw_specs[0]->cpu_name);
                }
                tp_llvm_specs_destroy(hsw_specs);
            }
        }
        tp_llvm_specs_destroy(specs);
    }

    // Feature diff tests
    printf("\n--- Feature diff ---\n");
    {
        const CPUEntry *generic = find_cpu("generic");
        const CPUEntry *hsw = find_cpu("haswell");
        const CPUEntry *skx = find_cpu("skylake-avx512");

        if (generic && hsw && skx) {
            FeatureDiff diff_gh = tp_compute_feature_diff(&generic->features, &hsw->features);
            printf("  generic->haswell: math=%d simd=%d fp16=%d bf16=%d\n",
                   diff_gh.has_new_math, diff_gh.has_new_simd,
                   diff_gh.has_new_float16, diff_gh.has_new_bfloat16);

            FeatureDiff diff_hs = tp_compute_feature_diff(&hsw->features, &skx->features);
            printf("  haswell->skylake-avx512: math=%d simd=%d fp16=%d bf16=%d\n",
                   diff_hs.has_new_math, diff_hs.has_new_simd,
                   diff_hs.has_new_float16, diff_hs.has_new_bfloat16);

            FeatureDiff diff_same = tp_compute_feature_diff(&hsw->features, &hsw->features);
            printf("  haswell->haswell: math=%d simd=%d (should be 0,0)\n",
                   diff_same.has_new_math, diff_same.has_new_simd);
        }

        if (generic && hsw && skx) {
            printf("  vec_size: generic=%d haswell=%d skylake-avx512=%d\n",
                   tp_max_vector_size(&generic->features),
                   tp_max_vector_size(&hsw->features),
                   tp_max_vector_size(&skx->features));
        }
    }

    // Cross-arch queries
    printf("\n--- Cross-arch queries ---\n");
    {
        const char *arches[] = {"x86_64", "aarch64", "riscv64"};
        unsigned ai;
        for (ai = 0; ai < 3; ai++) {
            const char *arch = arches[ai];
            unsigned nf = tp_cross_num_features(arch);
            unsigned nc = tp_cross_num_cpus(arch);
            unsigned nw = tp_cross_feature_words(arch);
            printf("  %s: %u features, %u CPUs, %u words\n", arch, nf, nc, nw);
            check(nf > 50, "should have >50 features");
            check(nc > 5, "should have >5 CPUs");
            check(nw >= 4 && nw <= 5, "should have 4 or 5 words");
        }

        check(tp_cross_num_features("powerpc") == 0, "unknown arch returns 0 features");
        check(tp_cross_feature_words("powerpc") == 0, "unknown arch returns 0 words");
        check(tp_cross_num_features("arm64") == tp_cross_num_features("aarch64"),
              "arm64 should normalize to aarch64");
        check(tp_cross_num_features("i686") == tp_cross_num_features("x86_64"),
              "i686 should normalize to x86_64");

        {
            CrossFeatureBits fb;
            check(tp_cross_lookup_cpu("x86_64", "haswell", &fb), "haswell should be found");
            check(fb.num_words == 4, "x86_64 should have 4 words");
            int haswell_bits = 0;
            unsigned w;
            for (w = 0; w < fb.num_words; w++)
                haswell_bits += __builtin_popcountll(fb.bits[w]);
            printf("  x86_64/haswell: %d hw features\n", haswell_bits);
            check(haswell_bits > 20, "haswell should have >20 hw features");

            check(tp_cross_lookup_cpu("aarch64", "cortex-a78", &fb), "cortex-a78 should be found");
            check(fb.num_words == 5, "aarch64 should have 5 words");
            int a78_bits = 0;
            for (w = 0; w < fb.num_words; w++)
                a78_bits += __builtin_popcountll(fb.bits[w]);
            printf("  aarch64/cortex-a78: %d hw features\n", a78_bits);
            check(a78_bits > 15, "cortex-a78 should have >15 hw features");

            check(tp_cross_lookup_cpu("riscv64", "sifive-u74", &fb), "sifive-u74 should be found");
            check(!tp_cross_lookup_cpu("x86_64", "nonexistent", &fb), "nonexistent should not be found");
            check(!tp_cross_lookup_cpu("badarch", "haswell", &fb), "bad arch should not be found");
        }

        // Apple M-series aliases
        {
            CrossFeatureBits m1_fb, a14_fb;
            check(tp_cross_lookup_cpu("aarch64", "apple-m1", &m1_fb), "apple-m1 alias should resolve");
            check(tp_cross_lookup_cpu("aarch64", "apple-a14", &a14_fb), "apple-a14 should be found");
            int m1_eq_a14 = (m1_fb.num_words == a14_fb.num_words);
            unsigned w;
            for (w = 0; m1_eq_a14 && w < m1_fb.num_words; w++)
                m1_eq_a14 = (m1_fb.bits[w] == a14_fb.bits[w]);
            check(m1_eq_a14, "apple-m1 should equal apple-a14");
        }

        // Architecture version features
        {
            CrossFeatureBits cfb;
            #define HAS_CROSS_FEAT(arch, cpu, feat) ( \
                tp_cross_lookup_cpu(arch, cpu, &cfb) && \
                tp_cross_feature_bit(arch, feat) >= 0 && \
                ((cfb.bits[tp_cross_feature_bit(arch, feat) / 64] >> (tp_cross_feature_bit(arch, feat) % 64)) & 1) \
            )

            check(HAS_CROSS_FEAT("aarch64", "cortex-x925", "v8a"),   "x925 should have v8a");
            check(HAS_CROSS_FEAT("aarch64", "cortex-x925", "v8.1a"), "x925 should have v8.1a");
            check(HAS_CROSS_FEAT("aarch64", "cortex-x925", "v8.2a"), "x925 should have v8.2a");
            check(HAS_CROSS_FEAT("aarch64", "cortex-x925", "v9a"),   "x925 should have v9a");
            check(HAS_CROSS_FEAT("aarch64", "cortex-x925", "v9.2a"), "x925 should have v9.2a");
            check(HAS_CROSS_FEAT("aarch64", "cortex-x925", "sve2"),  "x925 should have sve2");
            check(HAS_CROSS_FEAT("aarch64", "cortex-x925", "dotprod"), "x925 should have dotprod");
            check(HAS_CROSS_FEAT("aarch64", "cortex-x925", "fullfp16"), "x925 should have fullfp16");
            check(HAS_CROSS_FEAT("aarch64", "cortex-x925", "bf16"),  "x925 should have bf16");
            check(HAS_CROSS_FEAT("aarch64", "cortex-a78", "v8.1a"), "a78 should have v8.1a");
            check(HAS_CROSS_FEAT("aarch64", "cortex-a78", "v8.2a"), "a78 should have v8.2a");
            check(!HAS_CROSS_FEAT("aarch64", "cortex-a78", "v9a"),  "a78 should NOT have v9a");
            check(HAS_CROSS_FEAT("aarch64", "cortex-a78", "lse"),   "a78 should have lse");
            check(HAS_CROSS_FEAT("aarch64", "cortex-a78", "rdm"),   "a78 should have rdm");
            check(HAS_CROSS_FEAT("aarch64", "apple-m1", "v8.4a"), "m1 should report v8.4a");
            check(HAS_CROSS_FEAT("aarch64", "apple-m1", "dotprod"), "m1 should have dotprod");
            check(HAS_CROSS_FEAT("aarch64", "apple-m1", "sha3"),   "m1 should have sha3");
            check(HAS_CROSS_FEAT("aarch64", "apple-m1", "fullfp16"), "m1 should have fullfp16");
            check(HAS_CROSS_FEAT("x86_64", "x86-64-v3", "avx2"), "x86-64-v3 should have avx2");
            check(HAS_CROSS_FEAT("x86_64", "x86-64-v3", "fma"),  "x86-64-v3 should have fma");
            check(HAS_CROSS_FEAT("x86_64", "x86-64-v3", "bmi2"), "x86-64-v3 should have bmi2");
            check(!HAS_CROSS_FEAT("x86_64", "x86-64-v3", "avx512f"), "x86-64-v3 should NOT have avx512f");
            check(HAS_CROSS_FEAT("x86_64", "x86-64-v4", "avx512f"),  "x86-64-v4 should have avx512f");
            check(HAS_CROSS_FEAT("x86_64", "x86-64-v4", "avx512vl"), "x86-64-v4 should have avx512vl");
            check(HAS_CROSS_FEAT("riscv64", "sifive-u74", "m"), "u74 should have m (multiply)");
            check(HAS_CROSS_FEAT("riscv64", "sifive-u74", "a"), "u74 should have a (atomic)");
            #undef HAS_CROSS_FEAT
        }

        // Feature name/bit lookups
        {
            int avx2_bit = tp_cross_feature_bit("x86_64", "avx2");
            check(avx2_bit >= 0, "avx2 should have a valid bit");
            check(tp_cross_feature_bit("aarch64", "sve") >= 0, "sve should have a valid bit");
            check(tp_cross_feature_bit("x86_64", "nonexistent_feat") == -1,
                  "unknown feature should return -1");
            check(tp_cross_feature_name("x86_64", 0) != NULL, "feature 0 should have a name");
            check(tp_cross_feature_bit_at("x86_64", 0) >= 0, "feature 0 should have a valid bit");
            check(tp_cross_feature_name("x86_64", 99999) == NULL, "out of range index should return NULL");
            check(tp_cross_feature_bit_at("x86_64", 99999) == -1, "out of range index should return -1");
            check(tp_cross_feature_is_hw("x86_64", "avx2"), "avx2 should be hw");
            check(!tp_cross_feature_is_hw("x86_64", "nonexistent_feat"), "unknown feature should not be hw");
            check(tp_cross_cpu_name("x86_64", 0) != NULL, "cpu 0 should have a name");
            check(tp_cross_cpu_name("x86_64", 99999) == NULL, "out of range cpu index should return NULL");

            unsigned ver = tp_cross_tables_version_major("x86_64");
            printf("  tables version: %u\n", ver);
            check(ver >= 18, "tables version should be >= 18");
            check(tp_cross_tables_version_major("aarch64") == ver, "all arches should have same version");
        }

        // ============================================================
        // resolve_targets_for_llvm with deterministic host
        // ============================================================
        {
            const CPUEntry *hsw_cpu = find_cpu("haswell");
            if (hsw_cpu) {
                ResolveOptions hsw_opts;
                hsw_opts.host_features = &hsw_cpu->features;
                hsw_opts.host_cpu = "haswell";
                hsw_opts.strip_nondeterministic = 1;

                LLVMTargetSpec **specs = tp_resolve_targets_for_llvm(
                    "generic;haswell;skylake-avx512", &hsw_opts);
                size_t nspecs = ptrlen((void *const *)specs);

                check(nspecs == 3, "should produce 3 specs");
                check(strcmp(specs[0]->cpu_name, "x86-64") == 0, "spec[0] should be x86-64 (normalized)");
                check(specs[0]->base == -1, "spec[0] base should be -1");
                check(strcmp(specs[1]->cpu_name, "haswell") == 0, "spec[1] should be haswell");
                check(specs[1]->base == 0, "spec[1] base should be 0");
                check(specs[1]->diff.has_new_math, "haswell should have new math vs generic");
                check(specs[1]->diff.has_new_simd, "haswell should have new simd vs generic");
                check(!specs[1]->diff.has_new_float16, "haswell should NOT have new fp16 vs generic");
                check(strcmp(specs[2]->cpu_name, "skylake-avx512") == 0, "spec[2] should be skylake-avx512");
                check(specs[2]->diff.has_new_simd, "skx should have new simd vs generic");
                check(tp_max_vector_size(&specs[2]->en_features) == 64, "spec[2] vec_size should be 64 (avx512)");
                check(!tp_has_feature(&specs[1]->en_features, "rdrnd"), "rdrnd should be stripped from haswell");
                check(!tp_has_feature(&specs[1]->en_features, "slow-3ops-lea"), "tuning features should not be in en_features");

                {
                    FeatureBits combined;
                    int w;
                    for (w = 0; w < TARGET_FEATURE_WORDS; w++)
                        combined.bits[w] = specs[1]->en_features.bits[w] | specs[1]->dis_features.bits[w];
                    check(feature_equal(&combined, &hw_feature_mask), "en | dis should equal hw_feature_mask");
                }

                // Feature diff standalone tests
                {
                    const CPUEntry *gen = find_cpu("generic");
                    const CPUEntry *skx = find_cpu("skylake-avx512");
                    if (gen && skx) {
                        FeatureDiff d1 = tp_compute_feature_diff(&gen->features, &hsw_cpu->features);
                        check(d1.has_new_math, "generic->haswell should have new math");
                        check(d1.has_new_simd, "generic->haswell should have new simd");
                        FeatureDiff d2 = tp_compute_feature_diff(&hsw_cpu->features, &skx->features);
                        check(d2.has_new_simd, "haswell->skx should have new simd (avx512)");
                        FeatureDiff d3 = tp_compute_feature_diff(&hsw_cpu->features, &hsw_cpu->features);
                        check(!d3.has_new_math, "same->same should have no new math");
                        check(!d3.has_new_simd, "same->same should have no new simd");
                    }
                }

                // max_vector_size tests
                {
                    const CPUEntry *gen = find_cpu("generic");
                    const CPUEntry *skx = find_cpu("skylake-avx512");
                    if (gen && skx) {
                        check(tp_max_vector_size(&gen->features) == 16, "generic should be 16 (SSE)");
                        check(tp_max_vector_size(&hsw_cpu->features) == 32, "haswell should be 32 (AVX)");
                        check(tp_max_vector_size(&skx->features) == 64, "skx should be 64 (AVX-512)");
                    }
                }

                // ============================================================
                // Test actual Julia CI target strings
                // ============================================================
                printf("\n  --- Julia CI target strings ---\n");
                {
                    LLVMTargetSpec **x86_specs = tp_resolve_targets_for_llvm(
                        "generic;sandybridge,-xsaveopt,clone_all;haswell,-rdrnd,base(1);x86-64-v4,-rdrnd,base(1)",
                        &hsw_opts);
                    size_t nx86 = ptrlen((void *const *)x86_specs);
                    check(nx86 == 4, "x86 CI: should produce 4 specs");
                    check(strcmp(x86_specs[0]->cpu_name, "x86-64") == 0, "x86 CI: spec[0] should be x86-64");
                    check(strcmp(x86_specs[1]->cpu_name, "sandybridge") == 0, "x86 CI: spec[1] should be sandybridge");
                    check(strcmp(x86_specs[2]->cpu_name, "haswell") == 0, "x86 CI: spec[2] should be haswell");
                    check(strcmp(x86_specs[3]->cpu_name, "x86-64-v4") == 0, "x86 CI: spec[3] should be x86-64-v4");
                    check(x86_specs[1]->flags & TF_CLONE_ALL, "x86 CI: sandybridge should have clone_all");
                    check(x86_specs[2]->base == 1, "x86 CI: haswell base should be 1");
                    check(x86_specs[3]->base == 1, "x86 CI: x86-64-v4 base should be 1");
                    check(!tp_has_feature(&x86_specs[2]->en_features, "rdrnd"), "x86 CI: haswell should not have rdrnd");
                    check(!tp_has_feature(&x86_specs[3]->en_features, "rdrnd"), "x86 CI: v4 should not have rdrnd");
                    check(!tp_has_feature(&x86_specs[1]->en_features, "xsaveopt"), "x86 CI: sandybridge should not have xsaveopt");
                    check(x86_specs[1]->diff.has_new_simd, "x86 CI: sandybridge should have new simd vs generic");
                    check(x86_specs[2]->diff.has_new_simd, "x86 CI: haswell should have new simd vs sandybridge");
                    check(x86_specs[3]->diff.has_new_simd, "x86 CI: v4 should have new simd vs sandybridge");
                    printf("  x86_64 CI targets: OK (%zu specs)\n", nx86);
                    tp_llvm_specs_destroy(x86_specs);
                }

                // i686: pentium4
                {
                    LLVMTargetSpec **i686_specs = tp_resolve_targets_for_llvm("pentium4", NULL);
                    check(ptrlen((void *const *)i686_specs) == 1, "i686 CI: should produce 1 spec");
                    check(!(i686_specs[0]->flags & TF_UNKNOWN_NAME), "i686 CI: pentium4 should be known");
                    printf("  i686 CI targets: OK\n");
                    tp_llvm_specs_destroy(i686_specs);
                }

                // aarch64 macOS: generic;apple-m1,clone_all
                {
                    CrossFeatureBits m1_cross;
                    if (tp_cross_lookup_cpu("aarch64", "apple-m1", &m1_cross)) {
                        ParsedTarget **parsed = tp_parse_target_string("generic;apple-m1,clone_all");
                        check(ptrlen((void *const *)parsed) == 2, "aarch64 mac CI: should parse 2 targets");
                        check(strcmp(parsed[0]->cpu_name, "generic") == 0, "aarch64 mac CI: first should be generic");
                        check(strcmp(parsed[1]->cpu_name, "apple-m1") == 0, "aarch64 mac CI: second should be apple-m1");
                        check(parsed[1]->flags & TF_CLONE_ALL, "aarch64 mac CI: apple-m1 should have clone_all");
                        printf("  aarch64 macOS CI targets: OK\n");
                        tp_parsed_targets_destroy(parsed);
                    }
                }

                // aarch64 Linux
                {
                    ParsedTarget **parsed = tp_parse_target_string(
                        "generic;cortex-a57;thunderx2t99;carmel,clone_all;apple-m1,base(3);neoverse-512tvb,base(3)");
                    check(ptrlen((void *const *)parsed) == 6, "aarch64 linux CI: should parse 6 targets");
                    check(strcmp(parsed[3]->cpu_name, "carmel") == 0, "aarch64 linux CI: target[3] should be carmel");
                    check(parsed[3]->flags & TF_CLONE_ALL, "aarch64 linux CI: carmel should have clone_all");
                    check(parsed[4]->base == 3, "aarch64 linux CI: apple-m1 base should be 3");
                    check(parsed[5]->base == 3, "aarch64 linux CI: neoverse-512tvb base should be 3");
                    CrossFeatureBits tmpfb;
                    check(tp_cross_lookup_cpu("aarch64", "cortex-a57", &tmpfb), "aarch64 CI: cortex-a57 should be known");
                    check(tp_cross_lookup_cpu("aarch64", "thunderx2t99", &tmpfb), "aarch64 CI: thunderx2t99 should be known");
                    check(tp_cross_lookup_cpu("aarch64", "carmel", &tmpfb), "aarch64 CI: carmel should be known");
                    check(tp_cross_lookup_cpu("aarch64", "apple-m1", &tmpfb), "aarch64 CI: apple-m1 should be known");
                    int has_512tvb = tp_cross_lookup_cpu("aarch64", "neoverse-512tvb", &tmpfb);
                    printf("  aarch64 Linux CI targets: OK (neoverse-512tvb %s)\n",
                           has_512tvb ? "found" : "NOT found - may need LLVM update");
                    tp_parsed_targets_destroy(parsed);
                }

                // ============================================================
                // Popular CPU host simulation
                // ============================================================
                printf("\n  --- Popular CPU host simulation ---\n");
                #define TEST_X86_HOST(host_name, expected_best) do { \
                    const CPUEntry *_host = find_cpu(host_name); \
                    if (!_host) { printf("  %s: NOT IN TABLE (skip)\n", host_name); } else { \
                        ResolveOptions _opts; _opts.host_features = &_host->features; \
                        _opts.host_cpu = host_name; _opts.strip_nondeterministic = 1; \
                        LLVMTargetSpec **_sp = tp_resolve_targets_for_llvm( \
                            "generic;sandybridge,-xsaveopt,clone_all;haswell,-rdrnd,base(1);x86-64-v4,-rdrnd,base(1)", &_opts); \
                        int _best = 0; { int _i; \
                        for (_i = (int)ptrlen((void *const *)_sp) - 1; _i >= 0; _i--) { \
                            FeatureBits _thw, _hhw, _d; \
                            feature_and_out(&_thw, &_sp[_i]->en_features, &hw_feature_mask); \
                            feature_and_out(&_hhw, &_host->features, &hw_feature_mask); \
                            feature_andnot(&_d, &_thw, &_hhw); \
                            if (!feature_any(&_d)) { _best = _i; break; } \
                        }} \
                        printf("  %s -> [%d] %s (expected: %s) %s\n", host_name, _best, \
                               _sp[_best]->cpu_name, expected_best, \
                               strcmp(_sp[_best]->cpu_name, expected_best) == 0 ? "OK" : "MISMATCH"); \
                        check(strcmp(_sp[_best]->cpu_name, expected_best) == 0, \
                              host_name " should match " expected_best); \
                        tp_llvm_specs_destroy(_sp); \
                    } } while(0)

                TEST_X86_HOST("core2", "x86-64");
                TEST_X86_HOST("sandybridge", "sandybridge");
                TEST_X86_HOST("haswell", "haswell");
                TEST_X86_HOST("skylake", "haswell");
                TEST_X86_HOST("skylake-avx512", "x86-64-v4");
                TEST_X86_HOST("znver1", "sandybridge");
                TEST_X86_HOST("znver3", "sandybridge");
                TEST_X86_HOST("znver4", "x86-64-v4");
                TEST_X86_HOST("broadwell", "haswell");
                #undef TEST_X86_HOST

                printf("\n  --- psABI level targets (recommended) ---\n");
                #define TEST_X86_PSABI(host_name, expected_best) do { \
                    const CPUEntry *_host = find_cpu(host_name); \
                    if (!_host) { printf("  %s: NOT IN TABLE (skip)\n", host_name); } else { \
                        ResolveOptions _opts; _opts.host_features = &_host->features; \
                        _opts.host_cpu = host_name; _opts.strip_nondeterministic = 1; \
                        LLVMTargetSpec **_sp = tp_resolve_targets_for_llvm( \
                            "generic;x86-64-v2,clone_all;x86-64-v3,-rdrnd,base(1);x86-64-v4,-rdrnd,base(1)", &_opts); \
                        int _best = 0; { int _i; \
                        for (_i = (int)ptrlen((void *const *)_sp) - 1; _i >= 0; _i--) { \
                            FeatureBits _thw, _hhw, _d; \
                            feature_and_out(&_thw, &_sp[_i]->en_features, &hw_feature_mask); \
                            feature_and_out(&_hhw, &_host->features, &hw_feature_mask); \
                            feature_andnot(&_d, &_thw, &_hhw); \
                            if (!feature_any(&_d)) { _best = _i; break; } \
                        }} \
                        printf("  %s -> [%d] %s (expected: %s) %s\n", host_name, _best, \
                               _sp[_best]->cpu_name, expected_best, \
                               strcmp(_sp[_best]->cpu_name, expected_best) == 0 ? "OK" : "MISMATCH"); \
                        check(strcmp(_sp[_best]->cpu_name, expected_best) == 0, \
                              host_name " psABI should match " expected_best); \
                        tp_llvm_specs_destroy(_sp); \
                    } } while(0)

                TEST_X86_PSABI("core2", "x86-64");
                TEST_X86_PSABI("sandybridge", "x86-64-v2");
                TEST_X86_PSABI("haswell", "x86-64-v3");
                TEST_X86_PSABI("skylake", "x86-64-v3");
                TEST_X86_PSABI("skylake-avx512", "x86-64-v4");
                TEST_X86_PSABI("znver1", "x86-64-v3");
                TEST_X86_PSABI("znver3", "x86-64-v3");
                TEST_X86_PSABI("znver4", "x86-64-v4");
                TEST_X86_PSABI("broadwell", "x86-64-v3");
                #undef TEST_X86_PSABI

                // ============================================================
                // Serialization round-trip
                // ============================================================
                printf("\n  --- Serialization round-trip ---\n");
                {
                    LLVMTargetSpec **sp = tp_resolve_targets_for_llvm(
                        "generic;x86-64-v2,clone_all;x86-64-v3,-rdrnd,base(1);x86-64-v4,-rdrnd,base(1)",
                        &hsw_opts);
                    size_t ns = ptrlen((void *const *)sp);
                    tp_byte_buf blob = tp_serialize_targets((LLVMTargetSpec *const *)sp);
                    check(blob.len > 0, "serialized data should be non-empty");
                    LLVMTargetSpec **restored = tp_deserialize_targets(blob.data);
                    size_t nr = ptrlen((void *const *)restored);
                    check(nr == ns, "round-trip: same count");
                    {
                        size_t ri;
                        for (ri = 0; ri < ns && ri < nr; ri++) {
                            check(strcmp(restored[ri]->cpu_name, sp[ri]->cpu_name) == 0, "round-trip name mismatch");
                            check(restored[ri]->flags == sp[ri]->flags, "round-trip flags mismatch");
                            check(restored[ri]->base == sp[ri]->base, "round-trip base mismatch");
                            check(feature_equal(&restored[ri]->en_features, &sp[ri]->en_features), "round-trip en_features mismatch");
                            check(feature_equal(&restored[ri]->dis_features, &sp[ri]->dis_features), "round-trip dis_features mismatch");
                        }
                    }
                    printf("  serialization round-trip: OK (%zu bytes, %zu targets)\n", blob.len, ns);
                    free(blob.data);
                    tp_llvm_specs_destroy(restored);
                    tp_llvm_specs_destroy(sp);
                }

                // ============================================================
                // Target matching via library API
                // ============================================================
                printf("\n  --- Target matching ---\n");
                {
                    #define TEST_MATCH(host_name, target_str_val, expected_best) do { \
                        const CPUEntry *_host = find_cpu(host_name); \
                        if (!_host) { printf("  %s: NOT IN TABLE (skip)\n", host_name); } else { \
                            LLVMTargetSpec **_sysimg = tp_resolve_targets_for_llvm(target_str_val, NULL); \
                            ResolveOptions _hopts; _hopts.host_features = &_host->features; \
                            _hopts.host_cpu = host_name; _hopts.strip_nondeterministic = 1; \
                            LLVMTargetSpec **_hspecs = tp_resolve_targets_for_llvm("native", &_hopts); \
                            check(_hspecs[0] != NULL, "host should produce at least 1 spec"); \
                            TargetMatch _m = tp_match_targets((LLVMTargetSpec *const *)_sysimg, _hspecs[0]); \
                            const char *_matched = _m.best_idx >= 0 ? _sysimg[_m.best_idx]->cpu_name : "NONE"; \
                            printf("  %s -> [%d] %s (expected: %s) %s\n", host_name, _m.best_idx, _matched, expected_best, \
                                   strcmp(_matched, expected_best) == 0 ? "OK" : "MISMATCH"); \
                            check(strcmp(_matched, expected_best) == 0, host_name " match should be " expected_best); \
                            tp_llvm_specs_destroy(_sysimg); tp_llvm_specs_destroy(_hspecs); \
                        } } while(0)

                    TEST_MATCH("core2", "generic;x86-64-v2,clone_all;x86-64-v3,base(1);x86-64-v4,base(1)", "x86-64");
                    TEST_MATCH("haswell", "generic;x86-64-v2,clone_all;x86-64-v3,base(1);x86-64-v4,base(1)", "x86-64-v3");
                    TEST_MATCH("znver1", "generic;x86-64-v2,clone_all;x86-64-v3,base(1);x86-64-v4,base(1)", "x86-64-v3");
                    TEST_MATCH("znver4", "generic;x86-64-v2,clone_all;x86-64-v3,base(1);x86-64-v4,base(1)", "x86-64-v4");
                    TEST_MATCH("skylake-avx512", "generic;x86-64-v2,clone_all;x86-64-v3,base(1);x86-64-v4,base(1)", "x86-64-v4");
                    #undef TEST_MATCH
                }

                tp_llvm_specs_destroy(specs);
            }
        }
    } // end cross-arch tests

    if (failures > 0) {
        printf("\nFAILED: %d test(s) failed.\n", failures);
        return 1;
    }
    printf("\nDone. All tests passed.\n");
    return 0;
}
