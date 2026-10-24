/* SPDX-License-Identifier: MIT
 * rdx-hostd helpers: desktop user, installed programs with app presets, icons. */
#define _GNU_SOURCE
#include "rdx_apps.h"

#include <ctype.h>
#include <dirent.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char rdx_screen_svg[] =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'>"
    "<rect x='6' y='10' width='52' height='34' rx='4' fill='none' stroke='#2563eb' stroke-width='4'/>"
    "<path d='M22 54h20M32 44v10' stroke='#2563eb' stroke-width='4' stroke-linecap='round'/></svg>";

const char rdx_steam_launch_sh[] =
    "if pgrep -u \"$(id -u)\" -x steam >/dev/null; then steam -shutdown >/dev/null 2>&1; "
    "for i in $(seq 1 60); do pgrep -u \"$(id -u)\" -x steam >/dev/null || break; sleep 0.5; done; fi; "
    "exec steam \"$@\"";

/* ---------------- desktop user ---------------- */

static int user_by_name(const char *name, struct rdx_desktop_user *u)
{
    struct passwd *pw = getpwnam(name);
    if (!pw) return -1;
    snprintf(u->name, sizeof(u->name), "%s", pw->pw_name);
    u->uid = pw->pw_uid;
    u->gid = pw->pw_gid;
    snprintf(u->home, sizeof(u->home), "%s", pw->pw_dir);
    return 0;
}

static int seat0_session_user(struct rdx_desktop_user *u);

int rdx_linger_user(const char *dir, struct rdx_desktop_user *u)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    char one[64] = "";
    int n = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        struct passwd *pw = getpwnam(e->d_name);
        if (!pw || pw->pw_uid < 1000 || pw->pw_uid >= 65534) continue;   /* system users, nobody */
        if (!n++) snprintf(one, sizeof(one), "%s", pw->pw_name);
    }
    closedir(d);
    return n == 1 ? user_by_name(one, u) : -1;
}

int rdx_desktop_user(const char *override, struct rdx_desktop_user *u)
{
    memset(u, 0, sizeof(*u));
    if (override && *override) return user_by_name(override, u);
    if (!seat0_session_user(u)) return 0;
    /* no graphical session (a seat container: a lingering user manager, no display manager) */
    return rdx_linger_user(RDX_LINGER_DIR, u);
}

/* the user of the active graphical session on seat0; 0 = found */
static int seat0_session_user(struct rdx_desktop_user *u)
{
    FILE *p = popen("for s in $(loginctl list-sessions --no-legend 2>/dev/null | awk '{print $1}'); do "
                    "loginctl show-session \"$s\" -p Name -p Type -p Class -p Seat -p Active --value 2>/dev/null "
                    "| tr '\\n' ' '; echo; done", "r");
    if (!p) return -1;
    char line[512], best[64] = "";
    int best_active = -1;
    while (fgets(line, sizeof(line), p)) {
        /* order of --value output follows the -p order of systemd: Name Type Class Seat Active
         * is not guaranteed, so classify the words instead */
        char name[64] = "", *w, *save = NULL;
        int graphical = 0, user = 0, seat0 = 0, active = 0, idx = 0;
        for (w = strtok_r(line, " \n", &save); w; w = strtok_r(NULL, " \n", &save), idx++) {
            if (!strcmp(w, "x11") || !strcmp(w, "wayland")) graphical = 1;
            else if (!strcmp(w, "user")) user = 1;
            else if (!strcmp(w, "seat0")) seat0 = 1;
            else if (!strcmp(w, "yes")) active = 1;
            else if (!strcmp(w, "no") || !strcmp(w, "tty") || !strcmp(w, "unspecified") || !strcmp(w, "manager") ||
                     !strcmp(w, "greeter") || !strcmp(w, "lock-screen") || !strcmp(w, "background")) ;
            else if (!*name && getpwnam(w)) snprintf(name, sizeof(name), "%s", w);
        }
        if (graphical && user && seat0 && *name && active > best_active) {
            snprintf(best, sizeof(best), "%s", name);
            best_active = active;
        }
    }
    pclose(p);
    return *best ? user_by_name(best, u) : -1;
}

/* ---------------- .desktop launchers ---------------- */

struct entry {
    char id[96], name[128], exec[1024], icon[256], cats[256];
    int nodisplay, app;
};

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

