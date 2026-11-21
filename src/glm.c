/* glm.c — config persistence, HTTP client, response parsing, background jobs.
 *
 * Two build modes:
 *   normal        libcurl + pthreads; one detached worker thread per job
 *   GLM_NO_NET    no network at all: refresh() serves canned demo data, so the
 *                 UI still builds and can be screenshotted anywhere
 */
#include "glm.h"
#include "crypto.h"
#include "minijson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define PATH_SEP '\\'
#define mkdir(p, mode) _mkdir(p)
#define gmtime_r(t, tm) gmtime_s((tm), (t))
#define localtime_r(t, tm) localtime_s((tm), (t))
#else
#define PATH_SEP '/'
#endif

#ifdef GLM_NO_NET
/* the same code paths run single-threaded with trivial stubs */
typedef int pthread_mutex_t;
#define pthread_mutex_init(m, a) ((void)(m), (void)(a), 0)
#define pthread_mutex_destroy(m) ((void)(m))
#define pthread_mutex_lock(m) ((void)(m))
#define pthread_mutex_unlock(m) ((void)(m))
#else
#include <curl/curl.h>
#include <pthread.h>
#endif

#define ARENA_NODES (256 * 1024)
#define ARENA_STRS (256 * 1024)

static long long now_epoch(void) { return (long long)time(NULL); }
static void demo_fill(glm_client *c, int jitter);
static void demo_refresh(glm_client *c);

/* ---------------------------------------------------------------- client */

struct glm_client {
    glm_config cfg;
    pthread_mutex_t lock;
    int state;
    char error[256];
    glm_snapshot snap;
    long long fetch_gen;
    int fetching;
    int demo;

    int discovering;
    int discover_gen;
    int discover_ok;
    char discover_error[256];
    glm_context discovered[GLM_MAX_CONTEXTS];
    int discovered_count;
};

glm_client *glm_client_create(void) {
    glm_client *c = calloc(1, sizeof *c);
    pthread_mutex_init(&c->lock, NULL);
    return c;
}

void glm_client_destroy(glm_client *c) {
    if (!c) return;
    pthread_mutex_destroy(&c->lock);
    free(c);
}

void glm_client_get_config(glm_client *c, glm_config *out) {
    pthread_mutex_lock(&c->lock);
    *out = c->cfg;
    pthread_mutex_unlock(&c->lock);
}

void glm_client_apply_config(glm_client *c, const glm_config *cfg) {
    pthread_mutex_lock(&c->lock);
    c->cfg = *cfg;
    pthread_mutex_unlock(&c->lock);
    glm_client_refresh(c);
}

int glm_client_state(const glm_client *c) {
    pthread_mutex_lock((pthread_mutex_t *)&c->lock);
    int s = c->state;
    pthread_mutex_unlock((pthread_mutex_t *)&c->lock);
    return s;
}

const char *glm_client_error(const glm_client *c) { return c->error; }

int glm_client_snapshot(glm_client *c, glm_snapshot *out) {
    pthread_mutex_lock(&c->lock);
    *out = c->snap;
    pthread_mutex_unlock(&c->lock);
    return 1;
}

int glm_client_fetch_gen(const glm_client *c) {
    pthread_mutex_lock((pthread_mutex_t *)&c->lock);
    int g = (int)c->fetch_gen;
    pthread_mutex_unlock((pthread_mutex_t *)&c->lock);
    return g;
}

int glm_client_is_demo(const glm_client *c) { return c->demo; }

/* ---------------------------------------------------------------- config */

const char *glm_config_path(void) {
    static char path[1024];
    const char *over = getenv("GLM_CONFIG");
    if (over && over[0]) return over;
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata)
        snprintf(path, sizeof path, "%s\\glm-usage-panel\\config.json", appdata);
    else
        snprintf(path, sizeof path, "glm-usage-panel-config.json");
#else
    const char *home = getenv("HOME");
    if (!home) home = ".";
    snprintf(path, sizeof path, "%s/.config/glm-usage-panel/config.json", home);
#endif
    return path;
}

void glm_config_defaults(glm_config *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->auth_mode = GLM_AUTH_APIKEY;
    snprintf(cfg->base_url, sizeof cfg->base_url, "https://open.bigmodel.cn");
    snprintf(cfg->web_url, sizeof cfg->web_url, "https://bigmodel.cn");
    cfg->refresh_seconds = 60;
    cfg->auto_refresh = 1;
    cfg->dark = 1;
}

