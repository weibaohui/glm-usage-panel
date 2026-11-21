#include "minijson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- allocation */

static void *mj_nodes(mj_parser *p) {
    if (p->node_used + sizeof(mj_val) > p->node_cap) { p->err = 1; return NULL; }
    void *mem = p->node_arena + p->node_used;
    p->node_used += sizeof(mj_val);
    memset(mem, 0, sizeof(mj_val));
    return mem;
}

static char *mj_string_space(mj_parser *p, size_t n) {
    if (p->str_used + n + 1 > p->str_cap) { p->err = 1; return NULL; }
    char *mem = p->str_arena + p->str_used;
    p->str_used += n + 1;
    return mem;
}

/* ---------------------------------------------------------------- helpers */

static void mj_skip_ws(mj_parser *p) {
    while (p->p < p->end && (*p->p == ' ' || *p->p == '\t' || *p->p == '\n' || *p->p == '\r'))
        p->p++;
}

static mj_val *mj_value(mj_parser *p);

static int mj_hex4(mj_parser *p, unsigned *out) {
    if (p->end - p->p < 4) return 0;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p->p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    p->p += 4;
    *out = v;
    return 1;
}

static int mj_utf8_put(char *out, size_t *used, unsigned cp) {
    if (cp < 0x80) out[(*used)++] = (char)cp;
    else if (cp < 0x800) {
        out[(*used)++] = (char)(0xc0 | (cp >> 6));
        out[(*used)++] = (char)(0x80 | (cp & 63));
    } else if (cp < 0x10000) {
        out[(*used)++] = (char)(0xe0 | (cp >> 12));
        out[(*used)++] = (char)(0x80 | ((cp >> 6) & 63));
        out[(*used)++] = (char)(0x80 | (cp & 63));
    } else {
        out[(*used)++] = (char)(0xf0 | (cp >> 18));
        out[(*used)++] = (char)(0x80 | ((cp >> 12) & 63));
        out[(*used)++] = (char)(0x80 | ((cp >> 6) & 63));
        out[(*used)++] = (char)(0x80 | (cp & 63));
    }
    return 1;
}

/* Reads a string into the string arena. p->p sits on the opening quote. */
static const char *mj_read_string(mj_parser *p, int *len_out) {
    p->p++; /* opening quote */
    /* worst case every byte is an escape; reserve conservatively */
    size_t avail = (size_t)(p->end - p->p);
    char *dst = NULL;
    size_t used = 0;
    dst = mj_string_space(p, avail);
    if (!dst) return NULL;
    while (p->p < p->end && *p->p != '"') {
        char c = *p->p;
        if (c == '\\') {
            p->p++;
            if (p->p >= p->end) { p->err = 1; return NULL; }
            char e = *p->p++;
            switch (e) {
            case '"': dst[used++] = '"'; break;
            case '\\': dst[used++] = '\\'; break;
            case '/': dst[used++] = '/'; break;
            case 'b': dst[used++] = '\b'; break;
            case 'f': dst[used++] = '\f'; break;
            case 'n': dst[used++] = '\n'; break;
            case 'r': dst[used++] = '\r'; break;
            case 't': dst[used++] = '\t'; break;
            case 'u': {
                unsigned cp;
                if (!mj_hex4(p, &cp)) { p->err = 1; return NULL; }
                if (cp >= 0xd800 && cp <= 0xdbff && p->end - p->p >= 6 && p->p[0] == '\\' &&
                    p->p[1] == 'u') { /* surrogate pair */
                    p->p += 2;
                    unsigned lo;
                    if (!mj_hex4(p, &lo) || lo < 0xdc00 || lo > 0xdfff) { p->err = 1; return NULL; }
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                }
                mj_utf8_put(dst, &used, cp);
                break;
            }
            default: p->err = 1; return NULL;
            }
        } else {
            dst[used++] = c;
            p->p++;
        }
    }
    if (p->p >= p->end) { p->err = 1; return NULL; }
    p->p++; /* closing quote */
    dst[used] = '\0';
    if (len_out) *len_out = (int)used;
    return dst;
}

