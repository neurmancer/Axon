#define _POSIX_C_SOURCE 200809L
#include "terminal.h"
#include "theme.h"
#include "output.h"
#include <limits.h>
#include "../cli/mutation.h"
#include "../cli/maintenance.h"
#include "alpm_extra.h"

#include <curses.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char *name;
    char *target;
    char *version;
    char *description;
    int selected;
} row_t;

static int themed;

static int theme_pair(int index)
{
    return(COLOR_PAIR(themed ? index : 0));
}

static void theme(void)
{
    themed = 0;
    const char *disabled = getenv("NO_COLOR");
    if((disabled && *disabled) || !has_colors() || start_color() == ERR) {
        return;
    }
    short background = use_default_colors() == OK ? -1 : COLOR_BLACK;
    short colors[] = {COLOR_BLUE, COLOR_MAGENTA, COLOR_GREEN};
    const short rgb[][3] = {{90, 150, 255}, {193, 112, 255}, {0, 235, 116}};
    if(COLORS >= 256) {
        colors[0] = 69;
        colors[1] = 171;
        colors[2] = 48;
    }
    if(COLORS > 18 && can_change_color()) {
        for(int index = 0; index < 3; index++) {
            short slot = (short)(16 + index);
            if(init_color(slot, rgb[index][0] * 1000 / 255, rgb[index][1] * 1000 / 255,
                          rgb[index][2] * 1000 / 255) == OK) {
                colors[index] = slot;
            }
        }
    }
    for(short index = 0; index < 3; index++) {
        init_pair(index + 1, colors[index], background);
    }
    themed = 1;
    bkgd(COLOR_PAIR(1));
}

static void clear_rows(row_t *rows, size_t count)
{
    for(size_t index = 0; index < count; index++) {
        free(rows[index].name);
        free(rows[index].target);
        free(rows[index].version);
        free(rows[index].description);
    }
    free(rows);
}

static int add(row_t **rows, size_t *count, alpm_pkg_t *package, int remote)
{
    row_t *grown = realloc(*rows, (*count + 1) * sizeof(**rows));
    if(grown == NULL) {
        return(0);
    }
    *rows = grown;
    row_t *row = &grown[(*count)++];
    memset(row, 0, sizeof(*row));
    const char *name = alpm_pkg_get_name(package);
    const char *repository = remote ? alpm_db_get_name(alpm_pkg_get_db(package)) : "";
    row->name = strdup(name);
    row->version = axon_display_text(alpm_pkg_get_version(package), INT_MAX);
    const char *description = alpm_pkg_get_desc(package);
    row->description = axon_display_text(description != NULL ? description : "", INT_MAX);
    row->target = malloc(strlen(repository) + strlen(name) + 2);
    if(row->target != NULL) {
        sprintf(row->target, "%s%s%s", repository, remote ? "/" : "", name);
    }
    return(row->name != NULL && row->target != NULL && row->version != NULL && row->description != NULL);
}

static int load(const axon_config_t *config, int remote, row_t **rows, size_t *count)
{
    axon_alpm_t *core = NULL;
    axon_config_error_t error;
    if(axon_config_open(config, &core, &error) != AXON_CONFIG_OK) {
        axon_fprintf(stderr, "axon: %s\n", error.message);
        return(0);
    }
    int success = 1;
    if(remote) {
        const alpm_list_t *repositories = NULL;
        success = axon_alpm_repositories(core, &repositories) == ALPM_ERR_OK;
        for(const alpm_list_t *item = repositories; item && success; item = item->next) {
            alpm_db_t *database = item->data;
            if(alpm_db_get_valid(database) < 0) {
                success = 0;
                break;
            }
            alpm_list_t *packages = alpm_db_get_pkgcache(database);
            if(packages == NULL && alpm_errno(alpm_db_get_handle(database)) != ALPM_ERR_OK) {
                success = 0;
            }
            for(alpm_list_t *pkg = packages; pkg && success; pkg = pkg->next) {
                success = add(rows, count, pkg->data, 1);
            }
        }
    }

    else {
        const alpm_list_t *packages = NULL;
        success = axon_alpm_local_packages(core, &packages) == ALPM_ERR_OK;
        for(const alpm_list_t *item = packages; item && success; item = item->next) {
            success = add(rows, count, item->data, 0);
        }
    }
    if(axon_alpm_release(&core) != 0) {
        success = 0;
    }
    if(!success) {
        fputs("axon: cannot read package list; inspect database status with --check-db\n", stderr);
    }
    return(success);
}

