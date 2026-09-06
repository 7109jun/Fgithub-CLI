#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

#define FGH_VERSION "1.0.4"
#define FGH_MAX_CMD 8192
#define FGH_MAX_TOKENS 128
#define FGH_MAX_TEXT 2048

typedef enum {
    FGH_SUCCESS = 0,
    FGH_ERR_SYNTAX = 1,
    FGH_ERR_AUTH = 2,
    FGH_ERR_NETWORK = 3,
    FGH_ERR_NOT_FOUND = 4,
    FGH_ERR_MEMORY = 5,
    FGH_ERR_CONFIG = 6
} FghError;

static char g_token[512] = {0};
static char g_user[256] = {0};
static char g_default_branch[128] = "main";
static char g_editor[256] = "code";
static char g_current_repo[512] = {0};

// 최적화: HTTP 세션을 전역으로 유지하여 repl 모드에서 재사용
static HINTERNET g_hSession = NULL;

static int init_http(void) {
    if (!g_hSession) {
        g_hSession = WinHttpOpen(L"Fgithub-CLI/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!g_hSession) return 0;
    }
    return 1;
}

static void cleanup_http(void) {
    if (g_hSession) {
        WinHttpCloseHandle(g_hSession);
        g_hSession = NULL;
    }
}

static int str_ieq(const char* a, const char* b) {
    return _stricmp(a, b) == 0;
}

static int copy_string(char* dest, size_t cap, const char* src) {
    if (!dest || !src || cap == 0) return 0;
    size_t len = strlen(src);
    if (len >= cap) return 0;
    memcpy(dest, src, len + 1);
    return 1;
}

static char* dup_string(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char* d = (char*)malloc(len + 1);
    if (d) memcpy(d, s, len + 1);
    return d;
}

// 버그 수정: 빈 문자열("") 입력 시 메모리 경계 밖 참조(UB) 방지
static char* str_trim(char* s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    if (len == 0) return s;
    char* end = s + len - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    while (isspace((unsigned char)*s)) s++;
    return s;
}

static void fgh_error(const char* msg) {
    fprintf(stderr, "fgh error: %s\n", msg);
}

static int url_encode(const char* src, char* dest, size_t dest_size) {
    if (!src || !dest || dest_size == 0) return 0;
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j < dest_size - 3; i++) {
        if (isalnum((unsigned char)src[i]) || src[i] == '-' || src[i] == '_' || 
            src[i] == '.' || src[i] == '~') {
            dest[j++] = src[i];
        } else {
            if (j + 3 >= dest_size) return 0;
            sprintf(dest + j, "%%%02X", (unsigned char)src[i]);
            j += 3;
        }
    }
    dest[j] = '\0';
    return 1;
}

static const char* json_skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

// 버그 수정: snprintf 반환 값 정확히 체크하여 버퍼 오버플로우 방지
static int json_get_string(const char* json, const char* key, char* out, size_t out_size) {
    if (!json || !key || !out || out_size == 0) return 0;
    char search[512];
    int needed = snprintf(search, sizeof(search), "\"%s\"", key);
    if (needed < 0 || needed >= (int)sizeof(search)) return 0;
    
    const char* pos = strstr(json, search);
    if (!pos) return 0;
    
    pos += strlen(search);
    pos = json_skip_ws(pos);
    if (*pos != ':') return 0;
    pos = json_skip_ws(pos + 1);
    
    if (*pos == '"') {
        pos++;
        size_t i = 0;
        while (*pos && *pos != '"' && i < out_size - 1) {
            if (*pos == '\\' && *(pos + 1)) {
                pos++;
                if (*pos == 'n') out[i++] = '\n';
                else if (*pos == 'r') out[i++] = '\r';
                else if (*pos == 't') out[i++] = '\t';
                else out[i++] = *pos;
                pos++;
            } else {
                out[i++] = *pos++;
            }
        }
        out[i] = '\0';
        return 1;
    }
    return 0;
}

