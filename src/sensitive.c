#include "sensitive.h"

#include <ctype.h>
#include <string.h>

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

static int is_sensitive_basename_prefix(const char* basename, const char* prefix) {
    if (!basename || !prefix) return 0;
    return starts_with_ignore_case(basename, prefix);
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

    if (is_sensitive_basename_prefix(basename, ".env") ||
        is_sensitive_basename_prefix(basename, "credential") ||
        is_sensitive_basename_prefix(basename, "credentials") ||
        is_sensitive_basename_prefix(basename, "secret") ||
        is_sensitive_basename_prefix(basename, "secrets") ||
        is_sensitive_basename_prefix(basename, "id_rsa") ||
        is_sensitive_basename_prefix(basename, "id_dsa") ||
        is_sensitive_basename_prefix(basename, "id_ecdsa") ||
        is_sensitive_basename_prefix(basename, "id_ed25519") ||
        ends_with_ignore_case(basename, ".pem") ||
        ends_with_ignore_case(basename, ".key")) {
        return 1;
    }

    return 0;
}

static int buffer_contains_literal(const unsigned char* buffer,
                                   size_t bytes_read,
                                   const char* needle) {
    size_t needle_len;

    if (!buffer || !needle) {
        return 0;
    }
    needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > bytes_read) {
        return 0;
    }

    for (size_t i = 0; i + needle_len <= bytes_read; i++) {
        if (memcmp(buffer + i, needle, needle_len) == 0) {
            return 1;
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

static int buffer_contains_prefixed_run(const unsigned char* buffer,
                                        size_t bytes_read,
                                        const char* prefix,
                                        size_t required_run_len,
                                        int (*char_ok)(unsigned char)) {
    size_t prefix_len;

    if (!buffer || !prefix || !char_ok) {
        return 0;
    }
    prefix_len = strlen(prefix);
    if (prefix_len == 0 || prefix_len + required_run_len > bytes_read) {
        return 0;
    }

    for (size_t i = 0; i + prefix_len + required_run_len <= bytes_read; i++) {
        size_t run_len = 0;

        if (memcmp(buffer + i, prefix, prefix_len) != 0) {
            continue;
        }
        while (i + prefix_len + run_len < bytes_read &&
               char_ok(buffer[i + prefix_len + run_len])) {
            run_len++;
        }
        if (run_len >= required_run_len) {
            return 1;
        }
    }

    return 0;
}

int fuori_contains_sensitive_content(const unsigned char* buffer, size_t bytes_read) {
    static const char* private_key_markers[] = {
        "-----BEGIN PRIVATE KEY-----",
        "-----BEGIN RSA PRIVATE KEY-----",
        "-----BEGIN OPENSSH PRIVATE KEY-----",
        "-----BEGIN EC PRIVATE KEY-----",
        "-----BEGIN DSA PRIVATE KEY-----",
        "-----BEGIN PGP PRIVATE KEY BLOCK-----",
        NULL
    };
    static const char* github_prefixes[] = {
        "ghp_",
        "gho_",
        "ghu_",
        "ghs_",
        "ghr_",
        NULL
    };

    if (!buffer) {
        return 0;
    }

    for (size_t i = 0; private_key_markers[i] != NULL; i++) {
        if (buffer_contains_literal(buffer, bytes_read, private_key_markers[i])) {
            return 1;
        }
    }
    if (buffer_contains_prefixed_run(buffer, bytes_read, "AKIA", 16, is_upper_alnum) ||
        buffer_contains_prefixed_run(buffer, bytes_read, "ASIA", 16, is_upper_alnum)) {
        return 1;
    }
    for (size_t i = 0; github_prefixes[i] != NULL; i++) {
        if (buffer_contains_prefixed_run(buffer, bytes_read, github_prefixes[i], 20, is_openai_key_char)) {
            return 1;
        }
    }
    if (buffer_contains_prefixed_run(buffer, bytes_read, "sk-", 20, is_openai_key_char)) {
        return 1;
    }

    return 0;
}
