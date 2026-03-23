// CPU name alias resolution.
// Maps alternate CPU names to canonical names in the generated tables.
// From LLVM's ProcessorAlias definitions.

#ifndef CPU_ALIASES_H
#define CPU_ALIASES_H

#include <string.h>

// Pure name mapping — no table lookups.
// Callers should check if the result exists in their table.
static inline const char *resolve_cpu_alias(const char *name) {
    static const struct { const char *from; const char *to; } aliases[] = {
        {"apple-m1", "apple-a14"},
        {"apple-m2", "apple-a15"},
        {"apple-m3", "apple-a16"},
        {"apple-a18", "apple-m4"},
        {"apple-a19", "apple-m5"},
    };
    unsigned i;
    for (i = 0; i < sizeof(aliases)/sizeof(aliases[0]); i++)
        if (strcmp(name, aliases[i].from) == 0) return aliases[i].to;
    return name;
}

#endif // CPU_ALIASES_H
