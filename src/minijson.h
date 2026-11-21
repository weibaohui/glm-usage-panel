/* minijson.h — a tiny arena-based JSON parser, just enough for the GLM usage API
 * responses and the app's own config file. Strings are unescaped into a string
 * arena and always NUL-terminated, so mj_strz() is safe. */
#ifndef GLM_MINIJSON_H
#define GLM_MINIJSON_H

#include <stddef.h>

typedef enum { MJ_NULL = 0, MJ_BOOL, MJ_NUM, MJ_STR, MJ_ARR, MJ_OBJ } mj_type;

typedef struct mj_val {
    mj_type type;
    double num;             /* MJ_NUM */
    int boolean;            /* MJ_BOOL */
    const char *str;        /* MJ_STR: unescaped, NUL-terminated */
    int str_len;
    struct mj_val *child;   /* MJ_ARR / MJ_OBJ: first child */
    struct mj_val *next;    /* sibling */
    const char *key;        /* MJ_OBJ children: their key */
} mj_val;

typedef struct mj_parser {
    const char *p, *end;
    int depth;
    int err;
    /* arenas: nodes and unescaped strings come from here, no frees per value */
    char *node_arena;
    size_t node_cap, node_used;
    char *str_arena;
    size_t str_cap, str_used;
} mj_parser;

/* Parse one JSON document. arena buffers are caller-owned memory. Returns the
 * root value or NULL (parse error, or an arena ran out). */
mj_val *mj_parse(mj_parser *p, const char *json, size_t len, char *node_arena, size_t node_cap,
                 char *str_arena, size_t str_cap);

mj_val *mj_get(const mj_val *obj, const char *key);   /* MJ_OBJ lookup, NULL when missing */
mj_val *mj_at(const mj_val *arr, int index);          /* MJ_ARR element, NULL out of range */
int mj_count(const mj_val *arr);                      /* children of MJ_ARR / MJ_OBJ */

/* Convenience readers: NULL/missing-typed values are safe. */
const char *mj_strz(const mj_val *v);                 /* "" when not a string */
double mj_num(const mj_val *v);                       /* numbers and numeric strings */
long long mj_ll(const mj_val *v);
int mj_bool(const mj_val *v);                         /* 0 unless true */

#endif