static int parse_desktop(const char *path, struct entry *e)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[2048];
    int in_main = 0;
    memset(e, 0, sizeof(*e));
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (line[0] == '[') { in_main = !strcmp(line, "[Desktop Entry]"); continue; }
        if (!in_main) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "Name")) snprintf(e->name, sizeof(e->name), "%s", v);
        else if (!strcmp(k, "Exec")) snprintf(e->exec, sizeof(e->exec), "%s", v);
        else if (!strcmp(k, "Icon")) snprintf(e->icon, sizeof(e->icon), "%s", v);
        else if (!strcmp(k, "Categories")) snprintf(e->cats, sizeof(e->cats), "%s", v);
        else if (!strcmp(k, "Type")) e->app = !strcmp(v, "Application");
        else if ((!strcmp(k, "NoDisplay") || !strcmp(k, "Hidden")) && !strcmp(v, "true")) e->nodisplay = 1;
    }
    fclose(f);
    return e->app && *e->name && *e->exec && !e->nodisplay ? 0 : -1;
}

/* Exec line -> argv (double quotes, backslash escapes), field codes (%u %F ...) dropped */
static cJSON *exec_argv(const char *exec)
{
    cJSON *a = cJSON_CreateArray();
    char tok[1024];
    size_t n = 0;
    int q = 0, have = 0;
    for (const char *p = exec;; p++) {
        char c = *p;
        if (!c || (!q && isspace((unsigned char)c))) {
            if (have) {
                tok[n] = 0;
                if (!(tok[0] == '%' && tok[1] && !tok[2])) cJSON_AddItemToArray(a, cJSON_CreateString(tok));
            }
            n = 0; have = 0;
            if (!c) break;
            continue;
        }
        if (c == '"') { q = !q; have = 1; continue; }
        if (c == '\\' && p[1]) c = *++p;
        if (n < sizeof(tok) - 1) tok[n++] = c;
        have = 1;
    }
    return a;
}

static int has_cat(const struct entry *e, const char *cat)
{
    char buf[300];
    snprintf(buf, sizeof(buf), ";%s;", e->cats);
    char want[64];
    snprintf(want, sizeof(want), ";%s;", cat);
    return strstr(buf, want) != NULL;
}

/* ranking: 0 = known launcher preset, 1 = game, 2 = other */
static int known_rank(const char *id)
{
    return !strcmp(id, "steam") || !strcmp(id, "net.lutris.Lutris") || !strcmp(id, "lutris") ||
           !strcmp(id, "com.heroicgameslauncher.hgl") || !strcmp(id, "heroic") ||
           !strcmp(id, "com.valvesoftware.Steam") ? 0 : -1;
}

static cJSON *preset_from(const struct entry *e)
{
    cJSON *p = cJSON_CreateObject();
    char id[64];
    int j = 0;
    for (const char *s = e->id; *s && j < 40; s++)
        id[j++] = isalnum((unsigned char)*s) ? (char)tolower((unsigned char)*s) : '-';
    id[j] = 0;
    cJSON_AddStringToObject(p, "id", id);
    cJSON_AddStringToObject(p, "name", e->name);
    cJSON_AddStringToObject(p, "kind", "gamescope");
    cJSON_AddStringToObject(p, "icon", e->icon);
    cJSON_AddStringToObject(p, "desktop_id", e->id);
    cJSON *args = cJSON_AddArrayToObject(p, "gamescope_args");
    if (!strcmp(e->id, "steam") || !strcmp(e->id, "com.valvesoftware.Steam")) {
        /* Steam Deck mode: gamescope cooperates with Steam (-e), games launched from Steam
         * run in the same gamescope, full screen */
        cJSON_AddItemToArray(args, cJSON_CreateString("-e"));
        cJSON *cmd = cJSON_AddArrayToObject(p, "cmd");
        cJSON_AddItemToArray(cmd, cJSON_CreateString("sh"));
        cJSON_AddItemToArray(cmd, cJSON_CreateString("-c"));
        cJSON_AddItemToArray(cmd, cJSON_CreateString(rdx_steam_launch_sh));
        cJSON_AddItemToArray(cmd, cJSON_CreateString("steam"));
        cJSON_AddItemToArray(cmd, cJSON_CreateString("-gamepadui"));
        cJSON_AddStringToObject(p, "note", "Steam on this computer is closed while it runs in the stream.");
    } else {
        cJSON_AddItemToObject(p, "cmd", exec_argv(e->exec));
    }
    cJSON_AddItemToObject(p, "env", cJSON_CreateObject());
    return p;
}

static void scan_dir(const char *dir, struct entry **list, int *n, int *cap)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l < 9 || strcmp(de->d_name + l - 8, ".desktop")) continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        struct entry e;
        if (parse_desktop(path, &e)) continue;
        snprintf(e.id, sizeof(e.id), "%.*s", (int)(l - 8), de->d_name);
        int dup = 0;
        for (int i = 0; i < *n; i++)
            if (!strcmp((*list)[i].id, e.id)) { (*list)[i] = e; dup = 1; break; }   /* user overrides system */
        if (dup) continue;
        if (*n == *cap) { *cap = *cap ? *cap * 2 : 64; *list = realloc(*list, (size_t)*cap * sizeof(**list)); }
        (*list)[(*n)++] = e;
    }
    closedir(d);
}

