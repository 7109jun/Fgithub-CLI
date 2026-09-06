/*
 * Fgithub CLI v1.0.0
 * Single-file C / Windows / no third-party libraries.
 *
 * Native APIs used:
 *   WinHTTP  - HTTPS / HTTP transport
 *   DPAPI    - local token protection
 *   Shell32  - ShellExecuteA for optional editor support
 *
 * Build (MinGW-w64):
 *   gcc -O2 -std=c11 -Wall -Wextra -Wpedantic -D_CRT_SECURE_NO_WARNINGS -o fgh.exe fgh.c -lwinhttp -lcrypt32 -lshell32
 *
 * Build (MSVC):
 *   cl /O2 /W4 /D_CRT_SECURE_NO_WARNINGS fgh.c winhttp.lib crypt32.lib shell32.lib
 *
 * Authentication syntax is intentionally kept as:
 *   login name("USER") password("TOKEN")
 * password(...) is a GitHub access token, not a GitHub account password.
 */

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "shell32.lib")

#define FGH_VERSION       "1.1.0"
#define FGH_API_VERSION   "2026-03-10"
#define FGH_API_HOST      "api.github.com"
#define FGH_UPLOAD_HOST   "uploads.github.com"
#define FGH_MAX_CMD       65536
#define FGH_MAX_TOKEN     8192
#define FGH_MAX_TEXT      65536
#define FGH_MAX_JSON      (32u * 1024u * 1024u)
#define FGH_MAX_TOKENS    128
#define FGH_CFG_MAGIC     "FGH1"

static char g_token[FGH_MAX_TOKEN];
static char g_login[256];
static char g_default_branch[256] = "main";
static char g_editor[256] = "notepad";
static char g_current_repo[512];

static void fgh_error(const char *msg) {
    fprintf(stderr, "fgh: %s\n", msg);
}

static void fgh_errorf(const char *fmt, const char *arg) {
    fprintf(stderr, "fgh: ");
    fprintf(stderr, fmt, arg);
    fputc('\n', stderr);
}

static int str_ieq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        ++a; ++b;
    }
    return *a == 0 && *b == 0;
}

static int str_starts_i(const char *s, const char *prefix) {
    if (!s || !prefix) return 0;
    while (*prefix) {
        if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
        ++s; ++prefix;
    }
    return 1;
}

static void trim_inplace(char *s) {
    size_t n, i = 0;
    if (!s) return;
    n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    while (isspace((unsigned char)s[i])) ++i;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
}

static int copy_string(char *dst, size_t cap, const char *src) {
    size_t n;
    if (!dst || cap == 0 || !src) return 0;
    n = strlen(src);
    if (n >= cap) return 0;
    memcpy(dst, src, n + 1);
    return 1;
}


static char *dup_string(const char *src) {
    size_t n;
    char *p;
    if (!src) return NULL;
    n = strlen(src);
    p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, src, n + 1);
    return p;
}
static int utf8_to_wide_alloc(const char *src, wchar_t **out) {
    int n;
    wchar_t *p;
    if (!src || !out) return 0;
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, NULL, 0);
    if (n <= 0) return 0;
    p = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!p) return 0;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, p, n) != n) {
        free(p);
        return 0;
    }
    *out = p;
    return 1;
}

static int url_encode(const char *src, char *dst, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t i, j = 0;
    if (!dst || cap == 0) return 0;
    for (i = 0; src && src[i]; ++i) {
        unsigned char c = (unsigned char)src[i];
        int safe = isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            if (j + 2 > cap) return 0;
            dst[j++] = (char)c;
        } else {
            if (j + 4 > cap) return 0;
            dst[j++] = '%';
            dst[j++] = hex[c >> 4];
            dst[j++] = hex[c & 15];
        }
    }
    dst[j] = 0;
    return 1;
}

static int url_encode_path(const char *src, char *dst, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t i, j = 0;
    if (!dst || cap == 0) return 0;
    for (i = 0; src && src[i]; ++i) {
        unsigned char c = (unsigned char)src[i];
        int safe = isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
        if (safe) {
            if (j + 2 > cap) return 0;
            dst[j++] = (char)c;
        } else {
            if (j + 4 > cap) return 0;
            dst[j++] = '%';
            dst[j++] = hex[c >> 4];
            dst[j++] = hex[c & 15];
        }
    }
    dst[j] = 0;
    return 1;
}

static int json_escape(const char *src, char *dst, size_t cap) {
    size_t i, j = 0;
    if (!dst || cap == 0) return 0;
    for (i = 0; src && src[i]; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (j + 2 >= cap) return 0;
            dst[j++] = '\\'; dst[j++] = (char)c;
        } else if (c == '\n') {
            if (j + 2 >= cap) return 0;
            dst[j++] = '\\'; dst[j++] = 'n';
        } else if (c == '\r') {
            if (j + 2 >= cap) return 0;
            dst[j++] = '\\'; dst[j++] = 'r';
        } else if (c == '\t') {
            if (j + 2 >= cap) return 0;
            dst[j++] = '\\'; dst[j++] = 't';
        } else if (c < 0x20) {
            if (j + 6 >= cap) return 0;
            snprintf(dst + j, cap - j, "\\u%04x", (unsigned)c);
            j += 6;
        } else {
            if (j + 1 >= cap) return 0;
            dst[j++] = (char)c;
        }
    }
    if (j >= cap) return 0;
    dst[j] = 0;
    return 1;
}

static int base64_encode(const unsigned char *src, size_t len, char **out) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_len, i = 0, j = 0;
    char *p;
    if (!out || (!src && len)) return 0;
    out_len = ((len + 2) / 3) * 4;
    p = (char *)malloc(out_len + 1);
    if (!p) return 0;
    while (i < len) {
        size_t rem = len - i;
        unsigned a = src[i++];
        unsigned b = rem > 1 ? src[i++] : 0;
        unsigned c = rem > 2 ? src[i++] : 0;
        p[j++] = tbl[(a >> 2) & 0x3f];
        p[j++] = tbl[((a & 0x03) << 4) | ((b >> 4) & 0x0f)];
        p[j++] = rem > 1 ? tbl[((b & 0x0f) << 2) | ((c >> 6) & 0x03)] : '=';
        p[j++] = rem > 2 ? tbl[c & 0x3f] : '=';
    }
    p[j] = 0;
    *out = p;
    return 1;
}

static int read_file(const char *path, unsigned char **data, size_t *len) {
    FILE *f;
    long sz;
    size_t got;
    unsigned char *p;
    if (!path || !data || !len) return 0;
    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "fgh: cannot open '%s': %s\n", path, strerror(errno));
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    sz = ftell(f);
    if (sz < 0) { fclose(f); return 0; }
    if ((unsigned long long)sz > (256ull * 1024ull * 1024ull)) {
        fclose(f);
        fgh_error("file is larger than 256 MiB");
        return 0;
    }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    p = (unsigned char *)malloc((size_t)sz + 1);
    if (!p) { fclose(f); return 0; }
    got = fread(p, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(p); return 0; }
    p[got] = 0;
    *data = p;
    *len = got;
    return 1;
}

/* ---------------- Robust JSON parser ---------------- */
typedef struct {
    const char *p;
    const char *end;
    int error;
} JsonParser;

static void json_set_error(JsonParser *jp) { if (jp) jp->error = 1; }

static void json_ws(JsonParser *jp) {
    while (jp && jp->p < jp->end && isspace((unsigned char)*jp->p)) ++jp->p;
}

static int json_hex4(const char *p, unsigned *v) {
    int i;
    unsigned x = 0;
    if (!p || !v) return 0;
    for (i = 0; i < 4; ++i) {
        unsigned char c = (unsigned char)p[i];
        x <<= 4;
        if (c >= '0' && c <= '9') x |= c - '0';
        else if (c >= 'a' && c <= 'f') x |= c - 'a' + 10u;
        else if (c >= 'A' && c <= 'F') x |= c - 'A' + 10u;
        else return 0;
    }
    *v = x;
    return 1;
}

static int raw_append(unsigned char c, char **buf, size_t *len, size_t *cap) {
    size_t need = *len + 2;
    if (need > *cap) {
        size_t nc = *cap ? *cap : 64;
        while (nc < need) {
            if (nc > FGH_MAX_CMD / 2) return 0;
            nc *= 2;
        }
        if (nc > FGH_MAX_CMD) return 0;
        {
            char *nb = (char *)realloc(*buf, nc);
            if (!nb) return 0;
            *buf = nb; *cap = nc;
        }
    }
    (*buf)[(*len)++] = (char)c;
    (*buf)[*len] = 0;
    return 1;
}

static int utf8_append(unsigned cp, char **buf, size_t *len, size_t *cap) {
    unsigned char bytes[4];
    size_t n, need;
    if (!buf || !len || !cap) return 0;
    if (cp <= 0x7F) {
        bytes[0] = (unsigned char)cp; n = 1;
    } else if (cp <= 0x7FF) {
        bytes[0] = (unsigned char)(0xC0 | (cp >> 6));
        bytes[1] = (unsigned char)(0x80 | (cp & 0x3F)); n = 2;
    } else if (cp <= 0xFFFF) {
        bytes[0] = (unsigned char)(0xE0 | (cp >> 12));
        bytes[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | (cp & 0x3F)); n = 3;
    } else if (cp <= 0x10FFFF) {
        bytes[0] = (unsigned char)(0xF0 | (cp >> 18));
        bytes[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        bytes[3] = (unsigned char)(0x80 | (cp & 0x3F)); n = 4;
    } else return 0;
    need = *len + n + 1;
    if (need > *cap) {
        size_t nc = *cap ? *cap : 64;
        while (nc < need) {
            if (nc > SIZE_MAX / 2) return 0;
            nc *= 2;
        }
        {
            char *nb = (char *)realloc(*buf, nc);
            if (!nb) return 0;
            *buf = nb; *cap = nc;
        }
    }
    memcpy(*buf + *len, bytes, n);
    *len += n;
    (*buf)[*len] = 0;
    return 1;
}

static char *json_parse_string_alloc(JsonParser *jp) {
    char *out = NULL;
    size_t len = 0, cap = 0;
    if (!jp) return NULL;
    json_ws(jp);
    if (jp->p >= jp->end || *jp->p != '"') { json_set_error(jp); return NULL; }
    ++jp->p;
    while (jp->p < jp->end) {
        unsigned char c = (unsigned char)*jp->p++;
        if (c == '"') {
            if (!out) {
                out = (char *)malloc(1);
                if (!out) { json_set_error(jp); return NULL; }
                out[0] = 0;
            }
            return out;
        }
        if (c < 0x20) { free(out); json_set_error(jp); return NULL; }
        if (c != '\\') {
            if (!raw_append(c, &out, &len, &cap)) { free(out); json_set_error(jp); return NULL; }
            continue;
        }
        if (jp->p >= jp->end) { free(out); json_set_error(jp); return NULL; }
        c = (unsigned char)*jp->p++;
        switch (c) {
            case '"': if (!utf8_append('"', &out, &len, &cap)) goto oom; break;
            case '\\': if (!utf8_append('\\', &out, &len, &cap)) goto oom; break;
            case '/': if (!utf8_append('/', &out, &len, &cap)) goto oom; break;
            case 'b': if (!utf8_append('\b', &out, &len, &cap)) goto oom; break;
            case 'f': if (!utf8_append('\f', &out, &len, &cap)) goto oom; break;
            case 'n': if (!utf8_append('\n', &out, &len, &cap)) goto oom; break;
            case 'r': if (!utf8_append('\r', &out, &len, &cap)) goto oom; break;
            case 't': if (!utf8_append('\t', &out, &len, &cap)) goto oom; break;
            case 'u': {
                unsigned cp;
                if (jp->end - jp->p < 4 || !json_hex4(jp->p, &cp)) { free(out); json_set_error(jp); return NULL; }
                jp->p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    const char *save = jp->p;
                    unsigned low;
                    if (jp->end - jp->p >= 6 && jp->p[0] == '\\' && jp->p[1] == 'u' && json_hex4(jp->p + 2, &low) && low >= 0xDC00 && low <= 0xDFFF) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                        jp->p += 6;
                    } else {
                        jp->p = save;
                        free(out); json_set_error(jp); return NULL;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    free(out); json_set_error(jp); return NULL;
                }
                if (!utf8_append(cp, &out, &len, &cap)) goto oom;
                break;
            }
            default: free(out); json_set_error(jp); return NULL;
        }
    }
    free(out); json_set_error(jp); return NULL;
oom:
    free(out); json_set_error(jp); return NULL;
}