// 버그 수정: method 인자 정상 사용, mbstowcs 오버플로우 방지, Content-Type 헤더 추가
static int api_request(const char* method, const char* path, const char* body, 
                       const char* content_type, unsigned char** out_data, 
                       size_t* out_len, DWORD* status_code) {
    HINTERNET hConnect = NULL, hRequest = NULL;
    int result = FGH_ERR_NETWORK;
    *out_data = NULL;
    *out_len = 0;
    *status_code = 0;

    if (!init_http()) return FGH_ERR_NETWORK;

    hConnect = WinHttpConnect(g_hSession, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) goto cleanup;

    wchar_t wmethod[32];
    if (mbstowcs(wmethod, method, sizeof(wmethod) / sizeof(wchar_t)) == (size_t)-1) goto cleanup;
    wmethod[sizeof(wmethod) / sizeof(wchar_t) - 1] = L'\0';

    wchar_t wpath[2048];
    int wpath_len = mbstowcs(wpath, path, sizeof(wpath) / sizeof(wchar_t));
    if (wpath_len == -1 || wpath_len >= (int)(sizeof(wpath) / sizeof(wchar_t))) goto cleanup;
    
    hRequest = WinHttpOpenRequest(hConnect, wmethod, wpath, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) goto cleanup;

    if (g_token[0]) {
        wchar_t wauth[1024];
        swprintf(wauth, sizeof(wauth) / sizeof(wchar_t), L"Authorization: token %S", g_token);
        WinHttpAddRequestHeaders(hRequest, wauth, -1, WINHTTP_ADDREQ_FLAG_ADD);
    }
    WinHttpAddRequestHeaders(hRequest, L"User-Agent: Fgithub-CLI", -1, WINHTTP_ADDREQ_FLAG_ADD);
    WinHttpAddRequestHeaders(hRequest, L"Accept: application/vnd.github.v3+json", -1, WINHTTP_ADDREQ_FLAG_ADD);

    if (content_type) {
        wchar_t wctype[256];
        if (mbstowcs(wctype, content_type, sizeof(wctype) / sizeof(wchar_t)) != (size_t)-1) {
            wchar_t header[300];
            swprintf(header, sizeof(header) / sizeof(wchar_t), L"Content-Type: %s", wctype);
            WinHttpAddRequestHeaders(hRequest, header, -1, WINHTTP_ADDREQ_FLAG_ADD);
        }
    }

    DWORD body_len = body ? (DWORD)strlen(body) : 0;
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, 
                           (LPVOID)body, body_len, body_len, 0)) {
        goto cleanup;
    }

    if (!WinHttpReceiveResponse(hRequest, NULL)) goto cleanup;

    DWORD dwSize = sizeof(DWORD);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                       WINHTTP_HEADER_NAME_BY_INDEX, status_code, &dwSize, WINHTTP_NO_HEADER_INDEX);

    if (*status_code == 401) {
        fgh_error("Authentication failed (401). Check your token.");
        result = FGH_ERR_AUTH;
        goto cleanup;
    } else if (*status_code >= 400) {
        result = FGH_ERR_NETWORK;
        goto cleanup;
    }

    DWORD dwAvailableSize = 0;
    size_t total_size = 0;
    size_t capacity = 4096;
    *out_data = (unsigned char*)malloc(capacity);
    if (!*out_data) {
        result = FGH_ERR_MEMORY;
        goto cleanup;
    }

    while (WinHttpQueryDataAvailable(hRequest, &dwAvailableSize) && dwAvailableSize > 0) {
        if (total_size + dwAvailableSize + 1 > capacity) {
            capacity = (total_size + dwAvailableSize + 1) * 2;
            unsigned char* new_data = (unsigned char*)realloc(*out_data, capacity);
            if (!new_data) {
                free(*out_data);
                *out_data = NULL;
                result = FGH_ERR_MEMORY;
                goto cleanup;
            }
            *out_data = new_data;
        }
        DWORD dwDownloaded = 0;
        if (!WinHttpReadData(hRequest, (LPVOID)(*out_data + total_size), dwAvailableSize, &dwDownloaded)) {
            break;
        }
        total_size += dwDownloaded;
    }
    
    if (*out_data) (*out_data)[total_size] = '\0';
    *out_len = total_size;
    result = FGH_SUCCESS;

