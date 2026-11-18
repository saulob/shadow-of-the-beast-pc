// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"

#define HTTP_ERROR_OUT_OF_MEMORY ((int32_t)0x80431022)
#define HTTP_ERROR_INVALID_VALUE ((int32_t)0x804311fe)
#define HTTP_ERROR_INVALID_URL ((int32_t)0x80433060)

enum {
    BUILD_WITH_SCHEME = 0x01,
    BUILD_WITH_HOSTNAME = 0x02,
    BUILD_WITH_PORT = 0x04,
    BUILD_WITH_PATH = 0x08,
    BUILD_WITH_USERNAME = 0x10,
    BUILD_WITH_PASSWORD = 0x20,
    BUILD_WITH_QUERY = 0x40,
    BUILD_WITH_FRAGMENT = 0x80,
};

typedef struct {
    _Bool opaque;
    char *scheme, *username, *password, *hostname, *path, *query, *fragment;
    uint16_t port;
    uint8_t reserved[10];
} UriElement;

_Static_assert(sizeof(UriElement) == 80 && offsetof(UriElement, port) == 64, "OrbisHttpUriElement layout");

typedef struct {
    char *data;
    size_t size, capacity;
} Text;

static int text_append(Text *t, const char *s, size_t n) {
    if (t->size + n + 1 > t->capacity) {
        size_t capacity = t->capacity ? t->capacity : 256;
        while (capacity < t->size + n + 1) capacity *= 2;
        char *next = realloc(t->data, capacity);
        if (!next) return -1;
        t->data = next;
        t->capacity = capacity;
    }
    memcpy(t->data + t->size, s, n);
    t->size += n;
    t->data[t->size] = '\0';
    return 0;
}

static int text_append_string(Text *t, const char *s) { return text_append(t, s, strlen(s)); }

static int prefix_equals_ignore_case(const char *text, size_t text_size, const char *target) {
    const size_t n = strlen(target);
    if (text_size < n) return 0;
    for (size_t i = 0; i < n; ++i) {
        if (tolower((unsigned char)text[i]) != tolower((unsigned char)target[i])) return 0;
    }
    return 1;
}

static uint16_t scheme_default_port(const char *scheme, size_t size) {
    if (size > 0x20) return 0;
    if (prefix_equals_ignore_case(scheme, size, "HTTPS")) return 443;
    if (prefix_equals_ignore_case(scheme, size, "HTTP")) return 80;
    if (prefix_equals_ignore_case(scheme, size, "TTP")) return 80;
    return 0;
}

static ABI int32_t uri_build(char *out, uint64_t *require, uint64_t prepare, const UriElement *source,
                             uint32_t option) {
    if (!source) return HTTP_ERROR_INVALID_URL;
    if (!out && !require) return HTTP_ERROR_INVALID_VALUE;
    const char *scheme = source->scheme ? source->scheme : "", *username = source->username ? source->username : "";
    const char *password = source->password ? source->password : "", *hostname = source->hostname ? source->hostname : "";
    const char *path = source->path ? source->path : "", *query = source->query ? source->query : "";
    const char *fragment = source->fragment ? source->fragment : "";
    const size_t scheme_size = strlen(scheme);
    const int mailto = scheme_size == 6 && !memcmp(scheme, "mailto", 6);

    Text built = {0};
    int failed = 0;
    if ((option & BUILD_WITH_SCHEME) && scheme_size) failed |= text_append_string(&built, scheme) | text_append(&built, ":", 1);
    if (!source->opaque) failed |= text_append(&built, "//", 2);
    const int has_user = (option & BUILD_WITH_USERNAME) && *username, has_pass = (option & BUILD_WITH_PASSWORD) && *password;
    if (has_user) failed |= text_append_string(&built, username);
    if (has_pass) failed |= text_append(&built, ":", 1) | text_append_string(&built, password);
    if (has_user || has_pass) failed |= text_append(&built, "@", 1);
    if ((option & BUILD_WITH_HOSTNAME) && *hostname) failed |= text_append_string(&built, hostname);
    if ((option & BUILD_WITH_PORT) && source->port) {
        const uint16_t standard = scheme_default_port(scheme, scheme_size);
        if (!((standard || mailto) && standard == source->port)) {
            char digits[8];
            snprintf(digits, sizeof(digits), ":%u", (unsigned)source->port);
            failed |= text_append_string(&built, digits);
        }
    }
    if ((option & BUILD_WITH_PATH) && *path) failed |= text_append_string(&built, path);
    if ((option & BUILD_WITH_QUERY) && *query) failed |= text_append_string(&built, query);
    if ((option & BUILD_WITH_FRAGMENT) && *fragment) failed |= text_append_string(&built, fragment);
    if (failed) {
        free(built.data);
        return HTTP_ERROR_OUT_OF_MEMORY;
    }
    const uint64_t needed = built.size + 1;
    if (require) *require = needed;
    int32_t result = 0;
    if (out) {
        if (prepare < needed) result = HTTP_ERROR_OUT_OF_MEMORY;
        else memcpy(out, built.data ? built.data : "", needed);
    }
    free(built.data);
    return result;
}