void glm_config_load(glm_config *cfg) {
    glm_config_defaults(cfg);
    FILE *f = fopen(glm_config_path(), "rb");
    if (!f) return;
    static char buf[64 * 1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';

    static char node_arena[ARENA_NODES], str_arena[ARENA_STRS];
    mj_parser p;
    mj_val *root = mj_parse(&p, buf, n, node_arena, sizeof node_arena, str_arena, sizeof str_arena);
    if (!root) return;

    mj_val *v;
    if ((v = mj_get(root, "auth_mode"))) cfg->auth_mode = mj_ll(v) ? 1 : 0;
    if ((v = mj_get(root, "api_key"))) snprintf(cfg->api_key, sizeof cfg->api_key, "%s", mj_strz(v));
    if ((v = mj_get(root, "token"))) snprintf(cfg->token, sizeof cfg->token, "%s", mj_strz(v));
    if ((v = mj_get(root, "base_url")) && mj_strz(v)[0])
        snprintf(cfg->base_url, sizeof cfg->base_url, "%s", mj_strz(v));
    if ((v = mj_get(root, "web_url")) && mj_strz(v)[0])
        snprintf(cfg->web_url, sizeof cfg->web_url, "%s", mj_strz(v));
    if ((v = mj_get(root, "refresh_seconds")) && mj_ll(v) >= 15)
        cfg->refresh_seconds = (int)mj_ll(v);
    if ((v = mj_get(root, "auto_refresh"))) cfg->auto_refresh = mj_bool(v) ? 1 : 0;
    if ((v = mj_get(root, "dark"))) cfg->dark = mj_bool(v) ? 1 : 0;
    if ((v = mj_get(root, "selected"))) cfg->selected = (int)mj_ll(v);
    if ((v = mj_get(root, "contexts")) && v->type == MJ_ARR) {
        int count = mj_count(v);
        if (count > GLM_MAX_CONTEXTS) count = GLM_MAX_CONTEXTS;
        cfg->context_count = count;
        for (int i = 0; i < count; i++) {
            mj_val *cv = mj_at(v, i);
            glm_context *dst = &cfg->contexts[i];
            snprintf(dst->name, sizeof dst->name, "%s", mj_strz(mj_get(cv, "name")));
            snprintf(dst->org, sizeof dst->org, "%s", mj_strz(mj_get(cv, "org")));
            snprintf(dst->project, sizeof dst->project, "%s", mj_strz(mj_get(cv, "project")));
            dst->project_type = (int)mj_ll(mj_get(cv, "project_type"));
            if (dst->project_type != 1 && dst->project_type != 2) dst->project_type = 2;
        }
    }
    if (cfg->selected >= cfg->context_count) cfg->selected = 0;
}

static void json_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const char *s = in; *s && o + 7 < cap; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') { out[o++] = '\\'; out[o++] = (char)ch; }
        else if (ch < 0x20) o += (size_t)snprintf(out + o, cap - o, "\\u%04x", ch);
        else out[o++] = (char)ch;
    }
    out[o] = '\0';
}

int glm_config_save(const glm_config *cfg) {
    char path[1024];
    snprintf(path, sizeof path, "%s", glm_config_path());
    char *slash = strrchr(path, PATH_SEP);
    if (slash) {
        *slash = '\0';
        mkdir(path, 0755);
        *slash = PATH_SEP;
    }
    FILE *f = fopen(path, "wb");
    if (!f) return 0;

    char esc[4096];
    fprintf(f, "{\n  \"auth_mode\": %d,\n", cfg->auth_mode);
    json_escape(cfg->api_key, esc, sizeof esc);
    fprintf(f, "  \"api_key\": \"%s\",\n", esc);
    json_escape(cfg->token, esc, sizeof esc);
    fprintf(f, "  \"token\": \"%s\",\n", esc);
    json_escape(cfg->base_url, esc, sizeof esc);
    fprintf(f, "  \"base_url\": \"%s\",\n", esc);
    json_escape(cfg->web_url, esc, sizeof esc);
    fprintf(f, "  \"web_url\": \"%s\",\n", esc);
    fprintf(f, "  \"refresh_seconds\": %d,\n  \"auto_refresh\": %s,\n  \"dark\": %s,\n",
            cfg->refresh_seconds, cfg->auto_refresh ? "true" : "false",
            cfg->dark ? "true" : "false");
    fprintf(f, "  \"selected\": %d,\n  \"contexts\": [\n", cfg->selected);
    for (int i = 0; i < cfg->context_count; i++) {
        const glm_context *ctx = &cfg->contexts[i];
        json_escape(ctx->name, esc, sizeof esc);
        fprintf(f, "    {\"name\": \"%s\", ", esc);
        json_escape(ctx->org, esc, sizeof esc);
        fprintf(f, "\"org\": \"%s\", ", esc);
        json_escape(ctx->project, esc, sizeof esc);
        fprintf(f, "\"project\": \"%s\", \"project_type\": %d}%s\n", esc, ctx->project_type,
                i + 1 < cfg->context_count ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    return 1;
}

#ifndef GLM_NO_NET

/* ----------------------------------------------------------------- http */

typedef struct {
    char *buf;
    size_t len, cap;
} http_buf;

static size_t http_write(void *data, size_t sz, size_t n, void *ud) {
    http_buf *b = ud;
    size_t add = sz * n;
    if (b->len + add + 1 > b->cap) {
        size_t want = b->cap ? b->cap * 2 : 8192;
        while (want < b->len + add + 1) want *= 2;
        char *nb = realloc(b->buf, want);
        if (!nb) return 0;
        b->buf = nb;
        b->cap = want;
    }
    memcpy(b->buf + b->len, data, add);
    b->len += add;
    b->buf[b->len] = '\0';
    return add;
}

static int http_get(const char *url, const char **headers, int nhdrs, char **body_out,
                    char *err, size_t errcap) {
    CURL *curl = curl_easy_init();
    if (!curl) { snprintf(err, errcap, "curl 初始化失败"); return 0; }

    http_buf b = {0};
    struct curl_slist *hdrs = NULL;
    for (int i = 0; i < nhdrs; i++) hdrs = curl_slist_append(hdrs, headers[i]);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "glm-usage-panel/1.0");

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        snprintf(err, errcap, "网络错误: %s", curl_easy_strerror(rc));
        free(b.buf);
        return 0;
    }
    if (status != 200) {
        snprintf(err, errcap, "HTTP %ld", status);
        free(b.buf);
        return 0;
    }
    if (!b.buf) b.buf = strdup("");
    *body_out = b.buf;
    return 1;
}