int axon_terminal(const axon_config_t *config)
{
    if(!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fputs("axon: --ui requires an interactive terminal\n", stderr);
        return(1);
    }
    setlocale(LC_ALL, "");
    if(initscr() == NULL) {
        return(1);
    }
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    theme();
    row_t *rows = NULL;
    size_t count = 0, current = 0;
    int remote = 0, reload = 1, status = 0;
    char filter[128] = "";
    for(;;) {
        if(reload) {
            clear_rows(rows, count);
            rows = NULL;
            count = current = 0;
            endwin();
            int loaded = load(config, remote, &rows, &count);
            refresh();
            if(!loaded) {
                status = 1;
                break;
            }
            reload = 0;
        }
        size_t *visible = count ? malloc(count * sizeof(*visible)) : NULL;
        if(count && visible == NULL) {
            status = 1;
            break;
        }
        size_t shown = 0;
        for(size_t index = 0; index < count; index++) {
            if(strstr(rows[index].name, filter) != NULL || strstr(rows[index].description, filter) != NULL) {
                visible[shown++] = index;
            }
        }
        if(current >= shown) {
            current = shown ? shown - 1 : 0;
        }
        erase();
        int height, width;
        getmaxyx(stdscr, height, width);
        if(height < 8 || width < 40) {
            mvaddstr(0, 0, "Resize terminal (at least 40x8), q quits");
        }

        else {
            attron(theme_pair(2) | A_BOLD);
            char heading[256];
            snprintf(heading, sizeof(heading), "Axon | %s | %zu packages | filter: %s", remote ? "Repositories" : "Installed", shown, filter);
            char *header = axon_display_text(heading, width - 1);
            if(header != NULL) {
                mvaddstr(0, 0, header);
                free(header);
            }
            attrset(theme_pair(1));
            mvaddnstr(1, 0, "Tab switch  / filter  Space select  i install  r remove  u upgrade  q quit", width - 1);
            size_t capacity = (size_t)(height - 5);
            size_t first = current >= capacity ? current - capacity + 1 : 0;
            for(size_t index = first; index < shown && index < first + capacity; index++) {
                row_t *row = &rows[visible[index]];
                attrset(theme_pair(row->selected ? 3 : 1));
                if(index == current) {
                    attrset(theme_pair(2) | A_REVERSE | A_BOLD);
                }
                char *label = axon_display_text(row->target, width / 2);
                char *version = axon_display_text(row->version, width - width / 2 - 4);
                if(label != NULL && version != NULL) {
                    mvprintw(2 + (int)(index - first), 0, "%c %s", row->selected ? '*' : ' ', label);
                    mvaddstr(2 + (int)(index - first), width / 2 + 3, version);
                }
                else {
                    status = 1;
                }
                free(label);
                free(version);
                attrset(theme_pair(1));
            }
            if(shown) {
                char *description = axon_display_text(rows[visible[current]].description, width - 1);
                if(description != NULL) {
                    mvaddstr(height - 2, 0, description);
                    free(description);
                }
                else {
                    status = 1;
                }
            }
            attrset(theme_pair(2));
            mvaddnstr(height - 1, 0, "Enter details | f archive | o orphans | c cache | d check DB | e explicit | b dependency", width - 1);
            attrset(theme_pair(1));
        }
        int key = getch();
        if(key == 'q' || key == ERR) {
            free(visible);
            break;
        }
        if(key == KEY_DOWN || key == 'j') {
            if(current + 1 < shown) {
                current++;
            }
        }

        else if(key == KEY_UP || key == 'k') {
            if(current) {
                current--;
            }
        }

        else if(key == '\t') {
            remote = !remote;
            reload = 1;
        }

        else if(key == '/') {
            move(height - 1, 0);
            clrtoeol();
            attrset(theme_pair(2) | A_BOLD);
            addstr("Filter: ");
            attrset(theme_pair(1));
            echo();
            curs_set(1);
            getnstr(filter, sizeof(filter) - 1);
            noecho();
            curs_set(0);
            current = 0;
        }

        else if(key == ' ' && shown) {
            rows[visible[current]].selected = !rows[visible[current]].selected;
        }

        else if(key == 'u' || key == 'f' || key == 'o' || key == 'c' || key == 'd'
                || key == '\n' || ((key == 'e' || key == 'b') && !remote) || (key == 'i' && remote) || (key == 'r' && !remote)) {
            char **targets = calloc(count + 1, sizeof(*targets));
            if(targets == NULL) {
                free(visible);
                status = 1;
                break;
            }
            int selected = 0;
            for(size_t index = 0; index < shown; index++) {
                if(rows[visible[index]].selected) {
                    targets[selected++] = rows[visible[index]].target;
                }
            }
            if(!selected && shown) {
                targets[selected++] = rows[visible[current]].target;
            }
            endwin();
            clearerr(stdin);
            if(key == 'o' || key == 'c' || key == 'd') {
                axon_cli_maintenance(config, key == 'o' ? AXON_ORPHANS : key == 'c' ? AXON_CLEAN_CACHE : AXON_CHECK_DATABASE, 0, NULL);
            }

            else if((key == 'e' || key == 'b') && selected) {
                axon_cli_maintenance(config, key == 'e' ? AXON_REASON_EXPLICIT : AXON_REASON_DEPENDENCY, selected, targets);
            }

            else if(key == '\n' && shown) {
                row_t *row = &rows[visible[current]];
                fputs(axon_color(stdout, AXON_BLUE), stdout);
                axon_fprintf(stdout, "%s %s\n%s\n", row->target, row->version, row->description);
                fputs(axon_color(stdout, AXON_RESET), stdout);
            }

            else if(key == 'f') {
                char path[4096];
                fputs("Archive path: ", stdout);
                fflush(stdout);
                if(fgets(path, sizeof(path), stdin) != NULL && strchr(path, '\n') != NULL) {
                    path[strcspn(path, "\n")] = '\0';
                    char *archive = path;
                    if(axon_cli_mutate(config, AXON_OPERATION_FILE, 1, &archive, 0, 0, 0, 0) != 0) {
                        status = 1;
                    }
                }
            }

            else if(key == 'u') {
                if(axon_cli_mutate(config, AXON_OPERATION_UPGRADE, 0, NULL, 1, 0, 0, 0) != 0) {
                    status = 1;
                }
            }

            else if(selected && (key == 'i' || key == 'r')) {
                if(axon_cli_mutate(config, remote ? AXON_OPERATION_INSTALL : AXON_OPERATION_REMOVE,
                                  selected, targets, 0, 0, 0, 0) != 0) {
                    status = 1;
                }
            }
            free(targets);
            fputs("Press Enter to return to the package list.", stdout);
            fflush(stdout);
            int input;
            do {
                input = getchar();
            } while(input != '\n' && input != EOF);
            refresh();
            reload = 1;
        }
        free(visible);
    }
    clear_rows(rows, count);
    endwin();
    return(axon_output_failed() || ferror(stdout) || ferror(stderr) ? 1 : status);
}
