#include "sensitive.h"

#include <ctype.h>
#include <string.h>

typedef enum {
    SENSITIVE_MATCH_HARD = 0,
    SENSITIVE_MATCH_SOFT
} SensitiveMatchSeverity;

typedef enum {
    TOKEN_CONTEXT_NONE = 0,
    TOKEN_CONTEXT_ASSIGNMENT_OR_BEARER
} SensitiveTokenContext;

typedef struct {
    const char* begin_marker;
    const char* end_marker;
    SensitiveMatchSeverity severity;
} SensitiveLiteralRule;

typedef struct {
    const char* prefix;
    size_t min_run_len;
    size_t max_run_len;
    int (*char_ok)(unsigned char);
    int require_boundaries;
    SensitiveTokenContext context_requirement;
    SensitiveMatchSeverity severity;
} SensitivePrefixRule;

static int equals_ignore_case_char(unsigned char lhs, unsigned char rhs) {
    return tolower(lhs) == tolower(rhs);
}

static int starts_with_ignore_case(const char* text, const char* prefix) {
    if (!text || !prefix) return 0;
    while (*prefix != '\0') {
        if (*text == '\0' || !equals_ignore_case_char((unsigned char)*text, (unsigned char)*prefix)) {
            return 0;
        }
        text++;
        prefix++;
    }
    return 1;
}

static int equals_ignore_case(const char* lhs, const char* rhs) {
    if (!lhs || !rhs) return 0;
    while (*lhs != '\0' && *rhs != '\0') {
        if (!equals_ignore_case_char((unsigned char)*lhs, (unsigned char)*rhs)) {
            return 0;
        }
        lhs++;
        rhs++;
    }
    return *lhs == '\0' && *rhs == '\0';
}

static int ends_with_ignore_case(const char* text, const char* suffix) {
    size_t text_len;
    size_t suffix_len;

    if (!text || !suffix) return 0;
    text_len = strlen(text);
    suffix_len = strlen(suffix);
    if (suffix_len > text_len) return 0;
    return equals_ignore_case(text + text_len - suffix_len, suffix);
}

static int matches_sensitive_basename_pattern(const char* basename, const char* stem) {
    size_t stem_len;

    if (!basename || !stem) {
        return 0;
    }
    stem_len = strlen(stem);
    if (!starts_with_ignore_case(basename, stem)) {
        return 0;
    }
    return basename[stem_len] == '\0' || basename[stem_len] == '.';
}

int fuori_is_sensitive_filename(const char* filepath) {
    const char* basename = filepath;
    const char* slash = strrchr(filepath, '/');

    if (!filepath) {
        return 0;
    }
    if (slash) {
        basename = slash + 1;
    }

    if (matches_sensitive_basename_pattern(basename, ".env") ||
        matches_sensitive_basename_pattern(basename, "credential") ||
        matches_sensitive_basename_pattern(basename, "credentials") ||
        matches_sensitive_basename_pattern(basename, "secret") ||
        matches_sensitive_basename_pattern(basename, "secrets") ||
        equals_ignore_case(basename, "id_rsa") ||
        equals_ignore_case(basename, "id_dsa") ||
        equals_ignore_case(basename, "id_ecdsa") ||
        equals_ignore_case(basename, "id_ed25519") ||
        ends_with_ignore_case(basename, ".pem") ||
        ends_with_ignore_case(basename, ".key")) {
        return 1;
    }

    return 0;
}

static int buffer_contains_literal_pair(const unsigned char* buffer,
                                        size_t bytes_read,
                                        const char* begin_marker,
                                        const char* end_marker) {
    size_t begin_len;
    size_t end_len;

    if (!buffer || !begin_marker || !end_marker) {
        return 0;
    }

    begin_len = strlen(begin_marker);
    end_len = strlen(end_marker);
    if (begin_len == 0 || end_len == 0 ||
        begin_len > bytes_read || end_len > bytes_read) {
        return 0;
    }

    for (size_t i = 0; i + begin_len <= bytes_read; i++) {
        if (memcmp(buffer + i, begin_marker, begin_len) != 0) {
            continue;
        }
        for (size_t j = i + begin_len; j + end_len <= bytes_read; j++) {
            if (memcmp(buffer + j, end_marker, end_len) == 0) {
                return 1;
            }
        }
    }

    return 0;
}