cleanup:
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    return result;
}

static int cmd_status(void) {
    if (g_token[0] && g_user[0]) {
        printf("Logged in to github.com as %s\n", g_user);
        return FGH_SUCCESS;
    }
    printf("Not logged in.\n");
    return FGH_SUCCESS;
}

static int cmd_login(const char* user, const char* token) {
    if (!copy_string(g_user, sizeof(g_user), user)) {
        fgh_error("Username too long");
        return FGH_ERR_CONFIG;
    }
    if (!copy_string(g_token, sizeof(g_token), token)) {
        fgh_error("Token too long");
        return FGH_ERR_CONFIG;
    }
    printf("Logged in successfully.\n");
    return FGH_SUCCESS;
}

static int cmd_logout(void) {
    memset(g_token, 0, sizeof(g_token));
    memset(g_user, 0, sizeof(g_user));
    printf("Logged out.\n");
    return FGH_SUCCESS;
}

typedef enum { TK_WORD, TK_STRING, TK_BRACKET, TK_LPAREN, TK_RPAREN, TK_END } TokType;
typedef struct { TokType type; char* text; } Token;

static void tokens_free(Token* tokens, int n) {
    if (!tokens) return;
    for (int i = 0; i < n; ++i) free(tokens[i].text);
    memset(tokens, 0, n * sizeof(*tokens));
}

// 버그 수정: malloc 실패 시 NULL 체크 추가, 초기 버퍼 512로 증가
static int lex_line(const char* line, Token* tokens, int max_tokens) {
    const char* p = line;
    int n = 0;
    if (!line || !tokens || max_tokens < 2) return -1;
    memset(tokens, 0, max_tokens * sizeof(*tokens));

    while (*p) {
        while (isspace((unsigned char)*p)) ++p;
        if (!*p) break;
        if (n >= max_tokens - 1) { tokens_free(tokens, n); return -2; }

        if (*p == '(') {
            tokens[n].type = TK_LPAREN;
            tokens[n].text = dup_string("(");
            ++n; ++p; continue;
        }
        if (*p == ')') {
            tokens[n].type = TK_RPAREN;
            tokens[n].text = dup_string(")");
            ++n; ++p; continue;
        }
        if (*p == '"') {
            ++p;
            char* buf = (char*)malloc(512);
            if (!buf) { tokens_free(tokens, n); return -5; }
            size_t len = 0, cap = 512;
            while (*p && *p != '"') {
                if (len + 2 >= cap) {
                    cap *= 2;
                    char* nb = (char*)realloc(buf, cap);
                    if (!nb) { free(buf); tokens_free(tokens, n); return -5; }
                    buf = nb;
                }
                if (*p == '\\' && *(p + 1)) {
                    ++p;
                    if (*p == 'n') buf[len++] = '\n';
                    else if (*p == 'r') buf[len++] = '\r';
                    else if (*p == 't') buf[len++] = '\t';
                    else buf[len++] = *p;
                    ++p;
                } else {
                    buf[len++] = *p++;
                }
            }
            if (*p != '"') { free(buf); tokens_free(tokens, n); return -4; }
            ++p;
            buf[len] = '\0';
            tokens[n].type = TK_STRING;
            tokens[n].text = buf;
            ++n; continue;
        }
        if (*p == '[') {
            ++p;
            char* buf = (char*)malloc(512);
            if (!buf) { tokens_free(tokens, n); return -5; }
            size_t len = 0, cap = 512;
            while (*p && *p != ']') {
                if (len + 2 >= cap) {
                    cap *= 2;
                    char* nb = (char*)realloc(buf, cap);
                    if (!nb) { free(buf); tokens_free(tokens, n); return -5; }
                    buf = nb;
                }
                buf[len++] = *p++;
            }
            if (*p != ']') { free(buf); tokens_free(tokens, n); return -4; }
            ++p;
            while (len > 0 && isspace((unsigned char)buf[len - 1])) buf[--len] = '\0';
            buf[len] = '\0';
            tokens[n].type = TK_BRACKET;
            tokens[n].text = buf;
            ++n; continue;
        }

        const char* start_word = p;
        while (*p && !isspace((unsigned char)*p) && *p != '(' && *p != ')' && 
               *p != '[' && *p != ']') ++p;
        if (p == start_word) { tokens_free(tokens, n); return -3; }
        
        size_t wlen = (size_t)(p - start_word);
        tokens[n].text = (char*)malloc(wlen + 1);
        if (!tokens[n].text) { tokens_free(tokens, n); return -5; }
        memcpy(tokens[n].text, start_word, wlen);
        tokens[n].text[wlen] = '\0';
        tokens[n].type = TK_WORD;
        ++n;
    }
    tokens[n].type = TK_END;
    tokens[n].text = dup_string("");
    return n;
}