/* ---------------------------------------------------------------- parsing */

typedef struct {
    mj_parser parser;
    char *node_arena;          /* heap: 2x256KB on the stack would blow the worker's stack */
    char *str_arena;
    mj_val *root;
} json_doc;

static void json_doc_free(json_doc *d) {
    free(d->node_arena);
    free(d->str_arena);
    d->node_arena = d->str_arena = NULL;
}

static int json_doc_init(json_doc *d) {
    memset(d, 0, sizeof *d);
    d->node_arena = malloc(ARENA_NODES);
    d->str_arena = malloc(ARENA_STRS);
    if (!d->node_arena || !d->str_arena) {
        json_doc_free(d);
        return 0;
    }
    return 1;
}

static int json_open(json_doc *d, const char *body) {
    d->root = mj_parse(&d->parser, body, strlen(body), d->node_arena, ARENA_NODES, d->str_arena,
                       ARENA_STRS);
    return d->root != NULL;
}

/* Parse "2026-11-21T18:00:00.000+08:00" / "...Z" into epoch seconds (UTC based). */
static long long parse_iso8601(const char *s) {
    if (!s || !s[0]) return 0;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 3) return 0;
    /* days from civil (Howard Hinnant's algorithm), no reliance on the TZ database */
    long long yy = y;
    yy -= mo <= 2;
    long long era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned yoe = (unsigned)(yy - era * 400);
    unsigned doy = (unsigned)((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + (long long)doe - 719468;
    long long epoch = days * 86400 + h * 3600 + mi * 60 + sec;

    const char *plus = strchr(s, '+');
    long long off = 0;
    if (plus) {
        int oh = 0, om = 0;
        if (sscanf(plus + 1, "%d:%d", &oh, &om) >= 1) off = oh * 3600 + om * 60;
    }
    return epoch - off; /* "Z" or no suffix needs no correction */
}

static void parse_quota_body(json_doc *d, const char *body, glm_snapshot *snap, char *err, size_t errcap) {
    if (!json_open(d, body)) {
        snprintf(err, errcap, "响应不是合法 JSON");
        return;
    }
    mj_val *root = d->root;
    mj_val *code = mj_get(root, "code");
    mj_val *data = mj_get(root, "data");
    mj_val *mv = mj_get(root, "message");
    if (!mv) mv = mj_get(root, "msg");
    const char *message = mj_strz(mv);

    if (code && mj_ll(code) != 200 && mj_ll(code) != 0) {
        if (message[0]) snprintf(err, errcap, "%s", message);
        else snprintf(err, errcap, "接口返回 code %lld", mj_ll(code));
        if (strstr(message, "coding plan")) snap->no_plan = 1; /* expected on personal orgs */
        return;
    }
    if (!data) { snprintf(err, errcap, "响应缺少 data 字段"); return; }
    if (data->type == MJ_OBJ && mj_count(data) == 0) {
        snprintf(snap->hint, sizeof snap->hint,
                 "接口返回了空数据：请检查机构/项目上下文配置");
        return;
    }

    mj_val *level = mj_get(data, "level");
    if (level && mj_strz(level)[0]) snprintf(snap->level, sizeof snap->level, "%s", mj_strz(level));

    mj_val *limits = mj_get(data, "limits");
    if (limits && limits->type == MJ_ARR) {
        int n = mj_count(limits);
        for (int i = 0; i < n && snap->limit_count < GLM_MAX_LIMITS; i++) {
            mj_val *item = mj_at(limits, i);
            glm_limit *dst = &snap->limits[snap->limit_count];
            memset(dst, 0, sizeof *dst);
            snprintf(dst->kind, sizeof dst->kind, "%s", mj_strz(mj_get(item, "type")));
            dst->percentage = mj_num(mj_get(item, "percentage"));
            dst->remaining = mj_num(mj_get(item, "remaining"));
            dst->current_value = mj_num(mj_get(item, "currentValue"));
            dst->usage = mj_num(mj_get(item, "usage"));
            dst->unit = (int)mj_num(mj_get(item, "unit"));
            dst->number = mj_num(mj_get(item, "number"));
            /* nextResetTime comes as epoch millis (number) on the live API and as
             * an ISO-8601 string on older shapes; accept both */
            mj_val *nrt = mj_get(item, "nextResetTime");
            if (nrt && nrt->type == MJ_NUM && nrt->num > 1e12)
                dst->next_reset = (long long)(nrt->num / 1000.0);
            else
                dst->next_reset = parse_iso8601(mj_strz(nrt));
            snap->limit_count++;
        }
    }
}

static void parse_resets_body(json_doc *d, const char *body, glm_snapshot *snap) {
    if (!json_open(d, body)) return;
    mj_val *data = mj_get(d->root, "data");
    if (!data) return;
    mj_val *five = mj_get(data, "fiveHourResets");
    mj_val *week = mj_get(data, "weekResets");
    snap->has_resets = 1;
    if (five && five->type == MJ_ARR)
        for (mj_val *cv = five->child; cv; cv = cv->next) {
            mj_val *av = mj_get(cv, "available");
            if (!av || mj_bool(av)) snap->five_resets++;
        }
    if (week && week->type == MJ_ARR)
        for (mj_val *cv = week->child; cv; cv = cv->next) {
            mj_val *av = mj_get(cv, "available");
            if (!av || mj_bool(av)) snap->week_resets++;
        }
}

static void parse_account_body(json_doc *d, const char *body, glm_snapshot *snap) {
    if (!json_open(d, body)) return;
    mj_val *data = mj_get(d->root, "data");
    if (!data) return;
    snap->balance = mj_num(mj_get(data, "balance"));
    snap->give_amount = mj_num(mj_get(data, "giveAmount"));
    snap->total_spend = mj_num(mj_get(data, "totalSpendAmount"));
    snap->has_account = 1;
}

static int model_row_fill(glm_model_usage *row, mj_val *item) {
    mj_val *v = mj_get(item, "model");
    if (!v || !mj_strz(v)[0]) v = mj_get(item, "modelName");
    if (!v || !mj_strz(v)[0]) v = mj_get(item, "name");
    const char *name = mj_strz(v);
    if (!name[0]) return 0;
    static const char *token_keys[] = { "totalTokens", "tokens", "tokenTotal", "total_tokens",
                                        "tokenUsage" };
    double tokens = 0;
    for (size_t i = 0; i < sizeof token_keys / sizeof *token_keys && tokens == 0; i++)
        tokens = mj_num(mj_get(item, token_keys[i]));
    static const char *req_keys[] = { "count", "requests", "requestCount", "requestsCount" };
    double requests = 0;
    for (size_t i = 0; i < sizeof req_keys / sizeof *req_keys && requests == 0; i++)
        requests = mj_num(mj_get(item, req_keys[i]));
    if (tokens <= 0 && requests <= 0) return 0;
    snprintf(row->model, sizeof row->model, "%.47s", name);
    row->tokens = tokens;
    row->requests = requests;
    return 1;
}

static int cmp_models(const void *a, const void *b) {
    const glm_model_usage *x = a, *y = b;
    if (x->tokens > y->tokens) return -1;
    if (x->tokens < y->tokens) return 1;
    return 0;
}

static void parse_model_usage_body(json_doc *d, const char *body, glm_snapshot *snap) {
    if (!json_open(d, body)) return;
    mj_val *data = mj_get(d->root, "data");
    if (!data) return;
    if (data->type == MJ_OBJ) {
        mj_val *arr = mj_get(data, "usageList");
        if (!arr) arr = mj_get(data, "list");
        if (!arr) arr = mj_get(data, "items");
        if (!arr) arr = mj_get(data, "models");
        data = arr;
    }
    if (!data || data->type != MJ_ARR) return;
    snap->model_count = 0;
    int n = mj_count(data);
    for (int i = 0; i < n && snap->model_count < GLM_MAX_MODELS; i++) {
        mj_val *item = mj_at(data, i);
        if (item && model_row_fill(&snap->models[snap->model_count], item)) snap->model_count++;
    }
    if (snap->model_count > 1)
        qsort(snap->models, (size_t)snap->model_count, sizeof(glm_model_usage), cmp_models);
}

/* ------------------------------------------------------------ fetch round */

static void urlencode_spaces(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = in; *p && o + 4 < cap; p++) {
        if (*p == ' ') out[o++] = '%', out[o++] = '2', out[o++] = '0';
        else out[o++] = *p;
    }
    out[o] = '\0';
}