static int json_skip_value_parser(JsonParser *jp);

static int json_skip_number(JsonParser *jp) {
    const char *p = jp->p;
    if (p < jp->end && *p == '-') ++p;
    if (p >= jp->end) return 0;
    if (*p == '0') ++p;
    else {
        if (!isdigit((unsigned char)*p)) return 0;
        while (p < jp->end && isdigit((unsigned char)*p)) ++p;
    }
    if (p < jp->end && *p == '.') {
        ++p;
        if (p >= jp->end || !isdigit((unsigned char)*p)) return 0;
        while (p < jp->end && isdigit((unsigned char)*p)) ++p;
    }
    if (p < jp->end && (*p == 'e' || *p == 'E')) {
        ++p;
        if (p < jp->end && (*p == '+' || *p == '-')) ++p;
        if (p >= jp->end || !isdigit((unsigned char)*p)) return 0;
        while (p < jp->end && isdigit((unsigned char)*p)) ++p;
    }
    jp->p = p;
    return 1;
}

static int json_match_literal(JsonParser *jp, const char *lit) {
    size_t n = strlen(lit);
    if ((size_t)(jp->end - jp->p) < n || memcmp(jp->p, lit, n) != 0) return 0;
    jp->p += n;
    return 1;
}

static int json_skip_value_parser(JsonParser *jp) {
    if (!jp) return 0;
    json_ws(jp);
    if (jp->p >= jp->end) return 0;
    switch (*jp->p) {
        case '"': {
            char *s = json_parse_string_alloc(jp);
            if (!s) return 0;
            free(s); return 1;
        }
        case '{': {
            char *key = NULL;
            ++jp->p;
            json_ws(jp);
            if (jp->p < jp->end && *jp->p == '}') { ++jp->p; return 1; }
            for (;;) {
                key = json_parse_string_alloc(jp);
                if (!key) return 0;
                free(key); key = NULL;
                json_ws(jp);
                if (jp->p >= jp->end || *jp->p != ':') return 0;
                ++jp->p;
                if (!json_skip_value_parser(jp)) return 0;
                json_ws(jp);
                if (jp->p < jp->end && *jp->p == '}') { ++jp->p; return 1; }
                if (jp->p >= jp->end || *jp->p != ',') return 0;
                ++jp->p;
                json_ws(jp);
            }
        }
        case '[': {
            ++jp->p;
            json_ws(jp);
            if (jp->p < jp->end && *jp->p == ']') { ++jp->p; return 1; }
            for (;;) {
                if (!json_skip_value_parser(jp)) return 0;
                json_ws(jp);
                if (jp->p < jp->end && *jp->p == ']') { ++jp->p; return 1; }
                if (jp->p >= jp->end || *jp->p != ',') return 0;
                ++jp->p;
                json_ws(jp);
            }
        }
        case 't': return json_match_literal(jp, "true");
        case 'f': return json_match_literal(jp, "false");
        case 'n': return json_match_literal(jp, "null");
        default: return json_skip_number(jp);
    }
}

static int json_get_key_value_range(const char *json, const char *key, const char **vstart, const char **vend) {
    JsonParser jp;
    size_t key_len;
    if (!json || !key || !vstart || !vend) return 0;
    jp.p = json; jp.end = json + strlen(json); jp.error = 0;
    json_ws(&jp);
    if (jp.p >= jp.end || *jp.p != '{') return 0;
    ++jp.p;
    key_len = strlen(key);
    json_ws(&jp);
    if (jp.p < jp.end && *jp.p == '}') return 0;
    for (;;) {
        char *actual = json_parse_string_alloc(&jp);
        if (!actual) return 0;
        json_ws(&jp);
        if (jp.p >= jp.end || *jp.p != ':') { free(actual); return 0; }
        ++jp.p;
        json_ws(&jp);
        {
            const char *vs = jp.p;
            if (!json_skip_value_parser(&jp)) { free(actual); return 0; }
            if (strlen(actual) == key_len && memcmp(actual, key, key_len) == 0) {
                *vstart = vs; *vend = jp.p;
                free(actual);
                return 1;
            }
        }
        free(actual);
        json_ws(&jp);
        if (jp.p < jp.end && *jp.p == '}') return 0;
        if (jp.p >= jp.end || *jp.p != ',') return 0;
        ++jp.p;
        json_ws(&jp);
    }
}

static int json_get_string(const char *json, const char *key, char *out, size_t cap) {
    const char *vs, *ve;
    JsonParser jp;
    char *s;
    size_t n;
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!json_get_key_value_range(json, key, &vs, &ve) || vs >= ve || *vs != '"') return 0;
    jp.p = vs; jp.end = ve; jp.error = 0;
    s = json_parse_string_alloc(&jp);
    if (!s || jp.error) { free(s); return 0; }
    n = strlen(s);
    if (n >= cap) { free(s); return 0; }
    memcpy(out, s, n + 1);
    free(s);
    return 1;
}

static long json_get_long(const char *json, const char *key, long def) {
    const char *vs, *ve, *p, *e;
    char tmp[64]; size_t n;
    long v;
    if (!json_get_key_value_range(json, key, &vs, &ve)) return def;
    n = (size_t)(ve - vs);
    if (n == 0 || n >= sizeof(tmp)) return def;
    memcpy(tmp, vs, n); tmp[n] = 0;
    p = tmp; v = strtol(p, (char **)&e, 10);
    return (e == p) ? def : v;
}

static int json_get_bool(const char *json, const char *key, int def) {
    const char *vs, *ve;
    if (!json_get_key_value_range(json, key, &vs, &ve)) return def;
    if ((size_t)(ve - vs) == 4 && memcmp(vs, "true", 4) == 0) return 1;
    if ((size_t)(ve - vs) == 5 && memcmp(vs, "false", 5) == 0) return 0;
    return def;
}

/* Finds direct object members of a top-level array. No strstr(), so strings cannot impersonate keys. */
typedef int (*json_object_cb)(const char *obj, size_t len, void *ctx);
static int json_for_each_object(const char *json, const char *array_key, json_object_cb cb, void *ctx) {
    const char *vs, *ve, *p;
    JsonParser jp;
    if (!cb || !json_get_key_value_range(json, array_key, &vs, &ve) || vs >= ve || *vs != '[') return 0;
    jp.p = vs; jp.end = ve; jp.error = 0;
    json_ws(&jp); ++jp.p;
    json_ws(&jp);
    while (jp.p < jp.end && *jp.p != ']') {
        const char *start = jp.p, *before;
        if (*jp.p != '{') return 0;
        before = jp.p;
        if (!json_skip_value_parser(&jp) || jp.p <= before) return 0;
        p = jp.p;
        if (!cb(start, (size_t)(p - start), ctx)) return 1;
        json_ws(&jp);
        if (jp.p < jp.end && *jp.p == ',') { ++jp.p; json_ws(&jp); }
        else break;
    }
    return !jp.error;
}

/* Compatibility helpers used by list renderers. These also skip quoted strings correctly. */
static const char *json_skip_ws(const char *p) {
    while (p && *p && isspace((unsigned char)*p)) ++p;
    return p;
}

static const char *json_skip_string(const char *p) {
    if (!p || *p != '"') return p;
    ++p;
    while (*p) {
        if (*p == '\\') {
            if (!p[1]) return p + 1;
            p += 2;
            continue;
        }
        if (*p == '"') return p + 1;
        ++p;
    }
    return p;
}

