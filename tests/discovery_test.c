/* discovery_test.c — exercises the web-JWT discovery path and the
 * silent-empty-data pitfall against the mock server. Build:
 *   cc -I src tests/discovery_test.c src/glm.c src/crypto.c src/minijson.c \
 *      $(curl-config --cflags --libs) -o /tmp/discovery_test -lpthread
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "glm.h"

static void wait_discover(glm_client *c) {
    for (int i = 0; i < 100; i++) {
        if (!glm_client_discovering(c)) return;
        usleep(100 * 1000);
    }
    fprintf(stderr, "discovery timed out\n");
}

int main(void) {
    glm_client *c = glm_client_create();

    /* point the client at the mock before discovering */
    glm_config setup;
    glm_config_defaults(&setup);
    snprintf(setup.base_url, sizeof setup.base_url, "http://127.0.0.1:8791");
    snprintf(setup.web_url, sizeof setup.web_url, "http://127.0.0.1:8791");
    glm_client_apply_config(c, &setup);
    usleep(50 * 1000); /* let its first refresh round finish or fail quietly */

    /* 1) discovery with a web JWT (the mock accepts any Bearer token) */
    glm_client_discover(c, "Bearer eyJfake.test.token");
    wait_discover(c);
    glm_context found[GLM_MAX_CONTEXTS];
    char err[256];
    int n = glm_client_discover_result(c, found, GLM_MAX_CONTEXTS, err, sizeof err);
    if (n != 2) {
        fprintf(stderr, "FAIL: expected 2 discovered contexts, got %d (%s)\n", n, err);
        return 1;
    }
    if (strcmp(found[0].org, "org-team1") || strcmp(found[0].project, "proj-team1") ||
        found[0].project_type != 2 || !strstr(found[0].name, "团队")) {
        fprintf(stderr, "FAIL: context[0] wrong: %s / %s / %d / %s\n", found[0].org,
                found[0].project, found[0].project_type, found[0].name);
        return 1;
    }
    if (strcmp(found[1].org, "org-personal") || found[1].project_type != 1) {
        fprintf(stderr, "FAIL: context[1] wrong: %s / %d\n", found[1].org, found[1].project_type);
        return 1;
    }
    printf("discovery: 2 contexts OK (team first fields verified)\n");

    /* 2) the silent-empty pitfall: quota with org-decoy returns code 200 + {} —
     *    the snapshot must end up empty with a hint, not an error */
    glm_config cfg;
    glm_config_defaults(&cfg);
    snprintf(cfg.base_url, sizeof cfg.base_url, "http://127.0.0.1:8791");
    snprintf(cfg.web_url, sizeof cfg.web_url, "http://127.0.0.1:8791");
    cfg.auth_mode = GLM_AUTH_APIKEY;
    snprintf(cfg.api_key, sizeof cfg.api_key, "fakeid.fakesecret"); /* mints a fresh JWT */
    glm_context decoy;
    memset(&decoy, 0, sizeof decoy);
    snprintf(decoy.name, sizeof decoy.name, "decoy");
    snprintf(decoy.org, sizeof decoy.org, "org-decoy");
    snprintf(decoy.project, sizeof decoy.project, "proj-decoy");
    decoy.project_type = 2;
    cfg.contexts[0] = decoy;
    cfg.context_count = 1;
    cfg.auto_refresh = 0;
    glm_client_apply_config(c, &cfg);

    for (int i = 0; i < 200; i++) {
        if (glm_client_state(c) == GLM_OK || glm_client_state(c) == GLM_ERROR) break;
        usleep(100 * 1000);
    }
    glm_snapshot snap;
    glm_client_snapshot(c, &snap);
    if (glm_client_state(c) != GLM_OK || snap.limit_count != 0 || !snap.hint[0]) {
        fprintf(stderr, "FAIL: decoy org: state=%d limits=%d hint=%s error=%s\n",
                glm_client_state(c), snap.limit_count, snap.hint, glm_client_error(c));
        return 1;
    }
    printf("decoy org: empty data surfaced as hint OK (\"%s\")\n", snap.hint);

    glm_client_destroy(c);
    printf("all discovery tests passed\n");
    return 0;
}
