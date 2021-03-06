/**
 * @file sysrepoctl.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief sysrepoctl tool
 *
 * @copyright
 * Copyright 2018 Deutsche Telekom AG.
 * Copyright 2018 - 2019 CESNET, z.s.p.o.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define _GNU_SOURCE

#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <getopt.h>

#include <libyang/libyang.h>

#include "sysrepo.h"
#include "bin_common.h"

#define SRCTL_LIST_NAME "Module Name"
#define SRCTL_LIST_REVISION "Revision"
#define SRCTL_LIST_FLAGS "Flags"
#define SRCTL_LIST_OWNER "Owner"
#define SRCTL_LIST_PERMS "Permissions"
#define SRCTL_LIST_SUBMODS "Submodules"
#define SRCTL_LIST_FEATURES "Features"

struct list_item {
    char *name;
    char *revision;
    const char *impl_flag;
    int replay;
    const char *change_flag;
    int feat_changes;
    char *owner;
    mode_t perms;
    char *submodules;
    char *features;
};

static void
version_print(void)
{
    printf(
        "sysrepocfg - sysrepo YANG schema manipulation tool, compiled with libsysrepo v%s (SO v%s)\n"
        "\n",
        SR_VERSION, SR_SOVERSION
    );
}

static void
help_print(void)
{
    printf(
        "Usage:\n"
        "  sysrepoctl <operation-option> [other-options]\n"
        "\n"
        "Available operation-options:\n"
        "  -h, --help           Print usage help.\n"
        "  -V, --version        Print only information about sysrepo version.\n"
        "  -l, --list           List YANG modules in sysrepo.\n"
        "  -i, --install <path> Install the specified schema into sysrepo. Can be in either YANG or YIN format.\n"
        "  -u, --uninstall <module>[,<module2>,<module3> ...]\n"
        "                       Uninstall the specified module(s) from sysrepo.\n"
        "  -c, --change <module>\n"
        "                       Change access rights, features, or replay support of the specified module.\n"
        "  -U, --update <path>  Update the specified schema in sysrepo. Can be in either YANG or YIN format.\n"
        "  -C, --connection-count\n"
        "                       Print the number of sysrepo connections to STDOUT.\n"
        "  -R, --recover        Check current connections state and clean any non-existing ones.\n"
        "\n"
        "Available other-options:\n"
        "  -s, --search-dir <dir-path>\n"
        "                       Directory to search for include/import modules. Directory with already-installed\n"
        "                       modules is always searched (install, update op).\n"
        "  -e, --enable-feature <feature-name>\n"
        "                       Enabled specific feature. Can be specified multiple times (install, change op).\n"
        "  -d, --disable-feature <feature-name>\n"
        "                       Disable specific feature. Can be specified multiple times (change op).\n"
        "  -r, --replay <state> Change replay support (storing notifications) for this module to on/off or 1/0 (change op).\n"
        "  -o, --owner <user>   Set filesystem owner of a module (change op).\n"
        "  -g, --group <group>  Set filesystem group of a module (change op).\n"
        "  -p, --permissions <permissions>\n"
        "                       Set filesystem permissions of a module (chmod format) (change op).\n"
        "  -v, --verbosity <level>\n"
        "                       Change verbosity to a level (none, error, warning, info, debug) or number (0, 1, 2, 3, 4).\n"
        "\n"
    );
}

static void
error_print(int sr_error, const char *format, ...)
{
    va_list ap;
    char msg[2048];

    if (!sr_error) {
        sprintf(msg, "sysrepoctl error: %s\n", format);
    } else {
        sprintf(msg, "sysrepoctl error: %s (%s)\n", format, sr_strerror(sr_error));
    }

    va_start(ap, format);
    vfprintf(stderr, msg, ap);
    va_end(ap);
}

static void
srctl_list_collect_import(const struct lys_module *ly_mod, struct list_item **list, size_t *list_count)
{
    struct list_item *cur_item;
    const struct lys_module *ly_iter;
    uint32_t i;

    if (ly_mod->implemented) {
        /* must be added from sysrepo data */
        return;
    }

    /* skip internal modules */
    i = 0;
    while (i < ly_ctx_internal_modules_count(ly_mod->ctx)) {
        ly_iter = ly_ctx_get_module_iter(ly_mod->ctx, &i);
        if (ly_iter == ly_mod) {
            return;
        }
    }

    /* check for duplicates */
    for (i = 0; i < *list_count; ++i) {
        if (strcmp((*list)[i].name, ly_mod->name)) {
            continue;
        }
        if (strcmp((*list)[i].revision, ly_mod->rev_size ? ly_mod->rev[0].date : "")) {
            continue;
        }

        return;
    }

    /* new module */
    *list = realloc(*list, (*list_count + 1) * sizeof **list);
    cur_item = &(*list)[*list_count];
    ++(*list_count);

    /* init */
    memset(cur_item, 0, sizeof *cur_item);
    cur_item->impl_flag = "i";
    cur_item->change_flag = "";
    cur_item->owner = strdup("");
    cur_item->submodules = strdup("");
    cur_item->features = strdup("");

    /* fill name and revision */
    cur_item->name = strdup(ly_mod->name);
    cur_item->revision = strdup(ly_mod->rev_size ? ly_mod->rev[0].date : "");

    /* recursively */
    for (i = 0; i < ly_mod->imp_size; ++i) {
        srctl_list_collect_import(ly_mod->imp[i].module, list, list_count);
    }
}