static struct entry *scan_all(const struct rdx_desktop_user *u, int *n)
{
    struct entry *list = NULL;
    int cap = 0;
    *n = 0;
    const char *sys[] = {"/usr/share/applications", "/usr/local/share/applications",
                         "/var/lib/flatpak/exports/share/applications", "/var/lib/snapd/desktop/applications"};
    for (size_t i = 0; i < sizeof(sys) / sizeof(sys[0]); i++) scan_dir(sys[i], &list, n, &cap);
    if (u && *u->home) {
        char p[512];
        snprintf(p, sizeof(p), "%s/.local/share/flatpak/exports/share/applications", u->home);
        scan_dir(p, &list, n, &cap);
        snprintf(p, sizeof(p), "%s/.local/share/applications", u->home);
        scan_dir(p, &list, n, &cap);
    }
    return list;
}

static int rank_of(const struct entry *e)
{
    if (known_rank(e->id) == 0) return 0;
    return has_cat(e, "Game") ? 1 : 2;
}

static int by_rank(const void *a, const void *b)
{
    const struct entry *x = a, *y = b;
    int rx = rank_of(x), ry = rank_of(y);
    if (rx != ry) return rx - ry;
    return strcasecmp(x->name, y->name);
}

cJSON *rdx_detect_apps(const struct rdx_desktop_user *u)
{
    int n;
    struct entry *list = scan_all(u, &n);
    qsort(list, (size_t)n, sizeof(*list), by_rank);
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        /* our own launchers, settings panels, terminals and the like are not something to stream */
        if (!strncmp(list[i].id, "rdx-", 4)) continue;
        if (has_cat(&list[i], "Settings") || has_cat(&list[i], "System") || has_cat(&list[i], "TerminalEmulator"))
            if (rank_of(&list[i]) == 2) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", list[i].id);
        cJSON_AddStringToObject(o, "name", list[i].name);
        cJSON_AddStringToObject(o, "icon", list[i].icon);
        cJSON_AddBoolToObject(o, "game", rank_of(&list[i]) <= 1);
        cJSON_AddBoolToObject(o, "recommended", rank_of(&list[i]) == 0);
        cJSON_AddItemToObject(o, "preset", preset_from(&list[i]));
        cJSON_AddItemToArray(a, o);
    }
    free(list);
    return a;
}

cJSON *rdx_preset_for(const struct rdx_desktop_user *u, const char *desktop_id)
{
    int n;
    struct entry *list = scan_all(u, &n);
    cJSON *p = NULL;
    for (int i = 0; i < n && !p; i++)
        if (!strcmp(list[i].id, desktop_id)) p = preset_from(&list[i]);
    free(list);
    return p;
}

/* ---------------- icons ---------------- */

static int icon_ok(const char *path, const char **mime)
{
    size_t l = strlen(path);
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size > 4 * 1024 * 1024) return 0;
    if (l > 4 && !strcmp(path + l - 4, ".png")) *mime = "image/png";
    else if (l > 4 && !strcmp(path + l - 4, ".svg")) *mime = "image/svg+xml";
    else return 0;
    return 1;
}

int rdx_find_icon(const struct rdx_desktop_user *u, const char *icon, char *path, size_t n, const char **mime)
{
    path[0] = 0;
    if (!icon || !*icon) return -1;
    if (icon[0] == '/') {                               /* absolute Icon= in a launcher */
        if (strstr(icon, "/../")) return -1;
        snprintf(path, n, "%s", icon);
        return icon_ok(path, mime) ? 0 : -1;
    }
    if (strchr(icon, '/')) return -1;
    char bases[6][512];
    int nb = 0;
    if (u && *u->home) {
        snprintf(bases[nb++], 512, "%s/.local/share/icons", u->home);
        snprintf(bases[nb++], 512, "%s/.local/share/flatpak/exports/share/icons", u->home);
    }
    snprintf(bases[nb++], 512, "/usr/share/icons");
    snprintf(bases[nb++], 512, "/var/lib/flatpak/exports/share/icons");
    snprintf(bases[nb++], 512, "/usr/local/share/icons");
    const char *themes[] = {"hicolor", "Yaru", "Adwaita"};
    const char *sizes[] = {"256x256", "scalable", "128x128", "512x512", "96x96", "64x64", "48x48"};
    const char *ext[] = {"png", "svg"};
    for (int b = 0; b < nb; b++)
        for (size_t t = 0; t < 3; t++)
            for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
                for (int x = 0; x < 2; x++) {
                    snprintf(path, n, "%s/%s/%s/apps/%s.%s", bases[b], themes[t], sizes[s], icon, ext[x]);
                    if (icon_ok(path, mime)) return 0;
                }
    for (int x = 0; x < 2; x++) {
        snprintf(path, n, "/usr/share/pixmaps/%s.%s", icon, ext[x]);
        if (icon_ok(path, mime)) return 0;
    }
    path[0] = 0;
    return -1;
}