static void fetch_all(glm_client *c) {
    glm_config *cfg = &c->cfg;
    glm_snapshot snap;
    memset(&snap, 0, sizeof snap);
    char err[256] = "";

    json_doc doc;
    if (!json_doc_init(&doc)) return;

    const glm_context *ctx = NULL;
    if (cfg->context_count > 0 && cfg->selected < cfg->context_count)
        ctx = &cfg->contexts[cfg->selected];

    char jwt[1400] = "";
    if (cfg->auth_mode == GLM_AUTH_APIKEY &&
        !glm_jwt_mint(cfg->api_key, now_epoch(), jwt, sizeof jwt)) {
        pthread_mutex_lock(&c->lock);
        c->state = GLM_ERROR;
        snprintf(c->error, sizeof c->error, "API Key 格式应为 <id>.<secret>");
        c->fetch_gen++;
        pthread_mutex_unlock(&c->lock);
        return;
    }

    /* headers are recomputed per round: the JWT must be fresh */
    static char auth_h[2200], org_h[160], proj_h[160];
    int nhdrs = 0;
    const char *hdrs[8];
    const char *token_src = cfg->token;
    if (!strncmp(token_src, "Bearer ", 7)) token_src += 7;
    if (cfg->auth_mode == GLM_AUTH_TOKEN)
        snprintf(auth_h, sizeof auth_h, "Authorization: %s", token_src);
    else
        snprintf(auth_h, sizeof auth_h, "Authorization: %s", jwt);
    hdrs[nhdrs++] = auth_h;
    if (ctx && ctx->org[0]) {
        snprintf(org_h, sizeof org_h, "Bigmodel-Organization: %s", ctx->org);
        hdrs[nhdrs++] = org_h;
    }
    if (ctx && ctx->project[0]) {
        snprintf(proj_h, sizeof proj_h, "Bigmodel-Project: %s", ctx->project);
        hdrs[nhdrs++] = proj_h;
    }
    hdrs[nhdrs++] = "Accept: application/json";
    hdrs[nhdrs++] = "Accept-Language: zh-CN,zh;q=0.9,en;q=0.8";

    /* 1) quota limits — the core endpoint */
    static char url[512];
    snprintf(url, sizeof url, "%s/api/monitor/usage/quota/limit?type=2", cfg->base_url);
    char *body = NULL;
    if (!http_get(url, hdrs, nhdrs, &body, err, sizeof err)) {
        json_doc_free(&doc);
        pthread_mutex_lock(&c->lock);
        c->state = GLM_ERROR;
        snprintf(c->error, sizeof c->error, "查询额度失败: %.200s", err);
        c->fetch_gen++;
        pthread_mutex_unlock(&c->lock);
        return;
    }
    parse_quota_body(&doc, body, &snap, err, sizeof err);
    free(body);
    if (err[0] && !snap.no_plan) {
        json_doc_free(&doc);
        pthread_mutex_lock(&c->lock);
        c->state = GLM_ERROR;
        snprintf(c->error, sizeof c->error, "%.250s", err);
        c->fetch_gen++;
        pthread_mutex_unlock(&c->lock);
        return;
    }

    /* 2) reset vouchers (team plans, non-fatal) */
    if (!ctx || ctx->project_type == 2) {
        snprintf(url, sizeof url, "%s/api/biz/customer-package-reset/list?targetType=TEAM",
                 cfg->base_url);
        body = NULL;
        if (http_get(url, hdrs, nhdrs, &body, err, sizeof err)) {
            parse_resets_body(&doc, body, &snap);
            free(body);
        }
    }

    /* 3) account report (non-fatal; team orgs use the org-owner endpoint) */
    {
        const char *path = (!ctx || ctx->project_type == 2)
                               ? "/api/biz/account/query-org-owner-account-report"
                               : "/api/biz/account/query-customer-account-report";
        snprintf(url, sizeof url, "%s%s", cfg->base_url, path);
        body = NULL;
        if (http_get(url, hdrs, nhdrs, &body, err, sizeof err)) {
            parse_account_body(&doc, body, &snap);
            free(body);
        }
    }

    /* 4) model usage over the last 24 hours (best effort) */
    {
        long long start = now_epoch() - 24 * 3600, end = now_epoch();
        char s_str[32], e_str[32], s_enc[64], e_enc[64];
        time_t tt = (time_t)start;
        strftime(s_str, sizeof s_str, "%Y-%m-%d %H:%M:%S", localtime(&tt));
        tt = (time_t)end;
        strftime(e_str, sizeof e_str, "%Y-%m-%d %H:%M:%S", localtime(&tt));
        urlencode_spaces(s_str, s_enc, sizeof s_enc);
        urlencode_spaces(e_str, e_enc, sizeof e_enc);
        snprintf(url, sizeof url, "%s/api/monitor/usage/model-usage?startTime=%s&endTime=%s",
                 cfg->base_url, s_enc, e_enc);
        body = NULL;
        if (http_get(url, hdrs, nhdrs, &body, err, sizeof err)) {
            parse_model_usage_body(&doc, body, &snap);
            free(body);
        }
    }

    snap.fetched_at = now_epoch();
    json_doc_free(&doc);
    pthread_mutex_lock(&c->lock);
    c->snap = snap;
    c->state = GLM_OK;
    c->error[0] = '\0';
    c->fetch_gen++;
    pthread_mutex_unlock(&c->lock);
}