static int get_config_dir(char *out, size_t cap) {
    char appdata[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("APPDATA", appdata, sizeof(appdata));
    if (!n || n >= sizeof(appdata)) return 0;
    if (_snprintf_s(out, cap, _TRUNCATE, "%s\\fgh", appdata) < 0) return 0;
    CreateDirectoryA(out, NULL);
    return 1;
}

static int get_config_file(char *out, size_t cap) {
    char dir[MAX_PATH * 2];
    if (!get_config_dir(dir, sizeof(dir))) return 0;
    return _snprintf_s(out, cap, _TRUNCATE, "%s\\config.dat", dir) >= 0;
}

static int dpapi_protect(const char *plain, BYTE **blob, DWORD *blob_len) {
    DATA_BLOB in, out;
    if (!plain || !blob || !blob_len) return 0;
    in.pbData = (BYTE *)plain;
    in.cbData = (DWORD)strlen(plain);
    if (!CryptProtectData(&in, L"Fgithub token", NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) return 0;
    *blob = out.pbData;
    *blob_len = out.cbData;
    return 1;
}

static int dpapi_unprotect(const BYTE *blob, DWORD blob_len, char *out, size_t cap) {
    DATA_BLOB in, dec;
    size_t n;
    if (!blob || !out || cap == 0) return 0;
    in.pbData = (BYTE *)blob;
    in.cbData = blob_len;
    if (!CryptUnprotectData(&in, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &dec)) return 0;
    n = dec.cbData < cap - 1 ? dec.cbData : cap - 1;
    memcpy(out, dec.pbData, n);
    out[n] = 0;
    LocalFree(dec.pbData);
    return 1;
}

static int save_config(void) {
    char path[MAX_PATH * 2];
    FILE *f;
    BYTE *blob = NULL;
    DWORD blob_len = 0;
    if (!get_config_file(path, sizeof(path))) return 0;
    if (g_token[0] && !dpapi_protect(g_token, &blob, &blob_len)) return 0;
    f = fopen(path, "wb");
    if (!f) { if (blob) LocalFree(blob); return 0; }
    fprintf(f, "%s\n", FGH_CFG_MAGIC);
    fprintf(f, "login=%s\n", g_login);
    fprintf(f, "default_branch=%s\n", g_default_branch);
    fprintf(f, "editor=%s\n", g_editor);
    fprintf(f, "repo=%s\n", g_current_repo);
    fprintf(f, "token_len=%lu\n", (unsigned long)blob_len);
    if (blob_len && fwrite(blob, 1, blob_len, f) != blob_len) {
        fclose(f);
        LocalFree(blob);
        return 0;
    }
    fclose(f);
    if (blob) LocalFree(blob);
    return 1;
}

static int load_config(void) {
    char path[MAX_PATH * 2], line[1024];
    FILE *f;
    unsigned long token_len = 0;
    BYTE *blob = NULL;
    if (!get_config_file(path, sizeof(path))) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }
    trim_inplace(line);
    if (strcmp(line, FGH_CFG_MAGIC) != 0) { fclose(f); return 0; }
    while (fgets(line, sizeof(line), f)) {
        trim_inplace(line);
        if (str_starts_i(line, "login=")) copy_string(g_login, sizeof(g_login), line + 6);
        else if (str_starts_i(line, "default_branch=")) copy_string(g_default_branch, sizeof(g_default_branch), line + 15);
        else if (str_starts_i(line, "editor=")) copy_string(g_editor, sizeof(g_editor), line + 7);
        else if (str_starts_i(line, "repo=")) copy_string(g_current_repo, sizeof(g_current_repo), line + 5);
        else if (str_starts_i(line, "token_len=")) {
            token_len = strtoul(line + 10, NULL, 10);
            break; /* encrypted token begins immediately after this line */
        }
    }
    if (token_len > 0 && token_len < FGH_MAX_JSON) {
        blob = (BYTE *)malloc((size_t)token_len);
        if (blob) {
            if (fread(blob, 1, token_len, f) == token_len)
                dpapi_unprotect(blob, (DWORD)token_len, g_token, sizeof(g_token));
            free(blob);
        }
    }
    fclose(f);
    return 1;
}

static void clear_config(void) {
    char path[MAX_PATH * 2];
    if (get_config_file(path, sizeof(path))) DeleteFileA(path);
    memset(g_token, 0, sizeof(g_token));
    memset(g_login, 0, sizeof(g_login));
    memset(g_current_repo, 0, sizeof(g_current_repo));
}

static void print_http_error(DWORD status, const unsigned char *body) {
    printf("HTTP %lu\n", (unsigned long)status);
    if (body && *body) {
        char msg[1024] = {0};
        if (json_get_string((const char *)body, "message", msg, sizeof(msg))) printf("%s\n", msg);
        else printf("%s\n", body);
    }
}

static int http_request_internal(const char *host, const char *verb, const char *path,
                                 const void *body, size_t body_len, const char *content_type,
                                 unsigned char **response, size_t *response_len, DWORD *status) {
    HINTERNET session = NULL, conn = NULL, req = NULL;
    wchar_t *whost = NULL, *wpath = NULL, *wverb = NULL, *whdr = NULL;
    char headers[4096];
    int ok = 0;
    size_t used = 0, cap = 65536;
    unsigned char *buf = NULL;
    DWORD sc_len = sizeof(DWORD), avail = 0, got = 0;

    *response = NULL; *response_len = 0; *status = 0;

    if (!utf8_to_wide_alloc(host, &whost) || !utf8_to_wide_alloc(path, &wpath) || !utf8_to_wide_alloc(verb, &wverb)) goto cleanup;

    session = WinHttpOpen(L"Fgithub-CLI/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) goto cleanup;
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 30000);

    conn = WinHttpConnect(session, whost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) goto cleanup;
    req = WinHttpOpenRequest(conn, wverb, wpath, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) goto cleanup;

    _snprintf_s(headers, sizeof(headers), _TRUNCATE,
                "Accept: application/vnd.github+json\r\n"
                "Authorization: Bearer %s\r\n"
                "X-GitHub-Api-Version: %s\r\n"
                "User-Agent: Fgithub-CLI/%s\r\n"
                "%s",
                g_token, FGH_API_VERSION, FGH_VERSION,
                content_type ? content_type : "");
    if (!utf8_to_wide_alloc(headers, &whdr)) goto cleanup;

    if (body_len > 0xFFFFFFFFu) { goto cleanup; }
    if (!WinHttpSendRequest(req, whdr, (DWORD)-1L,
                            (void *)body, (DWORD)body_len, (DWORD)body_len, 0)) goto cleanup;
    if (!WinHttpReceiveResponse(req, NULL)) goto cleanup;
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, status, &sc_len, WINHTTP_NO_HEADER_INDEX)) goto cleanup;

    buf = (unsigned char *)malloc(cap);
    if (!buf) goto cleanup;
    for (;;) {
        if (!WinHttpQueryDataAvailable(req, &avail)) goto cleanup;
        if (!avail) break;
        if (used + avail + 1 > cap) {
            size_t new_cap = cap;
            while (used + avail + 1 > new_cap) {
                if (new_cap > FGH_MAX_JSON / 2) { new_cap = FGH_MAX_JSON; break; }
                new_cap *= 2;
            }
            if (new_cap < used + avail + 1 || new_cap > FGH_MAX_JSON) goto cleanup;
            {
                unsigned char *nb = (unsigned char *)realloc(buf, new_cap);
                if (!nb) goto cleanup;
                buf = nb;
                cap = new_cap;
            }
        }
        if (!WinHttpReadData(req, buf + used, avail, &got)) goto cleanup;
        used += got;
    }
    buf[used] = 0;

    *response = buf;
    *response_len = used;
    buf = NULL;
    ok = 1;

cleanup:
    free(whost); free(wpath); free(wverb); free(whdr);
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    free(buf);
    return ok;
}

static int api_request(const char *verb, const char *path, const char *body,
                       const char *content_type, unsigned char **response,
                       size_t *response_len, DWORD *status) {
    if (!g_token[0]) {
        fgh_error("not logged in; use: login name(\"...\") password(\"TOKEN\")");
        return 0;
    }
    if (!http_request_internal(FGH_API_HOST, verb, path, body,
                               body ? strlen(body) : 0, content_type,
                               response, response_len, status)) {
        fgh_error("network request failed");
        return 0;
    }
    if (*status < 200 || *status >= 300) {
        print_http_error(*status, *response);
        return 0;
    }
    return 1;
}

static int api_binary_request(const char *host, const char *verb, const char *path,
                              const unsigned char *body, size_t body_len,
                              const char *content_type, unsigned char **response,
                              size_t *response_len, DWORD *status) {
    if (!g_token[0]) {
        fgh_error("not logged in");
        return 0;
    }
    if (!http_request_internal(host, verb, path, body, body_len, content_type,
                               response, response_len, status)) {
        fgh_error("network request failed");
        return 0;
    }
    return 1;
}

static int split_owner_repo(const char *full, char *owner, size_t owner_cap,
                            char *repo, size_t repo_cap) {
    const char *slash;
    size_t n;
    if (!full || !owner || !repo) return 0;
    slash = strchr(full, '/');
    if (!slash || slash == full || !slash[1]) return 0;
    n = (size_t)(slash - full);
    if (n >= owner_cap || strlen(slash + 1) >= repo_cap) return 0;
    memcpy(owner, full, n); owner[n] = 0;
    return copy_string(repo, repo_cap, slash + 1);
}

static int ensure_repo_full(const char *input, char *full, size_t cap) {
    if (!input || !*input) return 0;
    if (strchr(input, '/')) return copy_string(full, cap, input);
    if (!g_login[0]) return 0;
    if (_snprintf_s(full, cap, _TRUNCATE, "%s/%s", g_login, input) < 0) return 0;
    return 1;
}

static int get_repo_default_branch(const char *full, char *branch, size_t cap) {
    char owner[256], repo[256], path[1024], tmp[256];
    unsigned char *r = NULL; size_t n = 0; DWORD st = 0;
    if (!split_owner_repo(full, owner, sizeof(owner), repo, sizeof(repo))) return 0;
    _snprintf_s(path, sizeof(path), _TRUNCATE, "/repos/%s/%s", owner, repo);
    if (!api_request("GET", path, NULL, NULL, &r, &n, &st)) { free(r); return 0; }
    if (!json_get_string((const char *)r, "default_branch", tmp, sizeof(tmp))) { free(r); return 0; }
    if (!copy_string(branch, cap, tmp)) { free(r); return 0; }
    free(r);
    return 1;
}

static int get_branch_sha(const char *full, const char *branch, char *sha, size_t cap) {
    char owner[256], repo[256], eb[1024], path[2048], tmp[256];
    unsigned char *r = NULL; size_t n = 0; DWORD st = 0;
    if (!split_owner_repo(full, owner, sizeof(owner), repo, sizeof(repo))) return 0;
    if (!url_encode_path(branch, eb, sizeof(eb))) return 0;
    _snprintf_s(path, sizeof(path), _TRUNCATE, "/repos/%s/%s/git/ref/heads/%s", owner, repo, eb);
    if (!api_request("GET", path, NULL, NULL, &r, &n, &st)) { free(r); return 0; }
    if (!json_get_string((const char *)r, "sha", tmp, sizeof(tmp))) { free(r); return 0; }
    if (!copy_string(sha, cap, tmp)) { free(r); return 0; }
    free(r);
    return 1;
}

static void print_user_summary(const char *json) {
    char login[256] = {0}, name[256] = {0}, bio[2048] = {0}, html[1024] = {0};
    long id = json_get_long(json, "id", 0);
    if (json_get_string(json, "login", login, sizeof(login))) printf("login: %s\n", login);
    if (json_get_string(json, "name", name, sizeof(name)) && name[0]) printf("name: %s\n", name);
    printf("id: %ld\n", id);
    if (json_get_string(json, "bio", bio, sizeof(bio)) && bio[0]) printf("bio: %s\n", bio);
    if (json_get_string(json, "html_url", html, sizeof(html))) printf("web: %s\n", html);
    printf("public repos: %ld\n", json_get_long(json, "public_repos", 0));
    printf("followers: %ld\n", json_get_long(json, "followers", 0));
    printf("following: %ld\n", json_get_long(json, "following", 0));
}

static int cmd_login(const char *name, const char *token) {
    unsigned char *r = NULL; size_t n = 0; DWORD st = 0;
    char actual[256] = {0};
    if (!token || !*token) { fgh_error("token is empty"); return 0; }
    if (!copy_string(g_token, sizeof(g_token), token)) { fgh_error("token is too long"); return 0; }
    if (!api_request("GET", "/user", NULL, NULL, &r, &n, &st)) {
        memset(g_token, 0, sizeof(g_token));
        free(r);
        return 0;
    }
    if (!json_get_string((const char *)r, "login", actual, sizeof(actual))) {
        copy_string(actual, sizeof(actual), name ? name : "unknown");
    }
    if (!copy_string(g_login, sizeof(g_login), actual)) { free(r); return 0; }
    save_config();
    printf("Logged in as %s.\n", g_login);
    free(r);
    return 1;
}

static int cmd_status(void) {
    unsigned char *r = NULL; size_t n = 0; DWORD st = 0;
    if (!g_token[0]) { printf("Not logged in.\n"); return 1; }
    if (!api_request("GET", "/user", NULL, NULL, &r, &n, &st)) { free(r); return 0; }
    printf("Logged in as %s.\n", g_login[0] ? g_login : "unknown");
    print_user_summary((const char *)r);
    free(r);
    return 1;
}

