/* main.c — entry point. glm-usage-panel [--demo]
 * --demo: canned data, no network, handy for screenshots and CI. */
#include <stdio.h>
#include <string.h>

#include "glm.h"
#include "ui.h"

int main(int argc, char **argv) {
    bool demo = false;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--demo")) demo = true;

    glm_client *glm = glm_client_create();
    if (demo) glm_client_set_demo(glm); /* before applying: apply_config triggers a refresh */

    glm_config cfg;
    glm_config_load(&cfg);
    glm_client_apply_config(glm, &cfg);

    if (demo) glm_client_refresh(glm); /* one round so the figures animate in */

    ui_run(glm, demo);
    glm_client_destroy(glm);
    return 0;
}