static int unreserved(unsigned char c) { return isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~'; }

static ABI int32_t uri_escape(char *out, uint64_t *require, uint64_t prepare, const char *in) {
    if (!in) return HTTP_ERROR_INVALID_VALUE;
    uint64_t needed = 1;
    for (const char *s = in; *s; ++s) needed += unreserved((unsigned char)*s) ? 1 : 3;
    if (require) *require = needed;
    if (!out) return 0;
    if (prepare < needed) return HTTP_ERROR_OUT_OF_MEMORY;
    static const char hex[] = "0123456789ABCDEF";
    char *dst = out;
    for (const unsigned char *s = (const unsigned char *)in; *s; ++s) {
        if (unreserved(*s)) {
            *dst++ = (char)*s;
        } else {
            *dst++ = '%';
            *dst++ = hex[*s >> 4];
            *dst++ = hex[*s & 15];
        }
    }
    *dst = '\0';
    return 0;
}

static void sweep_path(char *dst, const char *src, uint64_t src_size) {
    if (!src_size) return;
    if (src[0] != '/') {
        memcpy(dst, src, src_size - 1);
        dst[src_size - 1] = '\0';
        return;
    }
    dst[0] = '/';
    dst[1] = '\0';
    if (src_size - 1 <= 1) return;
    uint64_t position = 1;
    char *segment_end = dst;
    while (position < src_size - 1) {
        if (src[position] == '.') {
            if (src[position + 1] == '/') {
                position += 2;
                continue;
            }
            if (src[position + 1] == '.' && src[position + 2] == '/') {
                char *next_end = dst;
                if (segment_end != dst) {
                    *segment_end = '\0';
                    char *previous = strrchr(dst, '/');
                    if (previous) {
                        previous[1] = '\0';
                        next_end = previous;
                    }
                }
                position += 3;
                segment_end = next_end;
                continue;
            }
        }
        const char *start = src + position;
        const char *slash = strchr(start, '/');
        const uint64_t remaining = src_size - position - 1;
        uint64_t length = remaining;
        if (slash) {
            const uint64_t segment = (uint64_t)(slash + 1 - start);
            length = segment <= remaining ? segment : remaining;
        }
        memcpy(segment_end + 1, start, length);
        segment_end[length + 1] = '\0';
        segment_end += length;
        position += length;
    }
}

static int userinfo_punctuation(unsigned char c) {
    switch (c) {
    case 0x21: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2a: case 0x2b:
    case 0x2c: case 0x2d: case 0x2e: case 0x3a: case 0x3b: case 0x3d: case 0x5f: case 0x7e:
        return 1;
    default:
        return 0;
    }
}

static ABI int32_t uri_parse(UriElement *out, const char *source, void *pool, uint64_t *require, uint64_t prepare) {
    if (!source) return HTTP_ERROR_INVALID_URL;
    const int write = out && pool;
    if (!write && !require) return HTTP_ERROR_INVALID_VALUE;
    if (write) memset(out, 0, sizeof(*out));
    char *bytes = pool;

    int has_scheme = 0;
    uint64_t scheme_length = 0;
    {
        uint64_t i = 0;
        while (i < 0x20 && source[i]) {
            const unsigned char c = (unsigned char)source[i];
            if (c == ':') break;
            if (!isalnum(c) && c != '+' && c != '-' && c != '.') break;
            ++i;
        }
        if (i > 0 && source[i] == ':' && isalpha((unsigned char)source[0])) {
            has_scheme = 1;
            scheme_length = i;
        }
    }

    uint64_t used, offset;
    if (has_scheme) {
        if (write) {
            if (prepare < scheme_length + 1) return HTTP_ERROR_OUT_OF_MEMORY;
            memcpy(bytes, source, scheme_length);
            bytes[scheme_length] = '\0';
            out->scheme = bytes;
        }
        used = scheme_length + 1;
        offset = scheme_length + 1;
    } else {
        if (write) {
            if (prepare < 2) return HTTP_ERROR_OUT_OF_MEMORY;
            bytes[0] = '\0';
            out->scheme = bytes;
        }
        used = 1;
        offset = 0;
    }

    int slashes = 0;
    for (const char *p = source + offset; *p == '/'; ++p) ++slashes;
    if (slashes >= 2) offset += 2;
    else if (write) out->opaque = 1;

    const char *authority = source + offset;
    uint64_t scan = 0, colon = 0, at = 0;
    int seen_colon = 0, seen_at = 0;
    for (;;) {
        const unsigned char c = (unsigned char)authority[scan];
        if (!c) break;
        if (c == '@') {
            seen_at = 1;
            at = scan;
            break;
        }
        if (!seen_colon && c == ':') {
            seen_colon = 1;
            colon = scan;
        } else {
            if ((signed char)c < 0) break;
            if (!isalnum(c) && !userinfo_punctuation(c)) break;
        }
        ++scan;
    }

    char *user_dest = bytes + used;
    uint64_t advance = 0;
    if (seen_at) {
        const uint64_t user_length = seen_colon ? colon : at;
        const uint64_t password_offset = seen_colon ? colon + 1 : at + 1;
        const uint64_t password_length = seen_colon ? at - password_offset : 0;
        if (write) {
            if (prepare - used < password_offset + password_length + 1) return HTTP_ERROR_OUT_OF_MEMORY;
            memcpy(user_dest, authority, user_length);
            user_dest[user_length] = '\0';
            memcpy(user_dest + password_offset, authority + password_offset, password_length);
            user_dest[password_offset + password_length] = '\0';
            out->username = user_dest;
            out->password = user_dest + password_offset;
        }
        used += password_offset + password_length + 1;
        advance = at + 1;
    } else {
        if (write) {
            if (prepare - used < 2) return HTTP_ERROR_OUT_OF_MEMORY;
            user_dest[0] = user_dest[1] = '\0';
            out->username = user_dest;
            out->password = user_dest + 1;
        }
        used += 2;
    }
    offset += advance;

    char *host_dest = bytes + used;
    const char *host = source + offset;
    const char first = host[0];
    uint64_t host_scan = 0, host_stored = 0;
    if (first == '.') {
        host_scan = host_stored = 0;
    } else if (first == '[') {
        host_scan = 1;
        for (;;) {
            if (host_scan == 0xff) return HTTP_ERROR_INVALID_URL;
            const unsigned char c = (unsigned char)host[host_scan];
            if (!c || (signed char)c < 0 || c == ']') break;
            if (!isalnum(c) && c != '-' && c != '.' && c != '_' && c != ':') break;
            ++host_scan;
        }
        if (host[host_scan] != ']') return HTTP_ERROR_INVALID_URL;
        host_stored = host_scan - 1;
        ++host_scan;
    } else {
        for (;;) {
            if (host_scan == 0xff) return HTTP_ERROR_INVALID_URL;
            const unsigned char c = (unsigned char)host[host_scan];
            if (!c || (signed char)c < 0) break;
            if (!isalnum(c) && c != '-' && c != '.' && c != '_') break;
            ++host_scan;
        }
        host_stored = host_scan;
    }
    if (write) {
        if (prepare - used < host_stored + 1) return HTTP_ERROR_OUT_OF_MEMORY;
        memcpy(host_dest, first == '[' ? host + 1 : host, host_stored);
        host_dest[host_stored] = '\0';
        out->hostname = host_dest;
    }
    used += host_stored + 1;
    offset += host_scan;

    int explicit_port = 0;
    uint16_t port = 0;
    if (source[offset] == ':') {
        ++offset;
        const char *digits = source + offset;
        uint64_t count = 0;
        uint32_t value = 0;
        while (count < 5 && isdigit((unsigned char)digits[count])) {
            value = value * 10 + (uint32_t)(digits[count] - '0');
            ++count;
        }
        if (value > 0x10000) return HTTP_ERROR_INVALID_URL;
        if (digits[count] != '\0' && digits[count] != '/') return HTTP_ERROR_INVALID_URL;
        if (count) {
            explicit_port = 1;
            port = (uint16_t)value;
        }
        offset += count;
    }
    if (write) out->port = explicit_port ? port : scheme_default_port(out->scheme, strlen(out->scheme));

    char *path_dest = bytes + used;
    const char *path = source + offset;
    uint64_t path_length = 0;
    while (path[path_length] && path[path_length] != '?' && path[path_length] != '#') {
        if (path_length >= 0x3fff) return HTTP_ERROR_INVALID_URL;
        ++path_length;
    }
    if (write) {
        if (prepare - used < path_length + 1) return HTTP_ERROR_OUT_OF_MEMORY;
        char *copy = malloc(path_length + 1);
        if (!copy) return HTTP_ERROR_OUT_OF_MEMORY;
        memcpy(copy, path, path_length);
        copy[path_length] = '\0';
        memcpy(path_dest, path, path_length);
        path_dest[path_length] = '\0';
        sweep_path(path_dest, copy, path_length + 1);
        free(copy);
        out->path = path_dest;
    }
    used += path_length + 1;
    offset += path_length;

    char *query_dest = bytes + used;
    uint64_t query_length = 0;
    if (source[offset] == '?') {
        query_length = 1;
        while (source[offset + query_length] && source[offset + query_length] != '#') {
            if (query_length >= 0x3fff) return HTTP_ERROR_INVALID_URL;
            ++query_length;
        }
    }
    if (write) {
        if (prepare - used < query_length + 1) return HTTP_ERROR_OUT_OF_MEMORY;
        memcpy(query_dest, source + offset, query_length);
        query_dest[query_length] = '\0';
        out->query = query_dest;
    }
    used += query_length + 1;
    offset += query_length;

    char *fragment_dest = bytes + used;
    uint64_t fragment_length = 0;
    if (source[offset] == '#') {
        fragment_length = 1;
        while (source[offset + fragment_length]) {
            if (fragment_length >= 0x3fff) return HTTP_ERROR_INVALID_URL;
            ++fragment_length;
        }
    }
    if (write) {
        if (prepare - used < fragment_length + 1) return HTTP_ERROR_OUT_OF_MEMORY;
        memcpy(fragment_dest, source + offset, fragment_length);
        fragment_dest[fragment_length] = '\0';
        out->fragment = fragment_dest;
    }
    used += fragment_length + 1;
    if (require) *require = used;
    return 0;
}

static const RuntimeExport exports[] = {
    {"sceHttpUriParse", uri_parse},
    {"sceHttpUriBuild", uri_build},
    {"sceHttpUriEscape", uri_escape},
};

uintptr_t runtime_http_uri_resolve(const char *name) { return RUNTIME_LOOKUP(exports, name); }