static int cmd_new_repo(const char *name, const char *access) {
    char ename[2048], body[4096], html[1024] = {0}, full[512] = {0};
    unsigned char *r = NULL; size_t n = 0; DWORD st = 0;
    int is_private;
    if (!name || !*name) { fgh_error("repository name is empty"); return 0; }
    if (!json_escape(name, ename, sizeof(ename))) return 0;
    is_private = str_starts_i(access, "private");
    _snprintf_s(body, sizeof(body), _TRUNCATE,
                "{\"name\":\"%s\",\"private\":%s}",
                ename, is_private ? "true" : "false");
    if (!api_request("POST", "/user/repos", body, "Content-Type: application/json\r\n", &r, &n, &st)) { free(r); return 0; }
    json_get_string((const char *)r, "html_url", html, sizeof(html));
    if (g_login[0]) _snprintf_s(full, sizeof(full), _TRUNCATE, "%s/%s", g_login, name);
    if (full[0]) {
        if (!copy_string(g_current_repo, sizeof(g_current_repo), full)) { free(r); fgh_error("repository name is too long"); return 0; }
        save_config();
    }
    printf("Repository created: %s\n", html[0] ? html : name);
    free(r);
    return 1;
}

static int cmd_repo_download(const char *name) {
    char full[512], owner[256], repo[256], path[2048], filename[512];
    unsigned char *r = NULL; size_t n = 0; DWORD st = 0;
    FILE *f;
    if (!ensure_repo_full(name, full, sizeof(full)) || !split_owner_repo(full, owner, sizeof(owner), repo, sizeof(repo))) {
        fgh_error("repository must be owner/name or a logged-in repository name"); return 0;
    }
    _snprintf_s(path, sizeof(path), _TRUNCATE, "/repos/%s/%s/zipball", owner, repo);
    if (!api_binary_request(FGH_API_HOST, "GET", path, NULL, 0, NULL, &r, &n, &st)) { free(r); return 0; }
    if (st < 200 || st >= 300) { print_http_error(st, r); free(r); return 0; }
    _snprintf_s(filename, sizeof(filename), _TRUNCATE, "%s.zip", repo);
    f = fopen(filename, "wb");
    if (!f) { fprintf(stderr, "fgh: cannot write '%s'\n", filename); free(r); return 0; }
    if (n && fwrite(r, 1, n, f) != n) { fclose(f); free(r); return 0; }
    fclose(f);
    printf("Downloaded %s -> %s\n", full, filename);
    free(r);
    return 1;
}

static int cmd_fork(const char *repo_name, const char *author) {
    char path[2048], html[1024] = {0};
    unsigned char *r = NULL; size_t n = 0; DWORD st = 0;
    if (!repo_name || !author || !*repo_name || !*author) return 0;
    _snprintf_s(path, sizeof(path), _TRUNCATE, "/repos/%s/%s/forks", author, repo_name);
    if (!api_request("POST", path, "{}", "Content-Type: application/json\r\n", &r, &n, &st)) { free(r); return 0; }
    json_get_string((const char *)r, "html_url", html, sizeof(html));
    printf("Fork created: %s\n", html[0] ? html : "ok");
    free(r);
    return 1;
}

static int cmd_branch_create(const char *name) {
    char full[512], base[256] = {0}, sha[128] = {0}, owner[256], repo[256], ename[1024], path[2048], body[4096];
    unsigned char *r = NULL; size_t n = 0; DWORD st = 0;
    if (!g_current_repo[0]) { fgh_error("no current repository; use config set repository(\"owner/repo\")"); return 0; }
    if (!ensure_repo_full(g_current_repo, full, sizeof(full))) return 0;
    if (!split_owner_repo(full, owner, sizeof(owner), repo, sizeof(repo))) return 0;
    if (!get_repo_default_branch(full, base, sizeof(base))) {
        if (!copy_string(base, sizeof(base), g_default_branch)) return 0;
    }
    if (!get_branch_sha(full, base, sha, sizeof(sha))) return 0;
    if (!json_escape(name, ename, sizeof(ename))) return 0;
    _snprintf_s(path, sizeof(path), _TRUNCATE, "/repos/%s/%s/git/refs", owner, repo);
    _snprintf_s(body, sizeof(body), _TRUNCATE, "{\"ref\":\"refs/heads/%s\",\"sha\":\"%s\"}", ename, sha);
    if (!api_request("POST", path, body, "Content-Type: application/json\r\n", &r, &n, &st)) { free(r); return 0; }
    printf("Branch created: %s (from %s)\n", name, base);
    free(r);
    return 1;
}

static int cmd_branch_list(void) {
    char full[512], owner[256], repo[256], path[2048];
    unsigned char *r = NULL; size_t n = 0; DWORD st = 0;
    if (!g_current_repo[0]) { fgh_error("no current repository"); return 0; }
    if (!ensure_repo_full(g_current_repo, full, sizeof(full)) || !split_owner_repo(full, owner, sizeof(owner), repo, sizeof(repo))) return 0;
    _snprintf_s(path, sizeof(path), _TRUNCATE, "/repos/%s/%s/branches?per_page=100", owner, repo);
    if (!api_request("GET", path, NULL, NULL, &r, &n, &st)) { free(r); return 0; }
    printf("Branches:\n");
    {
        /* /branches itself is an array, so walk it directly. */
        const char *p = (const char *)r, *start, *end;
        int depth;
        if (*p == '[') p = json_skip_ws(p + 1);
        while (*p && *p != ']') {
            if (*p != '{') { p = json_skip_ws(p + 1); continue; }
            start = p; depth = 0;
            for (; *p; ++p) {
                if (*p == '"') { p = json_skip_string(p); if (!*p) break; --p; continue; }
                if (*p == '{') ++depth;
                else if (*p == '}') { --depth; if (depth == 0) { end = p + 1; break; } }
            }
            if (!*p) break;
            {
                char *obj = (char *)malloc((size_t)(end - start) + 1);
                char name[256] = {0};
                if (!obj) break;
                memcpy(obj, start, (size_t)(end - start)); obj[end - start] = 0;
                if (json_get_string(obj, "name", name, sizeof(name))) printf("  %s\n", name);
                free(obj);
            }
            p = json_skip_ws(end);
            if (*p == ',') p = json_skip_ws(p + 1); else break;
        }
    }
    free(r);
    return 1;
}

static int get_existing_file_sha(const char *owner, const char *repo, const char *file,
                                 const char *branch, char *sha, size_t cap) {
    char efile[4096], ebranch[2048], path[8192], tmp[256];
    unsigned char *r = NULL; size_t n = 0; DWORD st = 0;
    if (!url_encode_path(file, efile, sizeof(efile)) || !url_encode_path(branch, ebranch, sizeof(ebranch))) return 0;
    _snprintf_s(path, sizeof(path), _TRUNCATE, "/repos/%s/%s/contents/%s?ref=%s", owner, repo, efile, ebranch);
    if (!api_request("GET", path, NULL, NULL, &r, &n, &st)) { free(r); return 0; }
    if (st != 200) { free(r); return 0; }
    if (!json_get_string((const char *)r, "sha", tmp, sizeof(tmp))) { free(r); return 0; }
    if (!copy_string(sha, cap, tmp)) { free(r); return 0; }
    free(r);
    return 1;
}

static int cmd_commit(const char *repo_input, const char *branch, const char *content,
                      const char *name, int is_file) {
    char full[512], owner[256], repo[256], efile[4096], ebranch[2048];
    char existing_sha[256] = {0}, message[4096], emessage[8192], path[8192];
    unsigned char *raw = NULL; size_t raw_len = 0; char *b64 = NULL;
    char *body;
    size_t body_cap;
    unsigned char *r = NULL; size_t n = 0; DWORD st = 0;
    if (!ensure_repo_full(repo_input, full, sizeof(full)) || !split_owner_repo(full, owner, sizeof(owner), repo, sizeof(repo))) {
        fgh_error("repository must be owner/name"); return 0;
    }
    if (!branch || !*branch || !name || !*name) { fgh_error("branch/name is empty"); return 0; }
    if (is_file) {
        if (!read_file(content, &raw, &raw_len)) return 0;
    } else {
        raw_len = strlen(content ? content : "");
        raw = (unsigned char *)malloc(raw_len + 1);
        if (!raw) return 0;
        if (raw_len) memcpy(raw, content, raw_len);
        raw[raw_len] = 0;
    }
    if (!base64_encode(raw, raw_len, &b64)) { free(raw); return 0; }
    free(raw);
    if (!url_encode_path(name, efile, sizeof(efile)) || !url_encode_path(branch, ebranch, sizeof(ebranch))) { free(b64); return 0; }
    _snprintf_s(path, sizeof(path), _TRUNCATE, "/repos/%s/%s/contents/%s", owner, repo, efile);
    _snprintf_s(message, sizeof(message), _TRUNCATE, "Update %s", name);
    if (!json_escape(message, emessage, sizeof(emessage))) { free(b64); return 0; }
    get_existing_file_sha(owner, repo, name, branch, existing_sha, sizeof(existing_sha));
    body_cap = strlen(b64) + strlen(emessage) + strlen(ebranch) + strlen(existing_sha) + 256;
    body = (char *)malloc(body_cap);
    if (!body) { free(b64); return 0; }
    if (existing_sha[0]) {
        _snprintf_s(body, body_cap, _TRUNCATE,
                    "{\"message\":\"%s\",\"content\":\"%s\",\"sha\":\"%s\",\"branch\":\"%s\"}",
                    emessage, b64, existing_sha, ebranch);
    } else {
        _snprintf_s(body, body_cap, _TRUNCATE,
                    "{\"message\":\"%s\",\"content\":\"%s\",\"branch\":\"%s\"}",
                    emessage, b64, ebranch);
    }
    free(b64);
    if (!api_request("PUT", path, body, "Content-Type: application/json\r\n", &r, &n, &st)) {
        free(body); free(r); return 0;
    }
    {
        char commit_sha[256] = {0};
        json_get_string((const char *)r, "sha", commit_sha, sizeof(commit_sha));
        printf("Committed %s to %s (%s)\n", name, branch, commit_sha[0] ? commit_sha : "ok");
    }
    free(body); free(r);
    return 1;
}