/* ------------------------------------------------------- discovery (web JWT) */

static void discover_all(glm_client *c, const char *web_jwt) {
    char base[256];
    pthread_mutex_lock(&c->lock);
    snprintf(base, sizeof base, "%s", c->cfg.web_url);
    pthread_mutex_unlock(&c->lock);

    char url[512], auth_h[2100];
    snprintf(url, sizeof url, "%s/api/biz/customer/getCustomerInfo", base);
    const char *jwt = web_jwt;
    if (!strncmp(jwt, "Bearer ", 7)) jwt += 7;
    snprintf(auth_h, sizeof auth_h, "Authorization: Bearer %s", jwt);
    const char *hdrs[] = { auth_h, "Accept: application/json" };

    char err[256] = "";
    char *body = NULL;
    if (!http_get(url, hdrs, 2, &body, err, sizeof err)) {
        pthread_mutex_lock(&c->lock);
        snprintf(c->discover_error, sizeof c->discover_error, "发现请求失败: %.200s", err);
        c->discovering = 0;
        c->discover_gen++;
        pthread_mutex_unlock(&c->lock);
        return;
    }

    json_doc d;
    glm_context found[GLM_MAX_CONTEXTS];
    int count = 0;
    if (json_doc_init(&d) && json_open(&d, body)) {
        mj_val *data = mj_get(d.root, "data");
        mj_val *orgs = data ? mj_get(data, "organizations") : NULL;
        if (orgs && orgs->type == MJ_ARR) {
            int n = mj_count(orgs);
            for (int i = 0; i < n && count < GLM_MAX_CONTEXTS; i++) {
                mj_val *org = mj_at(orgs, i);
                mj_val *org_id = mj_get(org, "organizationId");
                mj_val *org_name = mj_get(org, "organizationName");
                if (!org_name) org_name = mj_get(org, "name");
                mj_val *projects = mj_get(org, "projects");
                int pc = projects && projects->type == MJ_ARR ? mj_count(projects) : 1;
                for (int j = 0; j < pc && count < GLM_MAX_CONTEXTS; j++) {
                    glm_context *dst = &found[count];
                    memset(dst, 0, sizeof *dst);
                    const char *oid = mj_strz(org_id);
                    if (!oid[0]) break;
                    snprintf(dst->org, sizeof dst->org, "%s", oid);
                    mj_val *proj = projects ? mj_at(projects, j) : NULL;
                    mj_val *proj_id = proj ? mj_get(proj, "projectId") : NULL;
                    mj_val *proj_name = proj ? mj_get(proj, "projectName") : NULL;
                    if (!proj_name) proj_name = proj ? mj_get(proj, "name") : NULL;
                    if (proj_id && mj_strz(proj_id)[0])
                        snprintf(dst->project, sizeof dst->project, "%s", mj_strz(proj_id));
                    mj_val *ptype = proj ? mj_get(proj, "projectType") : NULL;
                    dst->project_type = ptype ? (int)mj_ll(ptype) : 1;
                    if (dst->project_type != 1 && dst->project_type != 2) dst->project_type = 2;
                    if (proj_name && mj_strz(proj_name)[0] && org_name && mj_strz(org_name)[0])
                        snprintf(dst->name, sizeof dst->name, "%.31s · %.23s", mj_strz(org_name),
                                 mj_strz(proj_name));
                    else if (org_name && mj_strz(org_name)[0])
                        snprintf(dst->name, sizeof dst->name, "%.55s", mj_strz(org_name));
                    else
                        snprintf(dst->name, sizeof dst->name, "%.55s", oid);
                    count++;
                }
            }
        }
    }
    free(body);
    json_doc_free(&d);

    pthread_mutex_lock(&c->lock);
    c->discovering = 0;
    c->discover_gen++;
    if (count > 0) {
        memcpy(c->discovered, found, sizeof(glm_context) * (size_t)count);
        c->discovered_count = count;
        c->discover_ok = 1;
        c->discover_error[0] = '\0';
    } else {
        c->discover_ok = 0;
        snprintf(c->discover_error, sizeof c->discover_error,
                 "没有发现机构/项目：JWT 可能已过期或无权限");
    }
    pthread_mutex_unlock(&c->lock);
}