static int
srctl_list_collect(sr_conn_ctx_t *conn, struct lyd_node *sr_data, const struct ly_ctx *ly_ctx, struct list_item **list,
        size_t *list_count)
{
    struct list_item *cur_item;
    const struct lys_module *ly_mod;
    struct lyd_node *module, *child;
    char *owner, *group;
    const char *str;
    int ret = SR_ERR_OK;
    uint32_t i;

    LY_TREE_FOR(sr_data->child, module) {
        /* new module */
        *list = realloc(*list, (*list_count + 1) * sizeof **list);
        cur_item = &(*list)[*list_count];
        ++(*list_count);

        /* init */
        memset(cur_item, 0, sizeof *cur_item);
        cur_item->impl_flag = "";
        cur_item->change_flag = "";
        cur_item->submodules = strdup("");
        cur_item->features = strdup("");

        /* collect information from sysrepo data */
        LY_TREE_FOR(module->child, child) {
            if (!strcmp(child->schema->name, "name")) {
                str = ((struct lyd_node_leaf_list *)child)->value_str;
                cur_item->name = strdup(str);
            } else if (!strcmp(child->schema->name, "revision")) {
                str = ((struct lyd_node_leaf_list *)child)->value_str;
                cur_item->revision = strdup(str);
            } else if (!strcmp(child->schema->name, "replay-support")) {
                cur_item->replay = 1;
            } else if (!strcmp(child->schema->name, "removed")) {
                cur_item->change_flag = "R";
            } else if (!strcmp(child->schema->name, "updated-yang")) {
                cur_item->change_flag = "U";
            } else if (!strcmp(child->schema->name, "enabled-feature")) {
                str = ((struct lyd_node_leaf_list *)child)->value_str;
                cur_item->features = realloc(cur_item->features, strlen(cur_item->features) + 1 + strlen(str) + 1);
                if (cur_item->features[0]) {
                    strcat(cur_item->features, " ");
                }
                strcat(cur_item->features, str);
            } else if (!strcmp(child->schema->name, "changed-feature")) {
                cur_item->feat_changes = 1;
            }
        }

        if (!strcmp(module->schema->name, "module")) {
            cur_item->impl_flag = "I";

            /* get owner and permissions */
            if ((ret = sr_get_module_access(conn, cur_item->name, &owner, &group, &cur_item->perms)) != SR_ERR_OK) {
                return ret;
            }
            cur_item->owner = malloc(strlen(owner) + 1 + strlen(group) + 1);
            sprintf(cur_item->owner, "%s:%s", owner, group);
            free(owner);
            free(group);

            /* learn submodules */
            ly_mod = ly_ctx_get_module(ly_ctx, cur_item->name, cur_item->revision, 1);
            if (!ly_mod) {
                return SR_ERR_INTERNAL;
            }
            for (i = 0; i < ly_mod->inc_size; ++i) {
                str = ly_mod->inc[i].submodule->name;
                cur_item->submodules = realloc(cur_item->submodules, strlen(cur_item->submodules) + 1 + strlen(str) + 1);
                if (i) {
                    strcat(cur_item->submodules, " ");
                }
                strcat(cur_item->submodules, str);
            }

            /* set empty revision if no specified */
            if (!cur_item->revision) {
                cur_item->revision = strdup("");
            }
        } else {
            cur_item->change_flag = "N";
            cur_item->owner = strdup("");
            cur_item->revision = strdup("");
        }

        /* add all import modules as well */
        for (i = 0; i < ly_mod->imp_size; ++i) {
            srctl_list_collect_import(ly_mod->imp[i].module, list, list_count);
        }
    }

    return SR_ERR_OK;
}