static int cmd_issue_list(void) {
    char full[512], owner[256], repo[256], path[2048];
    unsigned char *r = NULL; size_t n = 0; DWORD st = 0;
    if (!g_current_repo[0]) { fgh_error("no current repository"); return 0; }
    if (!ensure_repo_full(g_current_repo, full, sizeof(full)) || !split_owner_repo(full, owner, sizeof(owner), repo, sizeof(repo))) return 0;
    _snprintf_s(path, sizeof(path), _TRUNCATE, "/repos/%s/%s/issues?state=all&per_page=100", owner, repo);
    if (!api_request("GET", path, NULL, NULL, &r, &n, &st)) { free(r); return 0; }
    printf("Issues:\n");
    {
        const char *p = (const char *)r, *start, *end;
        int depth;
        if (*p == '[') p = json_skip_ws(p + 1);
        while (*p && *p != ']') {
            char title[2048] = {0}, html[1024] = {0}, state[32] = {0};
            long number;
            if (*p != '{') { p = json_skip_ws(p + 1); continue; }
            start = p; depth = 0;
            for (; *p; ++p) {
                if (*p == '"') { p = json_skip_string(p); if (!*p) break; --p; continue; }
                if (*p == '{') ++depth;
                else if (*p == '}') { --depth; if (depth == 0) { end = p + 1; break; } }
            }
            if (!*p) break;
            {
                char *obj = (char *)malloc((size_t)(end - start) + 1);
                if (!obj) break;
                memcpy(obj, start, (size_t)(end - start)); obj[end - start] = 0;
                number = json_get_long(obj, "number", 0);
                json_get_string(obj, "title", title, sizeof(title));
                json_get_string(obj, "state", state, sizeof(state));
                json_get_string(obj, "html_url", html, sizeof(html));
                /* Pull requests are also returned by /issues; exclude them. */
                {
                    const char *pr_value = NULL, *pr_end = NULL;
                    int is_pr = json_get_key_value_range(obj, "pull_request", &pr_value, &pr_end) &&
                                pr_value && pr_end > pr_value && *pr_value == '{';
                    if (!is_pr)
                        printf("  #%ld [%s] %s%s%s\n", number, state, title, html[0] ? " - " : "", html);
                }
                free(obj);
            }
            p = json_skip_ws(end);
            if (*p == ',') p = json_skip_ws(p + 1); else break;
        }
    }
    free(r);
    return 1;
}

static int cmd_issue_create(const char *title, const char *body_text) {
    char full[512], owner[256], repo[256], path[2048], et[8192], eb[FGH_MAX_TEXT], body[FGH_MAX_TEXT + 12288];
    unsigned char *r = NULL; size_t n = 0; DWORD st = 0;
    if (!g_current_repo[0]) { fgh_error("no current repository"); return 0; }
    if (!ensure_repo_full(g_current_repo, full, sizeof(full)) || !split_owner_repo(full, owner, sizeof(owner), repo, sizeof(repo))) return 0;
    if (!json_escape(title, et, sizeof(et)) || !json_escape(body_text, eb, sizeof(eb))) return 0;
    _snprintf_s(path, sizeof(path), _TRUNCATE, "/repos/%s/%s/issues", owner, repo);
    _snprintf_s(body, sizeof(body), _TRUNCATE, "{\"title\":\"%s\",\"body\":\"%s\"}", et, eb);
    if (!api_request("POST", path, body, "Content-Type: application/json\r\n", &r, &n, &st)) { free(r); return 0; }
    printf("Issue created: #%ld\n", json_get_long((const char *)r, "number", 0));
    free(r);
    return 1;
}

static int cmd_issue_view(long number) {
    char full[512], owner[256], repo[256], path[2048], title[2048] = {0}, body_text[FGH_MAX_TEXT] = {0}, state[32] = {0}, html[1024] = {0};
    unsigned char *r = NULL; size_t n = 0; DWORD st = 0;
    if (!g_current_repo[0]) { fgh_error("no current repository"); return 0; }
    if (!ensure_repo_full(g_current_repo, full, sizeof(full)) || !split_owner_repo(full, owner, sizeof(owner), repo, sizeof(repo))) return 0;
    _snprintf_s(path, sizeof(path), _TRUNCATE, "/repos/%s/%s/issues/%ld", owner, repo, number);
    if (!api_request("GET", path, NULL, NULL, &r, &n, &st)) { free(r); return 0; }
    json_get_string((const char *)r, "title", title, sizeof(title));
    json_get_string((const char *)r, "body", body_text, sizeof(body_text));
    json_get_string((const char *)r, "state", state, sizeof(state));
    json_get_string((const char *)r, "html_url", html, sizeof(html));
    printf("#%ld %s\nstate: %s\n", number, title, state);
    if (body_text[0]) printf("%s\n", body_text);
    if (html[0]) printf("%s\n", html);
    free(r);
    return 1;
}

static int cmd_pr_create(const char *title, const char *head, const char *base) {
    char full[512], owner[256], repo[256], path[2048], et[8192], eh[4096], eb[4096], body[16384];
    unsigned char *r = NULL; size_t n = 0; DWORD st = 0;
    if (!g_current_repo[0]) { fgh_error("no current repository"); return 0; }
    if (!ensure_repo_full(g_current_repo, full, sizeof(full)) || !split_owner_repo(full, owner, sizeof(owner), repo, sizeof(repo))) return 0;
    if (!json_escape(title, et, sizeof(et)) || !json_escape(head, eh, sizeof(eh)) || !json_escape(base, eb, sizeof(eb))) return 0;
    _snprintf_s(path, sizeof(path), _TRUNCATE, "/repos/%s/%s/pulls", owner, repo);
    _snprintf_s(body, sizeof(body), _TRUNCATE, "{\"title\":\"%s\",\"head\":\"%s\",\"base\":\"%s\"}", et, eh, eb);
    if (!api_request("POST", path, body, "Content-Type: application/json\r\n", &r, &n, &st)) { free(r); return 0; }
    {
        char html[1024] = {0};
        json_get_string((const char *)r, "html_url", html, sizeof(html));
        printf("PR created: #%ld%s%s\n", json_get_long((const char *)r, "number", 0), html[0] ? " " : "", html);
    }
    free(r);
    return 1;
}

static int cmd_pr_list(void) {
    char full[512], owner[256], repo[256], path[2048];
    unsigned char *r = NULL; size_t n = 0; DWORD st = 0;
    if (!g_current_repo[0]) { fgh_error("no current repository"); return 0; }
    if (!ensure_repo_full(g_current_repo, full, sizeof(full)) || !split_owner_repo(full, owner, sizeof(owner), repo, sizeof(repo))) return 0;
    _snprintf_s(path, sizeof(path), _TRUNCATE, "/repos/%s/%s/pulls?state=all&per_page=100", owner, repo);
    if (!api_request("GET", path, NULL, NULL, &r, &n, &st)) { free(r); return 0; }
    printf("Pull requests:\n");
    {
        const char *p = (const char *)r, *start, *end;
        int depth;
        if (*p == '[') p = json_skip_ws(p + 1);
        while (*p && *p != ']') {
            char title[2048] = {0}, html[1024] = {0}, state[32] = {0};
            long number;
            if (*p != '{') { p = json_skip_ws(p + 1); continue; }
            start = p; depth = 0;
            for (; *p; ++p) {
                if (*p == '"') { p = json_skip_string(p); if (!*p) break; --p; continue; }
                if (*p == '{') ++depth;
                else if (*p == '}') { --depth; if (depth == 0) { end = p + 1; break; } }
            }
            if (!*p) break;
            {
                char *obj = (char *)malloc((size_t)(end - start) + 1);
                if (!obj) break;
                memcpy(obj, start, (size_t)(end - start)); obj[end - start] = 0;
                number = json_get_long(obj, "number", 0);
                json_get_string(obj, "title", title, sizeof(title));
                json_get_string(obj, "state", state, sizeof(state));
                json_get_string(obj, "html_url", html, sizeof(html));
                printf("  #%ld [%s] %s%s%s\n", number, state, title, html[0] ? " - " : "", html);
                free(obj);
            }
            p = json_skip_ws(end);
            if (*p == ',') p = json_skip_ws(p + 1); else break;
        }
    }
    free(r);
    return 1;
}

static int cmd_pr_merge(long number) {
    char full[512], owner[256], repo[256], path[2048];
    unsigned char *r = NULL; size_t n = 0; DWORD st = 0;
    int merged;
    char message[2048] = {0}, sha[256] = {0};
    if (!g_current_repo[0]) { fgh_error("no current repository"); return 0; }
    if (!ensure_repo_full(g_current_repo, full, sizeof(full)) || !split_owner_repo(full, owner, sizeof(owner), repo, sizeof(repo))) return 0;
    _snprintf_s(path, sizeof(path), _TRUNCATE, "/repos/%s/%s/pulls/%ld/merge", owner, repo, number);
    if (!api_request("PUT", path, "{}", "Content-Type: application/json\r\n", &r, &n, &st)) { free(r); return 0; }
    merged = json_get_bool((const char *)r, "merged", 0);
    json_get_string((const char *)r, "message", message, sizeof(message));
    json_get_string((const char *)r, "sha", sha, sizeof(sha));
    printf("PR #%ld: %s%s%s\n", number, merged ? "merged" : "not merged", message[0] ? " - " : "", message);
    if (sha[0]) printf("sha: %s\n", sha);
    free(r);
    return merged ? 1 : 0;
}

static int cmd_release_create(const char *tag, const char *title, const char *content_path) {
    char full[512], owner[256], repo[256], path[2048], et[2048], ename[4096], body[8192];
    unsigned char *r = NULL; size_t n = 0; DWORD st = 0;
    if (!g_current_repo[0]) { fgh_error("no current repository"); return 0; }
    if (!ensure_repo_full(g_current_repo, full, sizeof(full)) || !split_owner_repo(full, owner, sizeof(owner), repo, sizeof(repo))) return 0;
    if (!json_escape(tag, et, sizeof(et)) || !json_escape(title, ename, sizeof(ename))) return 0;
    _snprintf_s(path, sizeof(path), _TRUNCATE, "/repos/%s/%s/releases", owner, repo);
    _snprintf_s(body, sizeof(body), _TRUNCATE,
                "{\"tag_name\":\"%s\",\"name\":\"%s\",\"draft\":false,\"prerelease\":false}", et, ename);
    if (!api_request("POST", path, body, "Content-Type: application/json\r\n", &r, &n, &st)) { free(r); return 0; }
    {
        long release_id = json_get_long((const char *)r, "id", 0);
        if (content_path && *content_path) {
            unsigned char *file = NULL, *ur = NULL; size_t flen = 0, un = 0; DWORD ust = 0;
            char asset_name[512], encoded_name[2048], upload_path[4096];
            const char *s1 = strrchr(content_path, '\\');
            const char *s2 = strrchr(content_path, '/');
            const char *base = (s1 && s2) ? ((s1 > s2) ? s1 + 1 : s2 + 1) : (s1 ? s1 + 1 : (s2 ? s2 + 1 : content_path));
            if (!copy_string(asset_name, sizeof(asset_name), base)) { free(r); fgh_error("release asset filename is too long"); return 0; }
            if (!read_file(content_path, &file, &flen)) { free(r); return 0; }
            if (!url_encode(asset_name, encoded_name, sizeof(encoded_name))) { free(file); free(r); return 0; }
            _snprintf_s(upload_path, sizeof(upload_path), _TRUNCATE,
                        "/repos/%s/%s/releases/%ld/assets?name=%s", owner, repo, release_id, encoded_name);
            if (!api_binary_request(FGH_UPLOAD_HOST, "POST", upload_path, file, flen,
                                    "Content-Type: application/octet-stream\r\n", &ur, &un, &ust)) {
                free(file); free(r); return 0;
            }
            if (ust < 200 || ust >= 300) { print_http_error(ust, ur); free(ur); free(file); free(r); return 0; }
            printf("Release created and asset uploaded: %s\n", asset_name);
            free(ur); free(file);
        } else {
            printf("Release created: id=%ld\n", release_id);
        }
    }
    free(r);
    return 1;
}