/* ----------------------------------------------------------- thread entry */

typedef struct {
    glm_client *c;
    int discover;
    char web_jwt[2048];
} job;

static void *worker(void *arg) {
    job *j = arg;
    if (j->discover)
        discover_all(j->c, j->web_jwt);
    else
        fetch_all(j->c);
    pthread_mutex_lock(&j->c->lock);
    j->c->fetching = 0;
    pthread_mutex_unlock(&j->c->lock);
    free(j);
    return NULL;
}

static int start_job(glm_client *c, int discover, const char *web_jwt) {
    pthread_mutex_lock(&c->lock);
    if (c->fetching) { /* one job at a time; the UI shows a spinner meanwhile */
        pthread_mutex_unlock(&c->lock);
        return 0;
    }
    c->fetching = 1;
    if (discover) {
        c->discovering = 1;
        c->discover_ok = 0;
        c->discover_error[0] = '\0';
    } else {
        c->state = GLM_LOADING;
    }
    pthread_mutex_unlock(&c->lock);

    job *j = calloc(1, sizeof *j);
    j->c = c;
    j->discover = discover;
    if (web_jwt) snprintf(j->web_jwt, sizeof j->web_jwt, "%s", web_jwt);

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&tid, &attr, worker, j);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(j);
        pthread_mutex_lock(&c->lock);
        c->fetching = 0;
        c->state = GLM_ERROR;
        snprintf(c->error, sizeof c->error, "无法启动后台线程");
        pthread_mutex_unlock(&c->lock);
        return 0;
    }
    return 1;
}

