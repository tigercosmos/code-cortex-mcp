/*
 * compat_regex.cpp — Portable regular expression implementation.
 *
 * Backed by the system POSIX regex engine where available and std::regex on
 * Windows.  libstdc++'s std::regex executor recursively backtracks once per
 * input byte for patterns such as `.*`; sufficiently long indexed qualified
 * names can therefore overflow the process stack.  POSIX regexec is iterative
 * for this workload.  The Windows fallback rejects oversized non-trivial
 * inputs instead of risking a process crash.
 *
 * The opaque cbm_regex_t buffer stores regex_t directly on POSIX and a
 * heap-allocated std::regex state pointer on Windows. The C API is unchanged.
 */
#include "foundation/compat_regex.h"
#include "foundation/constants.h"

#include <cstring>
#ifdef _WIN32
#include <regex>
#include <string>
#else
#include <regex.h>
#endif

namespace {

#ifdef _WIN32
struct RegexState {
    std::string pattern;
    std::regex compiled;

    RegexState(const char *source, std::regex::flag_type flags)
        : pattern(source), compiled(source, flags) {}
};

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
#else
int translate_flags(int flags) {
    int f = 0;
    if (flags & CBM_REG_EXTENDED) {
        f |= REG_EXTENDED;
    }
    if (flags & CBM_REG_ICASE) {
        f |= REG_ICASE;
    }
    if (flags & CBM_REG_NOSUB) {
        f |= REG_NOSUB;
    }
    if (flags & CBM_REG_NEWLINE) {
        f |= REG_NEWLINE;
    }
    return f;
}
#endif

} // namespace

#ifdef _WIN32
static_assert(sizeof(RegexState *) <= CBM_SZ_256,
              "cbm_regex_t opaque buffer too small for pointer");
#else
static_assert(sizeof(regex_t) <= CBM_SZ_256,
              "cbm_regex_t opaque buffer too small for regex_t");
#endif

int cbm_regcomp(cbm_regex_t *r, const char *pattern, int flags) {
#ifdef _WIN32
    try {
        RegexState *re = new RegexState(pattern, translate_flags(flags));
        std::memcpy(r->opaque, &re, sizeof(re));
        return CBM_REG_OK;
    } catch (...) {
        /* Invalid pattern (std::regex_error) or OOM: non-zero = compile error,
         * matching the prior regcomp contract (callers only check != 0). */
        return 1;
    }
#else
    regex_t *re = reinterpret_cast<regex_t *>(r->opaque);
    int rc = regcomp(re, pattern, translate_flags(flags));
    return rc == 0 ? CBM_REG_OK : rc;
#endif
}

int cbm_regexec(const cbm_regex_t *r, const char *str, int nmatch, cbm_regmatch_t *matches,
                int eflags) {
#ifdef _WIN32
    (void)eflags; /* All call sites pass 0; POSIX exec flags are unused. */
    RegexState *re = nullptr;
    std::memcpy(&re, r->opaque, sizeof(re));
    if (!re) {
        return CBM_REG_NOMATCH;
    }
    if (re->pattern == ".*") {
        if (nmatch > 0 && matches) {
            matches[0].rm_so = 0;
            matches[0].rm_eo = static_cast<int>(std::strlen(str));
            for (int i = 1; i < nmatch && i < CBM_SZ_32; ++i) {
                matches[i].rm_so = -1;
                matches[i].rm_eo = -1;
            }
        }
        return CBM_REG_OK;
    }
    constexpr size_t kMaxSafeStdRegexInput = 64U * 1024U;
    if (std::strlen(str) > kMaxSafeStdRegexInput) {
        return CBM_REG_NOMATCH;
    }
    try {
        if (nmatch <= 0 || !matches) {
            return std::regex_search(str, re->compiled) ? CBM_REG_OK : CBM_REG_NOMATCH;
        }
        std::cmatch m;
        if (!std::regex_search(str, m, re->compiled)) {
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
#else
    const regex_t *re = reinterpret_cast<const regex_t *>(r->opaque);
    if (nmatch <= 0 || !matches) {
        int rc = regexec(re, str, 0, nullptr, eflags);
        return rc == 0 ? CBM_REG_OK : CBM_REG_NOMATCH;
    }
    regmatch_t native_matches[CBM_SZ_32];
    int n = nmatch > CBM_SZ_32 ? CBM_SZ_32 : nmatch;
    int rc = regexec(re, str, static_cast<size_t>(n), native_matches, eflags);
    if (rc != 0) {
        return CBM_REG_NOMATCH;
    }
    for (int i = 0; i < n; ++i) {
        matches[i].rm_so = static_cast<int>(native_matches[i].rm_so);
        matches[i].rm_eo = static_cast<int>(native_matches[i].rm_eo);
    }
    return CBM_REG_OK;
#endif
}

void cbm_regfree(cbm_regex_t *r) {
#ifdef _WIN32
    RegexState *re = nullptr;
    std::memcpy(&re, r->opaque, sizeof(re));
    delete re;
    std::memset(r->opaque, 0,
                sizeof(RegexState *)); /* clear stale ptr (cppcheck: avoid re after delete) */
#else
    regex_t *re = reinterpret_cast<regex_t *>(r->opaque);
    regfree(re);
#endif
}