static int tok_word(const Token* t, const char* s) { 
    return t && t->type == TK_WORD && str_ieq(t->text, s); 
}
static int tok_value(const Token* t) { 
    return t && (t->type == TK_STRING || t->type == TK_BRACKET || t->type == TK_WORD); 
}

static int read_param(Token* t, int* i, int nt, char* out, size_t cap, int* is_file) {
    if (!t || !i || *i < 0 || *i + 3 >= nt || !out || cap == 0) return 0;
    if (t[*i].type != TK_WORD || t[*i + 1].type != TK_LPAREN || 
        !tok_value(&t[*i + 2]) || t[*i + 3].type != TK_RPAREN) return 0;
    if (!copy_string(out, cap, t[*i + 2].text)) return 0;
    if (is_file) *is_file = (t[*i + 2].type == TK_BRACKET);
    *i += 4;
    return 1;
}

static int read_call(Token* t, int* i, int nt, char* out, size_t cap, int* is_file) {
    if (!t || !i || *i < 0 || *i + 2 >= nt || !out || cap == 0) return 0;
    if (t[*i].type != TK_LPAREN || !tok_value(&t[*i + 1]) || t[*i + 2].type != TK_RPAREN) return 0;
    if (!copy_string(out, cap, t[*i + 1].text)) return 0;
    if (is_file) *is_file = (t[*i + 1].type == TK_BRACKET);
    *i += 3;
    return 1;
}

// 버그 수정: nt(토큰 개수)를 인자로 받아 배열 범위 초과 접근 방지
static int read_number(Token* t, int index, int nt, long* out) {
    char* end;
    long n;
    if (!t || !out || index < 0) return 0;
    
    int start_idx = index;
    if (t[index].type == TK_LPAREN) {
        if (index + 2 >= nt) return 0; // 경계값 체크
        if (!t[index + 1].text || t[index + 1].type != TK_WORD || t[index + 2].type != TK_RPAREN) return 0;
        start_idx = index + 1;
    } else if (t[index].type != TK_WORD) {
        return 0;
    }

    errno = 0;
    n = strtol(t[start_idx].text, &end, 10);
    if (errno == ERANGE || *end != '\0') return 0;
    *out = n;
    return 1;
}