void glm_client_refresh(glm_client *c) {
    if (c->demo) { demo_refresh(c); return; }
    start_job(c, 0, NULL);
}

void glm_client_discover(glm_client *c, const char *web_jwt) {
    if (!web_jwt || !web_jwt[0]) return;
    start_job(c, 1, web_jwt);
}

int glm_client_discover_gen(const glm_client *c) {
    pthread_mutex_lock((pthread_mutex_t *)&c->lock);
    int g = (int)c->discover_gen;
    pthread_mutex_unlock((pthread_mutex_t *)&c->lock);
    return g;
}

int glm_client_discover_result(glm_client *c, glm_context *out, int max, char *err,
                               size_t errcap) {
    pthread_mutex_lock(&c->lock);
    int n = c->discovered_count < max ? c->discovered_count : max;
    memcpy(out, c->discovered, sizeof(glm_context) * (size_t)n);
    if (err && errcap) snprintf(err, errcap, "%s", c->discover_error);
    pthread_mutex_unlock(&c->lock);
    return n;
}

int glm_client_discovering(const glm_client *c) {
    pthread_mutex_lock((pthread_mutex_t *)&c->lock);
    int d = c->discovering;
    pthread_mutex_unlock((pthread_mutex_t *)&c->lock);
    return d;
}

#else /* GLM_NO_NET */

void glm_client_refresh(glm_client *c) { demo_refresh(c); }

void glm_client_discover(glm_client *c, const char *web_jwt) { (void)c; (void)web_jwt; }

int glm_client_discover_gen(const glm_client *c) { (void)c; return 0; }

int glm_client_discover_result(glm_client *c, glm_context *out, int max, char *err, size_t errcap) {
    (void)c; (void)out; (void)max;
    if (err && errcap) err[0] = '\0';
    return 0;
}

