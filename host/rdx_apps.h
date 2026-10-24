/* SPDX-License-Identifier: MIT
 * rdx-hostd helpers: the desktop user, installed programs (.desktop launchers) with
 * ready-to-add app presets, and icon lookup in the icon themes. */
#pragma once
#include <stddef.h>
#include <sys/types.h>
#include "../third_party/cjson/cJSON.h"

struct rdx_desktop_user {
    char name[64];
    uid_t uid;
    gid_t gid;
    char home[256];
};

/* the desktop user, 0 = found: `override` if set (config "desktop_user"), else the user of the
 * active graphical session on seat0 (loginctl), else - no graphical session at all, as in a seat
 * container (docs/MULTISEAT.md) - the one lingering user (rdx_linger_user). A real desktop
 * session always wins over the lingering user. */
int rdx_desktop_user(const char *override, struct rdx_desktop_user *u);

/* the only regular user (uid 1000..65533) with lingering enabled, from the files in `dir`
 * (systemd's /var/lib/systemd/linger); none or several: -1 */
#define RDX_LINGER_DIR "/var/lib/systemd/linger"
int rdx_linger_user(const char *dir, struct rdx_desktop_user *u);

/* launchers of the system and of the user, as
 * [{"id","name","icon","categories","game":bool,"preset":{app object ready for "apps"}}],
 * presets first (Steam, Lutris, Heroic), then games, then the rest */
cJSON *rdx_detect_apps(const struct rdx_desktop_user *u);

/* the preset for a launcher id (e.g. "steam"), NULL if not installed */
cJSON *rdx_preset_for(const struct rdx_desktop_user *u, const char *desktop_id);

/* icon file for an icon name or absolute path ("" if none); *mime gets the content type */
int rdx_find_icon(const struct rdx_desktop_user *u, const char *icon, char *path, size_t n, const char **mime);

/* the built-in screen icon (SVG) for "whole screen" apps */
extern const char rdx_screen_svg[];

/* shell snippet run as the app for Steam: closes a Steam that already runs on the desktop
 * (a second "steam" only forwards to it) and starts Steam's big-picture UI */
extern const char rdx_steam_launch_sh[];

/* the program an app's "cmd" runs, to find it among the processes: the base name of cmd[0];
 * "sh -c SCRIPT": the word after the script's last "exec " (rdx_steam_launch_sh: steam);
 * "env [K=V ...] prog": prog; "flatpak run [opts] APP-ID": the app id. "" if none */
void rdx_app_program(const cJSON *cmd, char *out, size_t n);

/* a process of `uid` running `prog` (rdx_app_program) outside rdx-hostd's service cgroup, i.e. an
 * instance on the desktop, not one of our streams: its pid (and in `what` "pid N: cmdline"),
 * 0 = none. `proc` is /proc (tests: a fake tree). Only reads. */
int rdx_desktop_instance(const char *proc, uid_t uid, const char *prog, char *what, size_t n);