static int print_search_repo_object(const char *obj, size_t len, void *ctx) {
    char full[512] = {0}, desc[2048] = {0}, html[1024] = {0};
    char *tmp = (char *)malloc(len + 1);
    (void)ctx;
    if (!tmp) return 0;
    memcpy(tmp, obj, len); tmp[len] = 0;
    json_get_string(tmp, "full_name", full, sizeof(full));
    json_get_string(tmp, "description", desc, sizeof(desc));
    json_get_string(tmp, "html_url", html, sizeof(html));
    printf("  %s\n", full);
    if (desc[0]) printf("    %s\n", desc);
    if (html[0]) printf("    %s\n", html);
    free(tmp);
    return 1;
}

static int print_search_user_object(const char *obj, size_t len, void *ctx) {
    char login[256] = {0}, html[1024] = {0};
    char *tmp = (char *)malloc(len + 1);
    (void)ctx;
    if (!tmp) return 0;
    memcpy(tmp, obj, len); tmp[len] = 0;
    json_get_string(tmp, "login", login, sizeof(login));
    json_get_string(tmp, "html_url", html, sizeof(html));
    printf("  %s%s%s\n", login, html[0] ? " - " : "", html);
    free(tmp);
    return 1;
}

static int print_search_issue_object(const char *obj, size_t len, void *ctx) {
    char title[2048] = {0}, html[1024] = {0};
    char *tmp = (char *)malloc(len + 1);
    (void)ctx;
    if (!tmp) return 0;
    memcpy(tmp, obj, len); tmp[len] = 0;
    json_get_long(tmp, "number", 0);
    json_get_string(tmp, "title", title, sizeof(title));
    json_get_string(tmp, "html_url", html, sizeof(html));
    printf("  #%ld %s%s%s\n", json_get_long(tmp, "number", 0), title, html[0] ? " - " : "", html);
    free(tmp);
    return 1;
}

static int cmd_search_repository(const char *query) {
    char q[8192], path[12288]; unsigned char *r = NULL; size_t n = 0; DWORD st = 0;
    if (!url_encode(query, q, sizeof(q))) return 0;
    _snprintf_s(path, sizeof(path), _TRUNCATE, "/search/repositories?q=%s&per_page=20", q);
    if (!api_request("GET", path, NULL, NULL, &r, &n, &st)) { free(r); return 0; }
    printf("Repository search: %s\n", query);
    json_for_each_object((const char *)r, "items", print_search_repo_object, NULL);
    free(r); return 1;
}

static int cmd_search_user(const char *query) {
    char q[8192], path[12288]; unsigned char *r = NULL; size_t n = 0; DWORD st = 0;
    if (!url_encode(query, q, sizeof(q))) return 0;
    _snprintf_s(path, sizeof(path), _TRUNCATE, "/search/users?q=%s&per_page=20", q);
    if (!api_request("GET", path, NULL, NULL, &r, &n, &st)) { free(r); return 0; }
    printf("User search: %s\n", query);
    json_for_each_object((const char *)r, "items", print_search_user_object, NULL);
    free(r); return 1;
}

static int cmd_search_issue_or_pr(const char *kind, const char *query) {
    char raw[8192], q[12288], path[16384]; unsigned char *r = NULL; size_t n = 0; DWORD st = 0;
    const char *prefix = str_ieq(kind, "pr") ? "is:pr " : "is:issue ";
    _snprintf_s(raw, sizeof(raw), _TRUNCATE, "%s%s", prefix, query);
    if (!url_encode(raw, q, sizeof(q))) return 0;
    _snprintf_s(path, sizeof(path), _TRUNCATE, "/search/issues?q=%s&per_page=20", q);
    if (!api_request("GET", path, NULL, NULL, &r, &n, &st)) { free(r); return 0; }
    printf("%s search: %s\n", str_ieq(kind, "pr") ? "PR" : "Issue", query);
    json_for_each_object((const char *)r, "items", print_search_issue_object, NULL);
    free(r); return 1;
}

static int cmd_search(const char *kind, const char *query) {
    if (str_ieq(kind, "repository")) return cmd_search_repository(query);
    if (str_ieq(kind, "user")) return cmd_search_user(query);
    if (str_ieq(kind, "issue") || str_ieq(kind, "pr")) return cmd_search_issue_or_pr(kind, query);
    fgh_error("unknown search type");
    return 0;
}

static int cmd_user_info(const char *login) {
    char e[1024], path[2048]; unsigned char *r = NULL; size_t n = 0; DWORD st = 0;
    if (!url_encode(login, e, sizeof(e))) return 0;
    _snprintf_s(path, sizeof(path), _TRUNCATE, "/users/%s", e);
    if (!api_request("GET", path, NULL, NULL, &r, &n, &st)) { free(r); return 0; }
    print_user_summary((const char *)r);
    free(r); return 1;
}

static int print_repo_user_object(const char *obj, size_t len, void *ctx) {
    char full[512] = {0}, html[1024] = {0};
    char *tmp = (char *)malloc(len + 1);
    (void)ctx;
    if (!tmp) return 0;
    memcpy(tmp, obj, len); tmp[len] = 0;
    json_get_string(tmp, "full_name", full, sizeof(full));
    json_get_string(tmp, "html_url", html, sizeof(html));
    printf("  %s%s%s\n", full, html[0] ? " - " : "", html);
    free(tmp);
    return 1;
}

static int print_login_object(const char *obj, size_t len, void *ctx) {
    char login[256] = {0};
    char *tmp = (char *)malloc(len + 1);
    (void)ctx;
    if (!tmp) return 0;
    memcpy(tmp, obj, len); tmp[len] = 0;
    json_get_string(tmp, "login", login, sizeof(login));
    printf("  %s\n", login);
    free(tmp);
    return 1;
}

static int cmd_user_repositories(const char *login) {
    char e[1024], path[2048]; unsigned char *r = NULL; size_t n = 0; DWORD st = 0;
    if (!url_encode(login, e, sizeof(e))) return 0;
    _snprintf_s(path, sizeof(path), _TRUNCATE, "/users/%s/repos?per_page=100&sort=updated", e);
    if (!api_request("GET", path, NULL, NULL, &r, &n, &st)) { free(r); return 0; }
    printf("Repositories of %s:\n", login);
    /* /users/{user}/repos is a direct array. */
    {
        const char *p = (const char *)r, *start, *end; int depth;
        if (*p == '[') p = json_skip_ws(p + 1);
        while (*p && *p != ']') {
            if (*p != '{') { p = json_skip_ws(p + 1); continue; }
            start = p; depth = 0;
            for (; *p; ++p) {
                if (*p == '"') { p = json_skip_string(p); if (!*p) break; --p; continue; }
                if (*p == '{') ++depth;
                else if (*p == '}') { --depth; if (depth == 0) { end = p + 1; break; } }
            }
            if (!*p) break;
            print_repo_user_object(start, (size_t)(end - start), NULL);
            p = json_skip_ws(end);
            if (*p == ',') p = json_skip_ws(p + 1); else break;
        }
    }
    free(r); return 1;
}

static int cmd_user_follow(const char *login, int following) {
    char e[1024], path[2048]; unsigned char *r = NULL; size_t n = 0; DWORD st = 0;
    if (!url_encode(login, e, sizeof(e))) return 0;
    _snprintf_s(path, sizeof(path), _TRUNCATE, "/users/%s/%s?per_page=100", e, following ? "following" : "followers");
    if (!api_request("GET", path, NULL, NULL, &r, &n, &st)) { free(r); return 0; }
    printf("%s of %s:\n", following ? "Following" : "Followers", login);
    {
        const char *p = (const char *)r, *start, *end; int depth;
        if (*p == '[') p = json_skip_ws(p + 1);
        while (*p && *p != ']') {
            if (*p != '{') { p = json_skip_ws(p + 1); continue; }
            start = p; depth = 0;
            for (; *p; ++p) {
                if (*p == '"') { p = json_skip_string(p); if (!*p) break; --p; continue; }
                if (*p == '{') ++depth;
                else if (*p == '}') { --depth; if (depth == 0) { end = p + 1; break; } }
            }
            if (!*p) break;
            print_login_object(start, (size_t)(end - start), NULL);
            p = json_skip_ws(end);
            if (*p == ',') p = json_skip_ws(p + 1); else break;
        }
    }
    free(r); return 1;
}

static int cmd_config_set(const char *key, const char *value) {
    int ok = 0;
    if (str_ieq(key, "default-branch")) ok = copy_string(g_default_branch, sizeof(g_default_branch), value);
    else if (str_ieq(key, "editor")) ok = copy_string(g_editor, sizeof(g_editor), value);
    else if (str_ieq(key, "repository")) ok = copy_string(g_current_repo, sizeof(g_current_repo), value);
    else { fgh_error("unknown config key"); return 0; }
    if (!ok) { fgh_error("config value is too long"); return 0; }
    if (!save_config()) { fgh_error("could not save config"); return 0; }
    printf("%s = %s\n", key, value);
    return 1;
}

static int cmd_config_get(const char *key) {
    if (str_ieq(key, "default-branch")) printf("%s\n", g_default_branch);
    else if (str_ieq(key, "editor")) printf("%s\n", g_editor);
    else if (str_ieq(key, "repository")) printf("%s\n", g_current_repo);
    else { fgh_error("unknown config key"); return 0; }
    return 1;
}

static void usage(void) {
    printf("Fgithub CLI %s\n", FGH_VERSION);
    printf("\nAuthentication\n");
    printf("  login name(\"USER\") password(\"TOKEN\")\n");
    printf("  login status\n");
    printf("  logout\n\n");
    printf("Repository\n");
    printf("  New repository Name(\"NAME\") Accessibility(\"Public|private\")\n");
    printf("  repository download(\"OWNER/REPO\")\n");
    printf("  fork repository(\"REPO\") original author(\"USER\")\n\n");
    printf("Code\n");
    printf("  commit Repository(\"OWNER/REPO\") Branch(\"BRANCH\") Content(\"TEXT\") Name(\"FILE\")\n");
    printf("  commit Repository(\"OWNER/REPO\") Branch(\"BRANCH\") Content([PATH]) Name(\"FILE\")\n");
    printf("  branch create(\"NAME\")\n");
    printf("  branch list\n\n");
    printf("Issues\n");
    printf("  github issue list\n");
    printf("  github issue create Title(\"TITLE\") body(\"BODY\")\n");
    printf("  github issue view (NUMBER)\n\n");
    printf("Pull requests\n");
    printf("  github pr create Title(\"TITLE\") branch(\"HEAD\") base(\"BASE\")\n");
    printf("  github pr list\n");
    printf("  github pr merge (NUMBER)\n\n");
    printf("Release\n");
    printf("  release create Tag(\"TAG\") Title(\"TITLE\") Content(\"PATH\")\n\n");
    printf("Search\n");
    printf("  github search repository(\"QUERY\")\n");
    printf("  github search user(\"QUERY\")\n");
    printf("  github search issue(\"QUERY\")\n");
    printf("  github search pr(\"QUERY\")\n\n");
    printf("User\n");
    printf("  github user(\"LOGIN\")\n");
    printf("  github user repositories(\"LOGIN\")\n");
    printf("  github user followers(\"LOGIN\")\n");
    printf("  github user following(\"LOGIN\")\n\n");
    printf("Config\n");
    printf("  config set default-branch(\"main\")\n");
    printf("  config set editor(\"code\")\n");
    printf("  config set repository(\"OWNER/REPO\")\n");
    printf("  config get default-branch|editor|repository\n\n");
    printf("Other\n  help\n  version\n  exit\n");
}