static mj_val *mj_value(mj_parser *p) {
    mj_skip_ws(p);
    if (p->p >= p->end || p->depth > 32) { p->err = 1; return NULL; }

    mj_val *v = mj_nodes(p);
    if (!v) return NULL;

    if (*p->p == '{') {
        v->type = MJ_OBJ;
        p->p++;
        mj_skip_ws(p);
        mj_val *last = NULL;
        if (p->p < p->end && *p->p == '}') { p->p++; return v; }
        for (;;) {
            p->depth++;
            mj_skip_ws(p);
            if (p->p >= p->end || *p->p != '"') { p->err = 1; return NULL; }
            int klen = 0;
            const char *key = mj_read_string(p, &klen);
            if (!key) return NULL;
            mj_skip_ws(p);
            if (p->p >= p->end || *p->p != ':') { p->err = 1; return NULL; }
            p->p++;
            mj_val *item = mj_value(p);
            if (!item) return NULL;
            item->key = key;
            if (last) last->next = item;
            else v->child = item;
            last = item;
            mj_skip_ws(p);
            p->depth--;
            if (p->p < p->end && *p->p == ',') { p->p++; continue; }
            if (p->p < p->end && *p->p == '}') { p->p++; return v; }
            p->err = 1;
            return NULL;
        }
    }

    if (*p->p == '[') {
        v->type = MJ_ARR;
        p->p++;
        mj_skip_ws(p);
        mj_val *last = NULL;
        if (p->p < p->end && *p->p == ']') { p->p++; return v; }
        for (;;) {
            p->depth++;
            mj_val *item = mj_value(p);
            if (!item) return NULL;
            if (last) last->next = item;
            else v->child = item;
            last = item;
            mj_skip_ws(p);
            p->depth--;
            if (p->p < p->end && *p->p == ',') { p->p++; continue; }
            if (p->p < p->end && *p->p == ']') { p->p++; return v; }
            p->err = 1;
            return NULL;
        }
    }

    if (*p->p == '"') {
        v->type = MJ_STR;
        int len = 0;
        v->str = mj_read_string(p, &len);
        v->str_len = len;
        if (!v->str) return NULL;
        return v;
    }

    if (p->end - p->p >= 4 && !memcmp(p->p, "true", 4)) {
        v->type = MJ_BOOL; v->boolean = 1; p->p += 4; return v;
    }
    if (p->end - p->p >= 5 && !memcmp(p->p, "false", 5)) {
        v->type = MJ_BOOL; v->boolean = 0; p->p += 5; return v;
    }
    if (p->end - p->p >= 4 && !memcmp(p->p, "null", 4)) {
        v->type = MJ_NULL; p->p += 4; return v;
    }

    /* number */
    {
        char buf[40];
        size_t n = 0;
        while (p->p < p->end && n + 1 < sizeof buf &&
               ((*p->p >= '0' && *p->p <= '9') || *p->p == '-' || *p->p == '+' || *p->p == '.' ||
                *p->p == 'e' || *p->p == 'E'))
            buf[n++] = *p->p++;
        if (!n) { p->err = 1; return NULL; }
        buf[n] = '\0';
        v->type = MJ_NUM;
        v->num = strtod(buf, NULL);
        return v;
    }
}

mj_val *mj_parse(mj_parser *p, const char *json, size_t len, char *node_arena, size_t node_cap,
                 char *str_arena, size_t str_cap) {
    p->p = json;
    p->end = json + len;
    p->depth = 0;
    p->err = 0;
    p->node_arena = node_arena;
    p->node_cap = node_cap;
    p->node_used = 0;
    p->str_arena = str_arena;
    p->str_cap = str_cap;
    p->str_used = 0;
    mj_val *root = mj_value(p);
    if (p->err) return NULL;
    return root;
}

/* ------------------------------------------------------------- accessors */

mj_val *mj_get(const mj_val *obj, const char *key) {
    if (!obj || obj->type != MJ_OBJ) return NULL;
    for (mj_val *c = obj->child; c; c = c->next)
        if (c->key && !strcmp(c->key, key)) return c;
    return NULL;
}

mj_val *mj_at(const mj_val *arr, int index) {
    if (!arr || arr->type != MJ_ARR) return NULL;
    int i = 0;
    for (mj_val *c = arr->child; c; c = c->next)
        if (i++ == index) return c;
    return NULL;
}

int mj_count(const mj_val *arr) {
    if (!arr || (arr->type != MJ_ARR && arr->type != MJ_OBJ)) return 0;
    int n = 0;
    for (mj_val *c = arr->child; c; c = c->next) n++;
    return n;
}

const char *mj_strz(const mj_val *v) { return v && v->type == MJ_STR && v->str ? v->str : ""; }

double mj_num(const mj_val *v) {
    if (!v) return 0;
    if (v->type == MJ_NUM) return v->num;
    if (v->type == MJ_STR && v->str) return strtod(v->str, NULL);
    if (v->type == MJ_BOOL) return v->boolean;
    return 0;
}

long long mj_ll(const mj_val *v) { return (long long)mj_num(v); }

int mj_bool(const mj_val *v) {
    if (!v) return 0;
    if (v->type == MJ_BOOL) return v->boolean;
    if (v->type == MJ_STR && v->str) return strcmp(v->str, "true") == 0 || strcmp(v->str, "1") == 0;
    return 0;
}