/* ---------------- the app's program among the running processes ---------------- */

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

void rdx_app_program(const cJSON *cmd, char *out, size_t n)
{
    out[0] = 0;
    int argc = cJSON_GetArraySize(cmd), i = 0;
    const cJSON *a0 = cJSON_GetArrayItem(cmd, 0);
    if (!cJSON_IsString(a0)) return;
    const char *b = base_name(a0->valuestring);
    if ((!strcmp(b, "sh") || !strcmp(b, "bash")) && argc >= 3 &&
        cJSON_IsString(cJSON_GetArrayItem(cmd, 1)) && !strcmp(cJSON_GetArrayItem(cmd, 1)->valuestring, "-c")) {
        /* a launch script: what its last "exec" runs (rdx_steam_launch_sh: "exec steam") */
        const char *sc = cJSON_GetArrayItem(cmd, 2)->valuestring, *e = NULL, *q;
        for (q = sc; (q = strstr(q, "exec ")); q += 5) e = q + 5;
        if (!e) return;
        while (*e == ' ') e++;
        size_t k = strcspn(e, " \t\n;\"'&|");
        char w[256];
        snprintf(w, sizeof(w), "%.*s", (int)(k < sizeof(w) - 1 ? k : sizeof(w) - 1), e);
        snprintf(out, n, "%s", base_name(w));
        return;
    }
    if (!strcmp(b, "env")) {               /* env [-u X] K=V ... prog */
        for (i = 1; i < argc; i++) {
            const char *s = cJSON_GetArrayItem(cmd, i)->valuestring;
            if (!s || s[0] == '-' || strchr(s, '=')) continue;
            snprintf(out, n, "%s", base_name(s));
            return;
        }
        return;
    }
    if (!strcmp(b, "flatpak")) {           /* flatpak run [--opts] APP-ID: the id is in the app's cmdline */
        for (i = 1; i < argc; i++) {
            const char *s = cJSON_GetArrayItem(cmd, i)->valuestring;
            if (s && s[0] != '-' && strcmp(s, "run")) { snprintf(out, n, "%s", s); return; }
        }
        return;
    }
    snprintf(out, n, "%s", b);
}

/* one /proc/<pid> file, NULs (cmdline) turned into spaces; length */
static size_t read_proc(const char *proc, const char *pid, const char *f, char *buf, size_t n)
{
    char p[600];
    snprintf(p, sizeof(p), "%s/%s/%s", proc, pid, f);
    FILE *fp = fopen(p, "r");
    if (!fp) { buf[0] = 0; return 0; }
    size_t k = fread(buf, 1, n - 1, fp);
    fclose(fp);
    buf[k] = 0;
    return k;
}

int rdx_desktop_instance(const char *proc, uid_t uid, const char *prog, char *what, size_t n)
{
    if (what && n) what[0] = 0;
    if (!prog || !*prog) return 0;
    DIR *d = opendir(proc);
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    char comm[64], cg[1024], cl[4096];
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        char p[512];
        struct stat st;
        snprintf(p, sizeof(p), "%s/%s", proc, e->d_name);
        if (stat(p, &st) || st.st_uid != uid) continue;
        /* ours (a stream's or the seat monitor's app) runs in rdx-hostd's service cgroup */
        read_proc(proc, e->d_name, "cgroup", cg, sizeof(cg));
        if (strstr(cg, "/rdx-hostd")) continue;
        read_proc(proc, e->d_name, "comm", comm, sizeof(comm));
        comm[strcspn(comm, "\n")] = 0;
        size_t k = read_proc(proc, e->d_name, "cmdline", cl, sizeof(cl));
        /* the program itself, or an interpreter running it (python3 /usr/games/lutris): argv[0]
         * or argv[1] by base name; comm is cut to 15 characters; a flatpak id anywhere in argv */
        const char *a0 = cl, *a1 = strlen(cl) + 1 < k ? cl + strlen(cl) + 1 : "";
        int hit = !strncmp(comm, prog, 15) && strlen(comm) == (strlen(prog) < 15 ? strlen(prog) : 15);
        hit = hit || !strcmp(base_name(a0), prog) || (*a1 && *a1 != '-' && !strcmp(base_name(a1), prog));
        if (!hit && strchr(prog, '.'))
            for (size_t i = 0; i < k && !hit; i += strlen(cl + i) + 1) hit = !strcmp(cl + i, prog);
        if (!hit) continue;
        found = atoi(e->d_name);
        if (what && n) {
            for (size_t i = 0; i + 1 < k; i++) if (!cl[i]) cl[i] = ' ';
            snprintf(what, n, "pid %d: %.200s", found, cl);
        }
    }
    closedir(d);
    return found;
}
