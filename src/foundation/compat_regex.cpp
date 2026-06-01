/*
 * compat_regex.cpp — Portable regular expression implementation.
 *
 * Backed by std::regex on all platforms (the prior implementation used the
 * system <regex.h> on POSIX and the vendored TRE library on Windows). Patterns
 * are POSIX Extended Regular Expressions; std::regex::extended implements that
 * grammar, including POSIX character classes like [[:space:]].
 *
 * The opaque cbm_regex_t buffer stores a heap-allocated std::regex* (copied in
 * via memcpy for alignment-safety); cbm_regfree deletes it. The C API in
 * compat_regex.h is unchanged, so callers are untouched.
 */
#include "foundation/compat_regex.h"
#include "foundation/constants.h"

#include <cstring>
#include <regex>

namespace {

std::regex::flag_type translate_flags(int flags) {
    /* POSIX grammar: EXTENDED -> ERE, otherwise BRE. Callers use ERE. */
    std::regex::flag_type f = (flags & CBM_REG_EXTENDED) ? std::regex::extended : std::regex::basic;
    if (flags & CBM_REG_ICASE) {
        f |= std::regex::icase;
    }
    if (flags & CBM_REG_NOSUB) {
        f |= std::regex::nosubs;
    }
    if (flags & CBM_REG_NEWLINE) {
        f |= std::regex::multiline;
    }
    return f;
}

} // namespace

static_assert(sizeof(std::regex *) <= CBM_SZ_256,
              "cbm_regex_t opaque buffer too small for pointer");

int cbm_regcomp(cbm_regex_t *r, const char *pattern, int flags) {
    try {
        std::regex *re = new std::regex(pattern, translate_flags(flags));
        std::memcpy(r->opaque, &re, sizeof(re));
        return CBM_REG_OK;
    } catch (...) {
        /* Invalid pattern (std::regex_error) or OOM: non-zero = compile error,
         * matching the prior regcomp contract (callers only check != 0). */
        return 1;
    }
}

int cbm_regexec(const cbm_regex_t *r, const char *str, int nmatch, cbm_regmatch_t *matches,
                int eflags) {
    (void)eflags; /* All call sites pass 0; POSIX exec flags are unused. */
    std::regex *re = nullptr;
    std::memcpy(&re, r->opaque, sizeof(re));
    if (!re) {
        return CBM_REG_NOMATCH;
    }
    try {
        if (nmatch <= 0 || !matches) {
            return std::regex_search(str, *re) ? CBM_REG_OK : CBM_REG_NOMATCH;
        }
        std::cmatch m;
        if (!std::regex_search(str, m, *re)) {
            return CBM_REG_NOMATCH;
        }
        int n = nmatch > CBM_SZ_32 ? CBM_SZ_32 : nmatch;
        for (int i = 0; i < n; i++) {
            if (static_cast<size_t>(i) < m.size() && m[i].matched) {
                matches[i].rm_so = static_cast<int>(m.position(static_cast<size_t>(i)));
                matches[i].rm_eo = static_cast<int>(m.position(static_cast<size_t>(i)) +
                                                    m.length(static_cast<size_t>(i)));
            } else {
                matches[i].rm_so = -1;
                matches[i].rm_eo = -1;
            }
        }
        return CBM_REG_OK;
    } catch (...) {
        /* Pathological backtracking / runtime error -> treat as no match. */
        return CBM_REG_NOMATCH;
    }
}

void cbm_regfree(cbm_regex_t *r) {
    std::regex *re = nullptr;
    std::memcpy(&re, r->opaque, sizeof(re));
    delete re;
    std::memset(r->opaque, 0,
                sizeof(std::regex *)); /* clear stale ptr (cppcheck: avoid re after delete) */
}