typedef enum { TK_WORD, TK_STRING, TK_BRACKET, TK_LPAREN, TK_RPAREN, TK_END } TokType;
typedef struct { TokType type; char *text; } Token;

static void tokens_free(Token *tokens, int n) {
    int i;
    if (!tokens) return;
    for (i = 0; i < n; ++i) free(tokens[i].text);
    memset(tokens, 0, (size_t)n * sizeof(*tokens));
}

static int token_append_char(char **buf, size_t *len, size_t *cap, unsigned char c) {
    size_t need = *len + 2;
    if (need > *cap) {
        size_t nc = *cap ? *cap : 64;
        while (nc < need) {
            if (nc > FGH_MAX_CMD / 2) return 0;
            nc *= 2;
        }
        if (nc > FGH_MAX_CMD) return 0;
        {
            char *nb = (char *)realloc(*buf, nc);
            if (!nb) return 0;
            *buf = nb; *cap = nc;
        }
    }
    (*buf)[(*len)++] = (char)c;
    (*buf)[*len] = 0;
    return 1;
}

static int lex_collect(Token *tok, const char *begin, const char *end, TokType type) {
    size_t len = (size_t)(end - begin);
    tok->text = (char *)malloc(len + 1);
    if (!tok->text) return 0;
    memcpy(tok->text, begin, len);
    tok->text[len] = 0;
    tok->type = type;
    return 1;
}

static int lex_line(const char *line, Token *tokens, int max_tokens) {
    const char *p = line;
    int n = 0;
    if (!line || !tokens || max_tokens < 2) return -1;
    memset(tokens, 0, (size_t)max_tokens * sizeof(*tokens));
    while (*p) {
        char *buf = NULL;
        size_t len = 0, cap = 0;
        while (isspace((unsigned char)*p)) ++p;
        if (!*p) break;
        if (n >= max_tokens - 1) { tokens_free(tokens, n); return -2; }
        if (*p == '(') {
            if (!lex_collect(&tokens[n], p, p + 1, TK_LPAREN)) { tokens_free(tokens, n); return -3; }
            ++n; ++p; continue;
        }
        if (*p == ')') {
            if (!lex_collect(&tokens[n], p, p + 1, TK_RPAREN)) { tokens_free(tokens, n); return -3; }
            ++n; ++p; continue;
        }
        if (*p == '"') {
            ++p;
            while (*p && *p != '"') {
                if (*p == '\\') {
                    unsigned char e;
                    ++p;
                    if (!*p) { free(buf); tokens_free(tokens, n); return -4; }
                    e = (unsigned char)*p++;
                    if (e == 'n') e = '\n';
                    else if (e == 'r') e = '\r';
                    else if (e == 't') e = '\t';
                    else if (e == 'b') e = '\b';
                    else if (e == 'f') e = '\f';
                    else if (e != '"' && e != '\\' && e != '/') {
                        /* Keep unknown escapes literal instead of silently deleting data. */
                        if (!token_append_char(&buf, &len, &cap, '\\')) { free(buf); tokens_free(tokens, n); return -3; }
                    }
                    if (!token_append_char(&buf, &len, &cap, e)) { free(buf); tokens_free(tokens, n); return -3; }
                } else {
                    if (!token_append_char(&buf, &len, &cap, (unsigned char)*p++)) { free(buf); tokens_free(tokens, n); return -3; }
                }
            }
            if (*p != '"') { free(buf); tokens_free(tokens, n); return -4; }
            ++p;
            tokens[n].type = TK_STRING;
            tokens[n].text = buf ? buf : dup_string("");
            if (!tokens[n].text) { free(buf); tokens_free(tokens, n); return -3; }
            ++n; continue;
        }
        if (*p == '[') {
            ++p;
            while (isspace((unsigned char)*p)) ++p;
            while (*p && *p != ']') {
                if (!token_append_char(&buf, &len, &cap, (unsigned char)*p++)) { free(buf); tokens_free(tokens, n); return -3; }
            }
            if (*p != ']') { free(buf); tokens_free(tokens, n); return -4; }
            ++p;
            while (len && isspace((unsigned char)buf[len - 1])) buf[--len] = 0;
            tokens[n].type = TK_BRACKET;
            tokens[n].text = buf ? buf : dup_string("");
            if (!tokens[n].text) { free(buf); tokens_free(tokens, n); return -3; }
            ++n; continue;
        }
        {
            const char *start_word = p;
            while (*p && !isspace((unsigned char)*p) && *p != '(' && *p != ')' && *p != '[' && *p != ']') ++p;
            if (p == start_word) { tokens_free(tokens, n); return -5; }
            if (!lex_collect(&tokens[n], start_word, p, TK_WORD)) { tokens_free(tokens, n); return -3; }
            ++n;
        }
    }
    tokens[n].type = TK_END;
    tokens[n].text = dup_string("");
    if (!tokens[n].text) { tokens_free(tokens, n); return -3; }
    return n;
}

static int tok_word(const Token *t, const char *s) { return t && t->type == TK_WORD && str_ieq(t->text, s); }
static int tok_value(const Token *t) { return t && (t->type == TK_STRING || t->type == TK_BRACKET || t->type == TK_WORD); }

static int read_param(Token *t, int *i, int nt, char *out, size_t cap, int *is_file) {
    if (!t || !i || *i < 0 || *i + 3 >= nt || !out || cap == 0) return 0;
    if (t[*i].type != TK_WORD || t[*i + 1].type != TK_LPAREN || !tok_value(&t[*i + 2]) || t[*i + 3].type != TK_RPAREN) return 0;
    if (!copy_string(out, cap, t[*i + 2].text)) return 0;
    if (is_file) *is_file = (t[*i + 2].type == TK_BRACKET);
    *i += 4;
    return 1;
}

static int read_call(Token *t, int *i, int nt, char *out, size_t cap, int *is_file) {
    if (!t || !i || *i < 0 || *i + 2 >= nt || !out || cap == 0) return 0;
    if (t[*i].type != TK_LPAREN || !tok_value(&t[*i + 1]) || t[*i + 2].type != TK_RPAREN) return 0;
    if (!copy_string(out, cap, t[*i + 1].text)) return 0;
    if (is_file) *is_file = (t[*i + 1].type == TK_BRACKET);
    *i += 3;
    return 1;
}

static int read_number(Token *t, int index, long *out) {
    char *end;
    long n;
    if (!t || !out || index < 0) return 0;
    if (t[index].type == TK_LPAREN) {
        if (!t[index + 1].text || t[index + 1].type != TK_WORD || t[index + 2].type != TK_RPAREN) return 0;
        errno = 0;
        n = strtol(t[index + 1].text, &end, 10);
        if (errno == ERANGE || *end) return 0;
    } else if (t[index].type == TK_WORD) {
        errno = 0;
        n = strtol(t[index].text, &end, 10);
        if (errno == ERANGE || *end) return 0;
    } else return 0;
    *out = n;
    return 1;
}

static int parse_fail(const char *usage_text) {
    fprintf(stderr, "fgh: invalid syntax. %s\n", usage_text);
    return 0;
}