static int is_upper_alnum(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static int is_openai_key_char(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' ||
           c == '_';
}

static int has_run_boundary_before(const unsigned char* buffer,
                                   size_t match_start,
                                   int (*char_ok)(unsigned char)) {
    if (match_start == 0) {
        return 1;
    }
    return !char_ok(buffer[match_start - 1]);
}

static int has_run_boundary_after(const unsigned char* buffer,
                                  size_t bytes_read,
                                  size_t run_end,
                                  int (*char_ok)(unsigned char)) {
    if (run_end >= bytes_read) {
        return 1;
    }
    return !char_ok(buffer[run_end]);
}

static int has_assignment_or_bearer_context(const unsigned char* buffer,
                                            size_t bytes_read,
                                            size_t match_start) {
    size_t scan_start;

    (void)bytes_read;

    if (match_start >= 7 &&
        memcmp(buffer + match_start - 7, "Bearer ", 7) == 0) {
        return 1;
    }

    scan_start = (match_start > 32) ? (match_start - 32) : 0;
    for (size_t i = match_start; i > scan_start; i--) {
        unsigned char c = buffer[i - 1];
        if (c == '=' || c == ':') {
            return 1;
        }
        if (c == '\n' || c == '\r') {
            break;
        }
    }

    return 0;
}

static int buffer_contains_rule_match(const unsigned char* buffer,
                                      size_t bytes_read,
                                      const SensitivePrefixRule* rule) {
    size_t prefix_len;

    if (!buffer || !rule || !rule->prefix || !rule->char_ok) {
        return 0;
    }
    prefix_len = strlen(rule->prefix);
    if (prefix_len == 0 || prefix_len + rule->min_run_len > bytes_read) {
        return 0;
    }

    for (size_t i = 0; i + prefix_len + rule->min_run_len <= bytes_read; i++) {
        size_t run_len = 0;

        if (memcmp(buffer + i, rule->prefix, prefix_len) != 0) {
            continue;
        }
        if (rule->require_boundaries &&
            !has_run_boundary_before(buffer, i, rule->char_ok)) {
            continue;
        }

        while (i + prefix_len + run_len < bytes_read &&
               rule->char_ok(buffer[i + prefix_len + run_len]) &&
               run_len < rule->max_run_len) {
            run_len++;
        }

        if (run_len < rule->min_run_len) {
            continue;
        }
        if (rule->require_boundaries &&
            !has_run_boundary_after(buffer,
                                    bytes_read,
                                    i + prefix_len + run_len,
                                    rule->char_ok)) {
            continue;
        }
        if (rule->context_requirement == TOKEN_CONTEXT_ASSIGNMENT_OR_BEARER &&
            !has_assignment_or_bearer_context(buffer, bytes_read, i)) {
            continue;
        }

        if (rule->severity == SENSITIVE_MATCH_HARD ||
            rule->severity == SENSITIVE_MATCH_SOFT) {
            return 1;
        }
    }

    return 0;
}

int fuori_contains_sensitive_content(const unsigned char* buffer, size_t bytes_read) {
    static const SensitiveLiteralRule private_key_rules[] = {
        {"-----BEGIN PRIVATE KEY-----", "-----END PRIVATE KEY-----", SENSITIVE_MATCH_HARD},
        {"-----BEGIN RSA PRIVATE KEY-----", "-----END RSA PRIVATE KEY-----", SENSITIVE_MATCH_HARD},
        {"-----BEGIN OPENSSH PRIVATE KEY-----", "-----END OPENSSH PRIVATE KEY-----", SENSITIVE_MATCH_HARD},
        {"-----BEGIN EC PRIVATE KEY-----", "-----END EC PRIVATE KEY-----", SENSITIVE_MATCH_HARD},
        {"-----BEGIN DSA PRIVATE KEY-----", "-----END DSA PRIVATE KEY-----", SENSITIVE_MATCH_HARD},
        {"-----BEGIN PGP PRIVATE KEY BLOCK-----", "-----END PGP PRIVATE KEY BLOCK-----", SENSITIVE_MATCH_HARD},
        {NULL, NULL, SENSITIVE_MATCH_HARD}
    };
    static const SensitivePrefixRule prefix_rules[] = {
        {"AKIA", 16, 16, is_upper_alnum, 1, TOKEN_CONTEXT_NONE, SENSITIVE_MATCH_HARD},
        {"ASIA", 16, 16, is_upper_alnum, 1, TOKEN_CONTEXT_NONE, SENSITIVE_MATCH_HARD},
        {"ghp_", 30, 255, is_openai_key_char, 1, TOKEN_CONTEXT_NONE, SENSITIVE_MATCH_SOFT},
        {"gho_", 30, 255, is_openai_key_char, 1, TOKEN_CONTEXT_NONE, SENSITIVE_MATCH_SOFT},
        {"ghu_", 30, 255, is_openai_key_char, 1, TOKEN_CONTEXT_NONE, SENSITIVE_MATCH_SOFT},
        {"ghs_", 30, 255, is_openai_key_char, 1, TOKEN_CONTEXT_NONE, SENSITIVE_MATCH_SOFT},
        {"ghr_", 30, 255, is_openai_key_char, 1, TOKEN_CONTEXT_NONE, SENSITIVE_MATCH_SOFT},
        {"sk-", 40, 255, is_openai_key_char, 1, TOKEN_CONTEXT_NONE, SENSITIVE_MATCH_SOFT},
        {NULL, 0, 0, NULL, 0, TOKEN_CONTEXT_NONE, SENSITIVE_MATCH_SOFT}
    };

    if (!buffer) {
        return 0;
    }

    for (size_t i = 0; private_key_rules[i].begin_marker != NULL; i++) {
        if (buffer_contains_literal_pair(buffer,
                                         bytes_read,
                                         private_key_rules[i].begin_marker,
                                         private_key_rules[i].end_marker) &&
            (private_key_rules[i].severity == SENSITIVE_MATCH_HARD ||
             private_key_rules[i].severity == SENSITIVE_MATCH_SOFT)) {
            return 1;
        }
    }
    for (size_t i = 0; prefix_rules[i].prefix != NULL; i++) {
        if (buffer_contains_rule_match(buffer, bytes_read, &prefix_rules[i])) {
            return 1;
        }
    }

    return 0;
}