static int parse_and_run(const char* line) {
    Token t[FGH_MAX_TOKENS];
    int nt, i;
    char a[FGH_MAX_TEXT], b[FGH_MAX_TEXT];
    int rc = FGH_ERR_SYNTAX;

    memset(t, 0, sizeof(t));
    nt = lex_line(line, t, FGH_MAX_TOKENS);
    
    if (nt == 0) { tokens_free(t, 1); return FGH_SUCCESS; }
    if (nt < 0) {
        if (nt == -2) fgh_error("too many command tokens");
        else if (nt == -4) fgh_error("unterminated quoted string or [file path]");
        else if (nt == -5) fgh_error("out of memory during parsing");
        else fgh_error("invalid command syntax");
        tokens_free(t, FGH_MAX_TOKENS);
        return FGH_ERR_SYNTAX;
    }

    if (tok_word(&t[0], "help")) { 
        printf("Fgithub CLI %s\n", FGH_VERSION);
        printf("Commands:\n");
        printf("  login name(\"USER\") password(\"TOKEN\")\n");
        printf("  login status\n");
        printf("  logout\n");
        printf("  version\n");
        rc = FGH_SUCCESS; goto done; 
    }
    if (tok_word(&t[0], "version")) { 
        printf("Fgithub CLI %s\n", FGH_VERSION); 
        rc = FGH_SUCCESS; goto done; 
    }
    
    if (tok_word(&t[0], "login")) {
        if (tok_word(&t[1], "status") && t[2].type == TK_END) {
            rc = cmd_status(); goto done;
        }
        i = 1;
        if (!read_param(t, &i, nt, a, sizeof(a), NULL) || !tok_word(&t[i], "password")) {
            fgh_error("Syntax: login name(\"USER\") password(\"TOKEN\")"); goto done;
        }
        ++i;
        if (!read_param(t, &i, nt, b, sizeof(b), NULL) || t[i].type != TK_END) {
            fgh_error("Syntax: login name(\"USER\") password(\"TOKEN\")"); goto done;
        }
        rc = cmd_login(a, b); goto done;
    }
    if (tok_word(&t[0], "logout")) {
        if (t[1].type != TK_END) { fgh_error("Syntax: logout"); goto done; }
        rc = cmd_logout(); goto done;
    }

    fgh_error("unknown command; type help");
    rc = FGH_ERR_SYNTAX;

done:
    tokens_free(t, nt + 1);
    return rc;
}

static void repl(void) {
    char* line = (char*)malloc(FGH_MAX_CMD);
    if (!line) { fgh_error("out of memory"); return; }
    
    printf("Fgithub CLI %s\nType 'help' for commands.\n", FGH_VERSION);
    for (;;) {
        printf("fgh> ");
        fflush(stdout);
        if (!fgets(line, FGH_MAX_CMD, stdin)) break;
        
        char* nl = strchr(line, '\n');
        if (!nl && !feof(stdin)) {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) {}
            fgh_error("command is too long");
            continue;
        }
        if (nl) *nl = '\0';
        
        char* trimmed = str_trim(line);
        if (!trimmed[0]) continue;
        if (str_ieq(trimmed, "exit") || str_ieq(trimmed, "quit")) break;
        
        parse_and_run(trimmed);
    }
    free(line);
    printf("Bye.\n");
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    if (argc >= 2) {
        if (str_ieq(argv[1], "--version") || str_ieq(argv[1], "-v")) {
            printf("Fgithub CLI %s\n", FGH_VERSION);
            return FGH_SUCCESS;
        }
        if (str_ieq(argv[1], "--help") || str_ieq(argv[1], "-h")) {
            printf("Fgithub CLI %s\nType 'help' for commands.\n", FGH_VERSION);
            return FGH_SUCCESS;
        }
    }

    if (argc == 1) {
        repl();
        cleanup_http(); // 세션 정리
        return FGH_SUCCESS;
    }

    size_t cap = FGH_MAX_CMD, used = 0;
    char* line = (char*)malloc(cap);
    if (!line) return FGH_ERR_MEMORY;
    line[0] = '\0';

    // 버그 수정: realloc 시 new_cap이 필요한 크기보다 작을 수 있는 문제 해결
    for (int i = 1; i < argc; ++i) {
        size_t len = strlen(argv[i]);
        while (used + len + 2 > cap) {
            size_t new_cap = cap * 2;
            if (new_cap > 1024 * 1024) { free(line); return FGH_ERR_MEMORY; }
            char* nb = (char*)realloc(line, new_cap);
            if (!nb) { free(line); return FGH_ERR_MEMORY; }
            line = nb; cap = new_cap;
        }
        if (i > 1) line[used++] = ' ';
        memcpy(line + used, argv[i], len);
        used += len;
    }
    line[used] = '\0';

    int result = parse_and_run(line);
    free(line);
    cleanup_http(); // 세션 정리
    return result;
}