int glm_client_discovering(const glm_client *c) { (void)c; return 0; }

#endif

/* ------------------------------------------------------------------ demo */

void glm_client_set_demo(glm_client *c) {
    c->demo = 1;
    demo_fill(c, 0);
    pthread_mutex_lock(&c->lock);
    c->state = GLM_OK;
    pthread_mutex_unlock(&c->lock);
}

static double demo_pct(double base, double jitter) {
    double v = base + jitter * ((double)(rand() % 200 - 100) / 100.0);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}

static void demo_fill(glm_client *c, int jitter) {
    glm_snapshot *s = &c->snap;
    memset(s, 0, sizeof *s);
    snprintf(s->level, sizeof s->level, "pro");

    glm_limit *l5 = &s->limits[0], *lw = &s->limits[1];
    snprintf(l5->kind, sizeof l5->kind, "TOKENS_LIMIT");
    l5->percentage = demo_pct(37.4, jitter * 1.2);
    l5->remaining = 5062120;
    l5->next_reset = now_epoch() + (2 * 3600 + 13 * 60) + jitter * 60;
    snprintf(lw->kind, sizeof lw->kind, "CREDIT_LIMIT");
    lw->percentage = demo_pct(12.8, jitter * 0.6);
    lw->remaining = 41048800;
    lw->next_reset = now_epoch() + (4 * 86400 + 6 * 3600) + jitter * 60;
    glm_limit *mcp = &s->limits[2];
    snprintf(mcp->kind, sizeof mcp->kind, "TIME_LIMIT");
    mcp->percentage = demo_pct(14.2, jitter * 0.3);
    mcp->current_value = 213;
    mcp->usage = 1500;
    s->limit_count = 3;

    s->has_resets = 1;
    s->five_resets = 2;
    s->week_resets = 1;

    s->has_account = 1;
    s->balance = 286.40;
    s->give_amount = 120.00;
    s->total_spend = 837.13;

    static const char *names[] = { "glm-4.6", "glm-4.5-air", "glm-4.5-flash", "glm-4.5v",
                                   "glm-4.6-flash" };
    static const double toks[] = { 18432000, 9644000, 7213000, 2310000, 867000 };
    static const double reqs[] = { 1284, 866, 1203, 97, 244 };
    s->model_count = 5;
    for (int i = 0; i < 5; i++) {
        snprintf(s->models[i].model, sizeof s->models[i].model, "%s", names[i]);
        s->models[i].tokens = toks[i];
        s->models[i].requests = reqs[i];
    }
    s->fetched_at = now_epoch();
}

static void demo_refresh(glm_client *c) {
    demo_fill(c, 1);
    pthread_mutex_lock(&c->lock);
    c->state = GLM_OK;
    c->error[0] = '\0';
    c->fetch_gen++;
    pthread_mutex_unlock(&c->lock);
}

/* ------------------------------------------------------------- formatting */

void glm_human_tokens(double v, char *out, size_t cap) {
    if (v >= 1e8) snprintf(out, cap, "%.2f 亿", v / 1e8);
    else if (v >= 1e4) snprintf(out, cap, "%.1f 万", v / 1e4);
    else snprintf(out, cap, "%.0f", v);
}

void glm_human_money(double v, char *out, size_t cap) { snprintf(out, cap, "¥%.2f", v); }

void glm_fmt_reset(long long epoch, long long now, char *out, size_t cap) {
    if (!epoch) { snprintf(out, cap, "重置时间未知"); return; }
    long long local = epoch + 8 * 3600; /* the quota console is China-based */
    time_t t = (time_t)local;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char when[32];
    strftime(when, sizeof when, "%m-%d %H:%M", &tmv);
    long long diff = epoch - now;
    if (diff <= 0) { snprintf(out, cap, "%s · 已重置", when); return; }
    char rel[48];
    if (diff >= 86400)
        snprintf(rel, sizeof rel, "%lld 天 %lld 小时后", diff / 86400, diff % 86400 / 3600);
    else if (diff >= 3600)
        snprintf(rel, sizeof rel, "%lld 小时 %lld 分后", diff / 3600, diff % 3600 / 60);
    else
        snprintf(rel, sizeof rel, "%lld 分 %lld 秒后", diff / 60, diff % 60);
    snprintf(out, cap, "%s · %s重置", when, rel);
}

void glm_fmt_clock(long long epoch, char *out, size_t cap) {
    time_t t = (time_t)epoch;
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(out, cap, "%H:%M:%S", &tmv);
}

int glm_usage_tone(double pct) {
    if (pct >= 85) return 2;
    if (pct >= 60) return 1;
    return 0;
}