static int
srctl_list_cmp(const void *ptr1, const void *ptr2)
{
    struct list_item *item1, *item2;

    item1 = (struct list_item *)ptr1;
    item2 = (struct list_item *)ptr2;

    /* sort alphabetically */
    return strcmp(item1->name, item2->name);
}

static int
srctl_list(sr_conn_ctx_t *conn)
{
    int ret;
    const struct ly_ctx *ly_ctx;
    char flags_str[5], perm_str[4];
    struct lyd_node *data = NULL;
    struct list_item *list = NULL;
    size_t i, line_len, list_count = 0;
    int max_name_len, max_owner_len, max_submod_len, max_feat_len;
    int rev_len, flag_len, perm_len;

    /* get context */
    ly_ctx = sr_get_context(conn);

    /* get SR module info data */
    if ((ret = sr_get_module_info(conn, &data)) != SR_ERR_OK) {
        goto cleanup;
    }

    /* collect all modules */
    if ((ret = srctl_list_collect(conn, data, ly_ctx, &list, &list_count))) {
        goto cleanup;
    }

    /* sort */
    qsort(list, list_count, sizeof *list, srctl_list_cmp);

    /* learn max lengths */
    max_name_len = strlen(SRCTL_LIST_NAME);
    rev_len = 10;
    flag_len = strlen(SRCTL_LIST_FLAGS);
    max_owner_len = strlen(SRCTL_LIST_OWNER);
    perm_len = strlen(SRCTL_LIST_PERMS);
    max_submod_len = strlen(SRCTL_LIST_SUBMODS);
    max_feat_len = strlen(SRCTL_LIST_FEATURES);
    for (i = 0; i < list_count; ++i) {
        if ((int)strlen(list[i].name) > max_name_len) {
            max_name_len = strlen(list[i].name);
        }
        if ((int)strlen(list[i].owner) > max_owner_len) {
            max_owner_len = strlen(list[i].owner);
        }
        if ((int)strlen(list[i].submodules) > max_submod_len) {
            max_submod_len = strlen(list[i].submodules);
        }
        if ((int)strlen(list[i].features) > max_feat_len) {
            max_feat_len = strlen(list[i].features);
        }
    }

    /* print repository info */
    printf("Sysrepo repository: %s\n\n", sr_get_repo_path());

    /* print header */
    printf("%-*s | %-*s | %-*s | %-*s | %-*s | %-*s | %-*s\n", max_name_len, "Module Name", rev_len, "Revision",
            flag_len, "Flags", max_owner_len, "Owner", perm_len, "Permissions", max_submod_len, "Submodules",
            max_feat_len, "Features");

    /* print ruler */
    line_len = max_name_len + 3 + rev_len + 3 + flag_len + 3 + max_owner_len + 3 + perm_len + 3 + max_submod_len + 3
            + max_feat_len;
    for (i = 0; i < line_len; ++i) {
        printf("-");
    }
    printf("\n");

    /* print modules */
    for (i = 0; i < list_count; ++i) {
        sprintf(flags_str, "%s%s%s%s", list[i].impl_flag, list[i].replay ? "R" : " ", list[i].change_flag,
                list[i].feat_changes ? "F" : "");
        if (!strcmp(list[i].impl_flag, "I")) {
            sprintf(perm_str, "%03o", list[i].perms);
        } else {
            perm_str[0] = '\0';
        }
        printf("%-*s | %-*s | %-*s | %-*s | %-*s | %-*s | %-*s\n", max_name_len, list[i].name, rev_len, list[i].revision,
            flag_len, flags_str, max_owner_len, list[i].owner, perm_len, perm_str, max_submod_len, list[i].submodules,
            max_feat_len, list[i].features);
    }

    /* print flag legend */
    printf("\nFlags meaning: I - Installed/i - Imported; R - Replay support; N - New/X - Removed/U - Updated; F - Feature changes\n\n");

cleanup:
    lyd_free_withsiblings(data);
    for (i = 0; i < list_count; ++i) {
        free(list[i].name);
        free(list[i].revision);
        free(list[i].owner);
        free(list[i].submodules);
        free(list[i].features);
    }
    free(list);
    return ret;
}