static int parse_and_run(const char *line) {
    Token t[FGH_MAX_TOKENS];
    int nt, i;
    char a[FGH_MAX_TEXT], b[FGH_MAX_TEXT], c[FGH_MAX_TEXT], d[FGH_MAX_TEXT];
    int file_flag = 0;
    long number;
    int rc = 0;

    memset(t, 0, sizeof(t));
    nt = lex_line(line, t, FGH_MAX_TOKENS);
    if (nt == 0) { tokens_free(t, 1); return 1; }
    if (nt < 0) {
        if (nt == -2) fgh_error("too many command tokens");
        else if (nt == -4) fgh_error("unterminated quoted string or [file path]");
        else if (nt == -5) fgh_error("unexpected ] in command");
        else fgh_error("invalid command syntax or out of memory");
        tokens_free(t, FGH_MAX_TOKENS);
        return 0;
    }

    if (tok_word(&t[0], "help")) { usage(); rc = 1; goto done; }
    if (tok_word(&t[0], "version")) { printf("Fgithub CLI %s\n", FGH_VERSION); rc = 1; goto done; }

    if (tok_word(&t[0], "login")) {
        if (tok_word(&t[1], "status") && t[2].type == TK_END) { rc = cmd_status(); goto done; }
        i = 1;
        if (!read_param(t, &i, nt, a, sizeof(a), NULL) || !tok_word(&t[i], "password")) {
            parse_fail("login name(\"USER\") password(\"TOKEN\")"); goto done;
        }
        ++i;
        if (!read_param(t, &i, nt, b, sizeof(b), NULL) || t[i].type != TK_END) {
            parse_fail("login name(\"USER\") password(\"TOKEN\")"); goto done;
        }
        rc = cmd_login(a, b); goto done;
    }

    if (tok_word(&t[0], "logout")) {
        if (t[1].type != TK_END) { parse_fail("logout"); goto done; }
        clear_config(); printf("Logged out.\n"); rc = 1; goto done;
    }

    if (tok_word(&t[0], "New") && tok_word(&t[1], "repository")) {
        i = 2;
        if (!tok_word(&t[i], "Name")) { parse_fail("New repository Name(\"NAME\") Accessibility(\"Public|private\")"); goto done; }
        ++i;
        if (!read_param(t, &i, nt, a, sizeof(a), NULL)) { parse_fail("New repository Name(\"NAME\") Accessibility(\"Public|private\")"); goto done; }
        if (!tok_word(&t[i], "Accessibility")) { parse_fail("New repository Name(\"NAME\") Accessibility(\"Public|private\")"); goto done; }
        ++i;
        if (!read_param(t, &i, nt, b, sizeof(b), NULL) || t[i].type != TK_END) { parse_fail("New repository Name(\"NAME\") Accessibility(\"Public|private\")"); goto done; }
        rc = cmd_new_repo(a, b); goto done;
    }

    if (tok_word(&t[0], "repository") && tok_word(&t[1], "download")) {
        i = 2;
        if (!read_call(t, &i, nt, a, sizeof(a), NULL) || t[i].type != TK_END) { parse_fail("repository download(\"OWNER/REPO\")"); goto done; }
        rc = cmd_repo_download(a); goto done;
    }

    if (tok_word(&t[0], "fork") && tok_word(&t[1], "repository")) {
        i = 2;
        if (!read_call(t, &i, nt, a, sizeof(a), NULL)) { parse_fail("fork repository(\"REPO\") original author(\"USER\")"); goto done; }
        if (!tok_word(&t[i], "original") || !tok_word(&t[i + 1], "author")) { parse_fail("fork repository(\"REPO\") original author(\"USER\")"); goto done; }
        i += 2;
        if (!read_param(t, &i, nt, b, sizeof(b), NULL) || t[i].type != TK_END) { parse_fail("fork repository(\"REPO\") original author(\"USER\")"); goto done; }
        rc = cmd_fork(a, b); goto done;
    }

    if (tok_word(&t[0], "commit")) {
        i = 1;
        if (!tok_word(&t[i], "Repository")) { parse_fail("commit Repository(\"OWNER/REPO\") Branch(\"BRANCH\") Content(\"TEXT\"|[PATH]) Name(\"FILE\")"); goto done; }
        ++i; if (!read_param(t, &i, nt, a, sizeof(a), NULL)) { parse_fail("commit Repository(\"OWNER/REPO\") Branch(\"BRANCH\") Content(\"TEXT\"|[PATH]) Name(\"FILE\")"); goto done; }
        if (!tok_word(&t[i], "Branch")) { parse_fail("commit Repository(\"OWNER/REPO\") Branch(\"BRANCH\") Content(\"TEXT\"|[PATH]) Name(\"FILE\")"); goto done; }
        ++i; if (!read_param(t, &i, nt, b, sizeof(b), NULL)) { parse_fail("commit Repository(\"OWNER/REPO\") Branch(\"BRANCH\") Content(\"TEXT\"|[PATH]) Name(\"FILE\")"); goto done; }
        if (!tok_word(&t[i], "Content")) { parse_fail("commit Repository(\"OWNER/REPO\") Branch(\"BRANCH\") Content(\"TEXT\"|[PATH]) Name(\"FILE\")"); goto done; }
        ++i; if (!read_param(t, &i, nt, c, sizeof(c), &file_flag)) { parse_fail("commit Repository(\"OWNER/REPO\") Branch(\"BRANCH\") Content(\"TEXT\"|[PATH]) Name(\"FILE\")"); goto done; }
        if (!tok_word(&t[i], "Name")) { parse_fail("commit Repository(\"OWNER/REPO\") Branch(\"BRANCH\") Content(\"TEXT\"|[PATH]) Name(\"FILE\")"); goto done; }
        ++i; if (!read_param(t, &i, nt, d, sizeof(d), NULL) || t[i].type != TK_END) { parse_fail("commit Repository(\"OWNER/REPO\") Branch(\"BRANCH\") Content(\"TEXT\"|[PATH]) Name(\"FILE\")"); goto done; }
        rc = cmd_commit(a, b, c, d, file_flag); goto done;
    }

    if (tok_word(&t[0], "branch")) {
        if (tok_word(&t[1], "create")) {
            i = 2; if (!read_call(t, &i, nt, a, sizeof(a), NULL) || t[i].type != TK_END) { parse_fail("branch create(\"NAME\")"); goto done; }
            rc = cmd_branch_create(a); goto done;
        }
        if (tok_word(&t[1], "list") && t[2].type == TK_END) { rc = cmd_branch_list(); goto done; }
    }

    if (tok_word(&t[0], "github") && tok_word(&t[1], "issue")) {
        if (tok_word(&t[2], "list") && t[3].type == TK_END) { rc = cmd_issue_list(); goto done; }
        if (tok_word(&t[2], "create")) {
            i = 3;
            if (!tok_word(&t[i], "Title")) { parse_fail("github issue create Title(\"TITLE\") body(\"BODY\")"); goto done; }
            ++i; if (!read_param(t, &i, nt, a, sizeof(a), NULL)) { parse_fail("github issue create Title(\"TITLE\") body(\"BODY\")"); goto done; }
            if (!tok_word(&t[i], "body")) { parse_fail("github issue create Title(\"TITLE\") body(\"BODY\")"); goto done; }
            ++i; if (!read_param(t, &i, nt, b, sizeof(b), NULL) || t[i].type != TK_END) { parse_fail("github issue create Title(\"TITLE\") body(\"BODY\")"); goto done; }
            rc = cmd_issue_create(a, b); goto done;
        }
        if (tok_word(&t[2], "view")) {
            if (!read_number(t, 3, &number) || ((t[3].type == TK_LPAREN) ? t[6].type != TK_END : t[4].type != TK_END)) { parse_fail("github issue view (NUMBER)"); goto done; }
            rc = cmd_issue_view(number); goto done;
        }
    }

    if (tok_word(&t[0], "github") && tok_word(&t[1], "pr")) {
        if (tok_word(&t[2], "create")) {
            i = 3;
            if (!tok_word(&t[i], "Title")) { parse_fail("github pr create Title(\"TITLE\") branch(\"HEAD\") base(\"BASE\")"); goto done; }
            ++i; if (!read_param(t, &i, nt, a, sizeof(a), NULL)) { parse_fail("github pr create Title(\"TITLE\") branch(\"HEAD\") base(\"BASE\")"); goto done; }
            if (!tok_word(&t[i], "branch")) { parse_fail("github pr create Title(\"TITLE\") branch(\"HEAD\") base(\"BASE\")"); goto done; }
            ++i; if (!read_param(t, &i, nt, b, sizeof(b), NULL)) { parse_fail("github pr create Title(\"TITLE\") branch(\"HEAD\") base(\"BASE\")"); goto done; }
            if (!tok_word(&t[i], "base")) { parse_fail("github pr create Title(\"TITLE\") branch(\"HEAD\") base(\"BASE\")"); goto done; }
            ++i; if (!read_param(t, &i, nt, c, sizeof(c), NULL) || t[i].type != TK_END) { parse_fail("github pr create Title(\"TITLE\") branch(\"HEAD\") base(\"BASE\")"); goto done; }
            rc = cmd_pr_create(a, b, c); goto done;
        }
        if (tok_word(&t[2], "list") && t[3].type == TK_END) { rc = cmd_pr_list(); goto done; }
        if (tok_word(&t[2], "merge")) {
            if (!read_number(t, 3, &number)) { parse_fail("github pr merge (NUMBER)"); goto done; }
            i = (t[3].type == TK_LPAREN) ? 6 : 4;
            if (t[i].type != TK_END) { parse_fail("github pr merge (NUMBER)"); goto done; }
            rc = cmd_pr_merge(number); goto done;
        }
    }

    if (tok_word(&t[0], "release") && tok_word(&t[1], "create")) {
        i = 2;
        if (!tok_word(&t[i], "Tag")) { parse_fail("release create Tag(\"TAG\") Title(\"TITLE\") Content(\"PATH\")"); goto done; }
        ++i; if (!read_param(t, &i, nt, a, sizeof(a), NULL)) { parse_fail("release create Tag(\"TAG\") Title(\"TITLE\") Content(\"PATH\")"); goto done; }
        if (!tok_word(&t[i], "Title")) { parse_fail("release create Tag(\"TAG\") Title(\"TITLE\") Content(\"PATH\")"); goto done; }
        ++i; if (!read_param(t, &i, nt, b, sizeof(b), NULL)) { parse_fail("release create Tag(\"TAG\") Title(\"TITLE\") Content(\"PATH\")"); goto done; }
        if (!tok_word(&t[i], "Content")) { parse_fail("release create Tag(\"TAG\") Title(\"TITLE\") Content(\"PATH\")"); goto done; }
        ++i; if (!read_param(t, &i, nt, c, sizeof(c), NULL) || t[i].type != TK_END) { parse_fail("release create Tag(\"TAG\") Title(\"TITLE\") Content(\"PATH\")"); goto done; }
        rc = cmd_release_create(a, b, c); goto done;
    }

    if (tok_word(&t[0], "github") && tok_word(&t[1], "search")) {
        i = 2;
        if (t[i].type != TK_WORD) { parse_fail("github search repository|user|issue|pr(\"QUERY\")"); goto done; }
        if (!copy_string(a, sizeof(a), t[i].text)) { fgh_error("search type too long"); goto done; }
        ++i;
        if (!read_call(t, &i, nt, b, sizeof(b), NULL) || t[i].type != TK_END) { parse_fail("github search repository|user|issue|pr(\"QUERY\")"); goto done; }
        rc = cmd_search(a, b); goto done;
    }

    if (tok_word(&t[0], "github") && tok_word(&t[1], "user")) {
        if (t[2].type == TK_LPAREN) {
            i = 2;
            if (!read_call(t, &i, nt, a, sizeof(a), NULL) || t[i].type != TK_END) { parse_fail("github user(\"LOGIN\")"); goto done; }
            rc = cmd_user_info(a); goto done;
        }
        if (tok_word(&t[2], "repositories") || tok_word(&t[2], "followers") || tok_word(&t[2], "following")) {
            int sub = tok_word(&t[2], "repositories") ? 0 : (tok_word(&t[2], "followers") ? 1 : 2);
            i = 3;
            if (!read_call(t, &i, nt, a, sizeof(a), NULL) || t[i].type != TK_END) { parse_fail("github user repositories|followers|following(\"LOGIN\")"); goto done; }
            rc = sub == 0 ? cmd_user_repositories(a) : cmd_user_follow(a, sub == 2); goto done;
        }
    }

    if (tok_word(&t[0], "config") && tok_word(&t[1], "set")) {
        i = 2;
        if (t[i].type != TK_WORD || !copy_string(a, sizeof(a), t[i].text)) { parse_fail("config set KEY(\"VALUE\")"); goto done; }
        ++i;
        if (!read_param(t, &i, nt, b, sizeof(b), NULL) || t[i].type != TK_END) { parse_fail("config set KEY(\"VALUE\")"); goto done; }
        rc = cmd_config_set(a, b); goto done;
    }

    if (tok_word(&t[0], "config") && tok_word(&t[1], "get")) {
        if (t[2].type != TK_WORD || t[3].type != TK_END) { parse_fail("config get default-branch|editor|repository"); goto done; }
        rc = cmd_config_get(t[2].text); goto done;
    }

    fgh_error("unknown command; type help");
    rc = 0;

done:
    tokens_free(t, nt + 1);
    return rc;
}

static void repl(void) {
    char *line = (char *)malloc(FGH_MAX_CMD);
    if (!line) { fgh_error("out of memory"); return; }
    printf("Fgithub CLI %s\nType help for commands.\n", FGH_VERSION);
    for (;;) {
        printf("fgh> ");
        if (!fgets(line, FGH_MAX_CMD, stdin)) break;
        if (!strchr(line, '\n') && !feof(stdin)) {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) {}
            fgh_error("command is too long");
            continue;
        }
        trim_inplace(line);
        if (!line[0]) continue;
        if (str_ieq(line, "exit") || str_ieq(line, "quit")) break;
        parse_and_run(line);
    }
    free(line);
    printf("Bye.\n");
}

int main(int argc, char **argv) {
    int result;
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    load_config();
    if (argc == 1) {
        repl();
        return 0;
    }
    {
        size_t cap = FGH_MAX_CMD, used = 0, i;
        char *line = (char *)malloc(cap);
        if (!line) return 2;
        line[0] = 0;
        for (i = 1; i < (size_t)argc; ++i) {
            size_t len = strlen(argv[i]);
            if (used + len + 2 > cap) {
                size_t new_cap = cap;
                char *nb;
                while (used + len + 2 > new_cap) new_cap *= 2;
                if (new_cap > 1024u * 1024u) { free(line); return 2; }
                nb = (char *)realloc(line, new_cap);
                if (!nb) { free(line); return 2; }
                line = nb; cap = new_cap;
            }
            if (i > 1) line[used++] = ' ';
            memcpy(line + used, argv[i], len);
            used += len;
        }
        line[used] = 0;
        result = parse_and_run(line) ? 0 : 1;
        free(line);
    }
    return result;
}
