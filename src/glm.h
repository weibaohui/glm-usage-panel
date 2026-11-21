/* glm.h — GLM (Zhipu BigModel) coding-plan usage client: config, data model,
 * background fetching over libcurl, and a few formatting helpers for the UI. */
#ifndef GLM_CLIENT_H
#define GLM_CLIENT_H

#include <stddef.h>

#define GLM_MAX_CONTEXTS 16
#define GLM_MAX_LIMITS 8
#define GLM_MAX_MODELS 24

typedef struct {
    char name[64];
    char org[80];          /* "org-xxxx", may be empty: default personal context */
    char project[80];      /* "proj-xxxx" */
    int project_type;      /* 1 personal, 2 team coding plan */
} glm_context;

typedef enum { GLM_AUTH_APIKEY = 0, GLM_AUTH_TOKEN = 1 } glm_auth_mode;

typedef struct {
    int auth_mode;                        /* GLM_AUTH_APIKEY: sign a fresh JWT per request;
                                           * GLM_AUTH_TOKEN: send a pre-issued token as-is */
    char api_key[512];                    /* "<id>.<secret>" */
    char token[2048];                     /* coding-plan token or a pre-issued JWT */
    char base_url[256];                   /* default https://open.bigmodel.cn */
    char web_url[256];                    /* default https://bigmodel.cn, for discovery */
    glm_context contexts[GLM_MAX_CONTEXTS];
    int context_count;
    int selected;
    int refresh_seconds;                  /* auto-refresh interval, >= 15 */
    int auto_refresh;                     /* bool */
    int dark;                             /* bool: start in the dark theme */
} glm_config;

typedef struct {
    char kind[24];                        /* TOKENS_LIMIT / CREDIT_LIMIT / TIME_LIMIT */
    double percentage;                    /* used, 0..100 */
    double remaining;                     /* tokens left */
    double current_value, usage;          /* TIME_LIMIT counters */
    long long next_reset;                 /* epoch seconds, 0 = unknown */
} glm_limit;

typedef struct {
    char model[48];
    double tokens;
    double requests;
} glm_model_usage;

typedef struct {
    glm_limit limits[GLM_MAX_LIMITS];
    int limit_count;
    char level[24];                       /* "pro", "max", ... when the API returns one */
    int no_plan;                          /* this org has no coding plan: show the account view */
    int has_account;
    double balance, give_amount, total_spend;
    int has_resets;
    int five_resets, week_resets;
    glm_model_usage models[GLM_MAX_MODELS];
    int model_count;
    char hint[256];                       /* non-fatal remark, e.g. "接口返回空数据" */
    long long fetched_at;                 /* epoch seconds */
} glm_snapshot;

enum { GLM_IDLE = 0, GLM_LOADING, GLM_OK, GLM_ERROR };

typedef struct glm_client glm_client;

glm_client *glm_client_create(void);
void glm_client_destroy(glm_client *c);

/* Config: load/save the JSON file (path overridable with GLM_CONFIG for tests),
 * and hand an edited copy back to the client. Applying the config starts a
 * refresh when the client is configured. */
void glm_config_defaults(glm_config *cfg);
void glm_config_load(glm_config *cfg);
int glm_config_save(const glm_config *cfg);       /* false when writing failed */
const char *glm_config_path(void);
void glm_client_get_config(glm_client *c, glm_config *out);
void glm_client_apply_config(glm_client *c, const glm_config *cfg);

void glm_client_refresh(glm_client *c);           /* async: one fetch at a time */
int glm_client_state(const glm_client *c);
const char *glm_client_error(const glm_client *c);
int glm_client_snapshot(glm_client *c, glm_snapshot *out);   /* copy of the latest data */
int glm_client_fetch_gen(const glm_client *c);    /* increments per finished fetch */

void glm_client_set_demo(glm_client *c);          /* --demo: never touch the network */
int glm_client_is_demo(const glm_client *c);

/* One-shot org/project discovery with a web JWT (async, like refresh). Results
 * land in out; call with a fresh buffer every frame and check gen. */
void glm_client_discover(glm_client *c, const char *web_jwt);
int glm_client_discover_gen(const glm_client *c);
int glm_client_discover_result(glm_client *c, glm_context *out, int max, char *err, size_t errcap);
int glm_client_discovering(const glm_client *c);

/* Formatting helpers for the UI. */
void glm_human_tokens(double v, char *out, size_t cap);      /* 3.2万 / 1.1亿 / 8421 */
void glm_human_money(double v, char *out, size_t cap);       /* ¥16.47 */
void glm_fmt_reset(long long epoch, long long now, char *out, size_t cap); /* "11-21 18:00 · 2小时13分后" */
void glm_fmt_clock(long long epoch, char *out, size_t cap);  /* "14:32:05" */

/* Tone buckets shared by the UI. */
int glm_usage_tone(double pct); /* 0 calm, 1 warn, 2 hot */

#endif