int
main(int argc, char** argv)
{
    sr_conn_ctx_t *conn = NULL;
    const char *file_path = NULL, *search_dir = NULL, *module_name = NULL, *owner = NULL, *group = NULL;
    char **features = NULL, **dis_features = NULL, *ptr;
    mode_t perms = -1;
    sr_log_level_t log_level = SR_LL_ERR;
    int r, i, rc = EXIT_FAILURE, opt, operation = 0, feat_count = 0, dis_feat_count = 0, replay = -1;
    uint32_t conn_count;
    struct option options[] = {
        {"help",            no_argument,       NULL, 'h'},
        {"version",         no_argument,       NULL, 'V'},
        {"list",            no_argument,       NULL, 'l'},
        {"install",         required_argument, NULL, 'i'},
        {"uninstall",       required_argument, NULL, 'u'},
        {"change",          required_argument, NULL, 'c'},
        {"update",          required_argument, NULL, 'U'},
        {"connection-count",no_argument,       NULL, 'C'},
        {"recover",         no_argument,       NULL, 'R'},
        {"search-dir",      required_argument, NULL, 's'},
        {"enable-feature",  required_argument, NULL, 'e'},
        {"disable-feature", required_argument, NULL, 'd'},
        {"replay",          required_argument, NULL, 'r'},
        {"owner",           required_argument, NULL, 'o'},
        {"group",           required_argument, NULL, 'g'},
        {"permissions",     required_argument, NULL, 'p'},
        {"verbosity",       required_argument, NULL, 'v'},
        {NULL,              0,                 NULL, 0},
    };

    if (argc == 1) {
        help_print();
        goto cleanup;
    }

    /* process options */
    opterr = 0;
    while ((opt = getopt_long(argc, argv, "hVli:u:c:U:CRs:e:d:r:o:g:p:v:", options, NULL)) != -1) {
        switch (opt) {
        case 'h':
            version_print();
            help_print();
            rc = EXIT_SUCCESS;
            goto cleanup;
        case 'V':
            version_print();
            rc = EXIT_SUCCESS;
            goto cleanup;
        case 'l':
            if (operation) {
                error_print(0, "Operation already specified");
                goto cleanup;
            }
            operation = 'l';
            break;
        case 'i':
            if (operation) {
                error_print(0, "Operation already specified");
                goto cleanup;
            }
            operation = 'i';
            file_path = optarg;
            break;
        case 'u':
            if (operation) {
                error_print(0, "Operation already specified");
                goto cleanup;
            }
            operation = 'u';
            module_name = optarg;
            break;
        case 'c':
            if (operation) {
                error_print(0, "Operation already specified");
                goto cleanup;
            }
            operation = 'c';
            module_name = optarg;
            break;
        case 'U':
            if (operation) {
                error_print(0, "Operation already specified");
                goto cleanup;
            }
            operation = 'U';
            file_path = optarg;
            break;
        case 'C':
            if (operation) {
                error_print(0, "Operation already specified");
                goto cleanup;
            }
            operation = 'C';
            break;
        case 'R':
            if (operation) {
                error_print(0, "Operation already specified");
                goto cleanup;
            }
            operation = 'R';
            break;
        case 's':
            if (search_dir) {
                error_print(0, "Search dir already specified");
                goto cleanup;
            }
            search_dir = optarg;
            break;
        case 'e':
            features = realloc(features, (feat_count + 1) * sizeof *features);
            features[feat_count++] = optarg;
            break;
        case 'd':
            dis_features = realloc(dis_features, (dis_feat_count + 1) * sizeof *dis_features);
            dis_features[dis_feat_count++] = optarg;
            break;
        case 'r':
            if (!strcmp(optarg, "on") || !strcmp(optarg, "1")) {
                replay = 1;
            } else if (!strcmp(optarg, "off") || !strcmp(optarg, "0")) {
                replay = 0;
            } else {
                error_print(0, "Invalid replay support \"%s\"", optarg);
                goto cleanup;
            }
            break;
        case 'o':
            if (owner) {
                error_print(0, "Owner already specified");
                goto cleanup;
            }
            owner = optarg;
            break;
        case 'g':
            if (group) {
                error_print(0, "Group already specified");
                goto cleanup;
            }
            group = optarg;
            break;
        case 'p':
            if ((int)perms != -1) {
                error_print(0, "Permissions already specified");
                goto cleanup;
            }
            perms = strtoul(optarg, &ptr, 8);
            if (ptr[0]) {
                error_print(0, "Invalid permissions \"%s\"", optarg);
                goto cleanup;
            }
            break;
        case 'v':
            if (!strcmp(optarg, "none")) {
                log_level = SR_LL_NONE;
            } else if (!strcmp(optarg, "error")) {
                log_level = SR_LL_ERR;
            } else if (!strcmp(optarg, "warning")) {
                log_level = SR_LL_WRN;
            } else if (!strcmp(optarg, "info")) {
                log_level = SR_LL_INF;
            } else if (!strcmp(optarg, "debug")) {
                log_level = SR_LL_DBG;
            } else if ((strlen(optarg) == 1) && (optarg[0] >= '0') && (optarg[0] <= '4')) {
                log_level = atoi(optarg);
            } else {
                error_print(0, "Invalid verbosity \"%s\"", optarg);
                goto cleanup;
            }
            break;
        default:
            error_print(0, "Invalid option or missing argument: -%c", optopt);
            goto cleanup;
        }
    }

    /* check for additional argument */
    if (optind < argc) {
        error_print(0, "Redundant parameters (%s)", argv[optind]);
        goto cleanup;
    }

    /* set logging */
    sr_log_stderr(log_level);

    if (operation != 'C') {
        /* create connection */
        if ((r = sr_connect(0, &conn)) != SR_ERR_OK) {
            error_print(r, "Failed to connect");
            goto cleanup;
        }
    }

    /* perform the operation */
    switch (operation) {
    case 'l':
        /* list */
        if ((r = srctl_list(conn)) != SR_ERR_OK) {
            error_print(r, "Failed to list modules");
            goto cleanup;
        }
        rc = EXIT_SUCCESS;
        break;
    case 'i':
        /* install */
        if ((r = sr_install_module(conn, file_path, search_dir, (const char **)features, feat_count)) != SR_ERR_OK) {
            /* succeed if the module is already installed */
            if (r != SR_ERR_EXISTS) {
                error_print(r, "Failed to install module \"%s\"", file_path);
                goto cleanup;
            }
        }
        rc = EXIT_SUCCESS;
        break;
    case 'u':
        /* uninstall */
        ptr = (char *)module_name;
        for (module_name = strtok(ptr, ","); module_name; module_name = strtok(NULL, ",")) {
            if ((r = sr_remove_module(conn, module_name)) != SR_ERR_OK) {
                error_print(r, "Failed to uninstall module \"%s\"", module_name);
                goto cleanup;
            }
        }
        rc = EXIT_SUCCESS;
        break;
    case 'c':
        /* change */

        /* change owner, group, and/or permissions */
        if (owner || group || ((int)perms != -1)) {
            if ((r = sr_set_module_access(conn, module_name, owner, group, perms)) != SR_ERR_OK) {
                error_print(r, "Failed to change module \"%s\" access", module_name);
                goto cleanup;
            }
        }

        /* change enabled features */
        for (i = 0; i < feat_count; ++i) {
            if ((r = sr_enable_module_feature(conn, module_name, features[i])) != SR_ERR_OK) {
                error_print(r, "Failed to enable feature \"%s\"", features[i]);
                goto cleanup;
            }
        }

        /* change disabled features */
        for (i = 0; i < dis_feat_count; ++i) {
            if ((r = sr_disable_module_feature(conn, module_name, dis_features[i])) != SR_ERR_OK) {
                error_print(r, "Failed to disable feature \"%s\"", dis_features[i]);
                goto cleanup;
            }
        }

        /* enable replay */
        if (replay != -1) {
            if ((r = sr_set_module_replay_support(conn, module_name, replay))) {
                error_print(r, "Failed to change replay support");
                goto cleanup;
            }
        }
        rc = EXIT_SUCCESS;
        break;
    case 'U':
        /* update */
        if ((r = sr_update_module(conn, file_path, search_dir)) != SR_ERR_OK) {
            error_print(r, "Failed to update module \"%s\"", file_path);
            goto cleanup;
        }
        rc = EXIT_SUCCESS;
        break;
    case 'C':
        /* connection-count */
        if ((r = sr_connection_count(&conn_count)) != SR_ERR_OK) {
            error_print(r, "Failed to get connection count");
            goto cleanup;
        }
        fprintf(stdout, "%u\n", conn_count);
        rc = EXIT_SUCCESS;
        break;
    case 'R':
        /* recover */
        if ((r = sr_connection_recover(conn)) != SR_ERR_OK) {
            error_print(r, "Failed to recover stale connections");
            goto cleanup;
        }
        rc = EXIT_SUCCESS;
        break;
    case 0:
        error_print(0, "No operation specified");
        break;
    default:
        error_print(0, "Internal");
        break;
    }

cleanup:
    sr_disconnect(conn);
    free(features);
    free(dis_features);
    return rc;
}
