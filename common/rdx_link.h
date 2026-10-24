/* SPDX-License-Identifier: MIT
 *
 * Link health of the RDMA link a daemon streams over (rdx-hostd, rdx-clientd): the port's state
 * and the network module's (transceiver's) temperature, with a warning before the module's own
 * threshold takes the link down, and the reason when the link went down.
 *
 * Read by the daemons (root), never by the tray: a background thread samples every
 * RDX_LINK_SAMPLE_MS and follows the kernel log.
 *   - port: the RDMA port whose GID table holds link_ip (sysfs /sys/class/infiniband/<dev>/ports/<p>/
 *     gids, state, phys_state, rate). The RDMA devices are not network-namespaced, so this works in a
 *     seat container behind a macvlan as well.
 *   - module temperature: the NIC driver's hwmon (mlx5: tempN_label "Module<n>", tempN_input, crit =
 *     the module's high warning threshold, highest) under the port's PCI function; readable without root.
 *   - module thresholds, vendor, part: the module EEPROM by SIOCETHTOOL (ETHTOOL_GMODULEINFO /
 *     GMODULEEEPROM, the interface of `ethtool -m`; SFF-8636 page 03h and SFF-8472 A2h), when the
 *     port's netdev is in this network namespace (not in a seat container). Without hwmon the
 *     temperature comes from the EEPROM too.
 *   - why the link went down: the NIC's kernel log line (mlx5: "Port module event[error]: module 1,
 *     Cable error, High Temperature") from /dev/kmsg, else the module temperature just before, else
 *     "no signal from the other side".
 * Without a sensor (a DAC cable, a ConnectX-4) only the port state is reported.
 *
 * The card itself (the NIC chip, not the module):
 *   - chip temperature: the driver's hwmon "sensor0" (mlx5 on a ConnectX-5 and newer: tempN_input,
 *     tempN_crit = the limit the firmware warns at, tempN_highest). The ConnectX-4 has no hwmon at
 *     all; its only sign of heat is the kernel log ("temp_warn: High temperature on sensors with bit
 *     set ...", "Health issue observed, High temperature"), which counts as chip alarm for
 *     RDX_LINK_CHIP_EVENT_S. A warning comes RDX_LINK_CHIP_MARGIN_C below the limit, the alarm at it.
 *   - failure: "the card is gone" is its own level (RDX_LINK_FAILED, JSON "failed"), not a link down
 *     that heals by reconnecting: the PCI function is off the bus or does not answer config reads
 *     (all ones, EIO), the kernel log says "Fatal error ... detected" / "PCI slot is unavailable" /
 *     "health recovery flow aborted", or the RDMA device has been missing for RDX_LINK_NODEV_S while
 *     the card is on the bus. The message then says to power-cycle the machine, "(likely
 *     overheated)" when the card or its module was hot or reported heat in the last
 *     RDX_LINK_HEAT_EVIDENCE_S. Seen on a ConnectX-4: temp_warn, an hour later the health error
 *     "High temperature", minutes later "Fatal error 1 detected / PCI slot is unavailable".
 */
#pragma once
#include <stddef.h>

struct cJSON;

#define RDX_LINK_SAMPLE_MS 2000
#define RDX_LINK_RECENT_S  600      /* a link-down stays in the warning line this long after the link is back */
#define RDX_LINK_CHIP_MARGIN_C 10.0 /* the chip warning starts this far below the sensor's limit */
#define RDX_LINK_CHIP_EVENT_S  600  /* a kernel log heat report keeps the chip at alarm this long */
#define RDX_LINK_HEAT_EVIDENCE_S 7200   /* heat this recent makes a failure "likely overheated" */
#define RDX_LINK_NODEV_S   30       /* the RDMA device missing this long (card on the bus) is a failure */

enum rdx_link_level { RDX_LINK_UNKNOWN = 0, RDX_LINK_OK, RDX_LINK_WARN, RDX_LINK_ALARM, RDX_LINK_DOWN,
                      RDX_LINK_FAILED /* the card is gone: power-cycle, no reconnect helps */ };

/* what made a warn/alarm level: the transceiver or the card's chip */
enum rdx_link_hot { RDX_HOT_NONE = 0, RDX_HOT_MODULE, RDX_HOT_CHIP };

struct rdx_link_module {
    int present;                    /* a temperature is known */
    char label[32];                 /* hwmon label, e.g. "Module1" ("" = from the EEPROM) */
    double temp_c, warn_c, alarm_c, highest_c;   /* NAN = unknown */
    char vendor[20], part[20];
};

struct rdx_link_chip {
    int present;                    /* the hwmon temperature is known */
    int event;                      /* the kernel log reported heat / health trouble within RDX_LINK_CHIP_EVENT_S */
    char label[32];
    double temp_c, limit_c, highest_c;           /* NAN = unknown; limit = hwmon tempN_crit */
    char event_text[120];           /* the kernel's words, "" = none */
};

struct rdx_link_info {
    int known;                      /* an RDMA port carries link_ip */
    char link_ip[64], rdma_dev[64], pci[32], netdev[32];
    int port;
    int up;
    char state[24], phys_state[24], rate[48];
    struct rdx_link_module mod;
    struct rdx_link_chip chip;
    int hot;                        /* enum rdx_link_hot: the source of a warn/alarm level */
    int nic_failed;                 /* level == RDX_LINK_FAILED */
    int nic_heat;                   /* ... and it was likely heat */
    int nic_power_cycle;            /* ... and only a power-cycle helps (not: the RDMA device only vanished) */
    char nic_why[64];               /* short cause: "PCI device not answering" */
    char nic_detail[200];           /* what was seen: the PCI path, the kernel's words */
    int level;                    /* enum rdx_link_level */
    char message[200];              /* one line, "" when ok */
    long downs;                     /* link downs seen since the start */
    int have_down;                  /* last_down below is valid */
    double down_at, back_at;        /* unix time; back_at 0 = still down */
    double down_temp_c;             /* module temperature just before (NAN = unknown) */
    char down_reason[96], down_detail[200];
    double since;                   /* unix time of the last level change */
};

/* start the sampling thread for link_ip ("" = none yet); sysroot NULL = "/" (tests: a fake tree) */
int  rdx_link_start(const char *link_ip, const char *sysroot);
/* the link IP changed (configuration) */
void rdx_link_target(const char *link_ip);
/* readable after a change of level, module presence or the down counter; rdx_link_take() drains it */
int  rdx_link_event_fd(void);
int  rdx_link_take(void);
/* a snapshot */
void rdx_link_get(struct rdx_link_info *out);

const char *rdx_link_level_name(int level);
struct cJSON *rdx_link_json(const struct rdx_link_info *li);
/* the other computer's link as its daemon reported it (hello "link", event "link.health"); 0 = ok */
int  rdx_link_from_json(const struct cJSON *j, struct rdx_link_info *out);
/* the one line a client shows in its in-stream menu and on its placeholder, from its own link and the
 * other computer's (peer NULL = unknown); "" = nothing to say. now = unix time */
void rdx_link_line(const struct rdx_link_info *own, const struct rdx_link_info *peer, double now, char *out, size_t n);

/* for tests and the thread: one sample now, one kernel log record, the EEPROM parser */
void rdx_link_sample_once(void);
void rdx_link_kmsg_record(const char *rec);
/* type = ethtool_modinfo.type (ETH_MODULE_SFF_8636 ...); fills the thresholds/vendor/part it finds and,
 * when temp_too, the temperature; returns 0 when the layout was known */
int  rdx_link_parse_eeprom(unsigned type, const unsigned char *ee, size_t len, struct rdx_link_module *m, int temp_too);
