/* SPDX-License-Identifier: MIT
 * common/rdx_link: port state, module temperature and link-down reasons on a fake sysfs tree
 * (no NIC needed), the EEPROM parser, the JSON round trip and the client's warning line.
 */
#define _GNU_SOURCE
#include "../common/rdx_link.h"
#include "../third_party/cjson/cJSON.h"
#include <linux/ethtool.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[256];
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put(const char *rel, const char *val)
{
    char p[512], cmd[700];
    snprintf(p, sizeof(p), "%s%s", root, rel);
    snprintf(cmd, sizeof(cmd), "mkdir -p \"$(dirname '%s')\"", p);
    if (system(cmd)) exit(2);
    FILE *f = fopen(p, "w");
    if (!f) { perror(p); exit(2); }
    fprintf(f, "%s\n", val);
    fclose(f);
}

static void put_bin(const char *rel, const unsigned char *b, size_t n)
{
    char p[512], cmd[700];
    snprintf(p, sizeof(p), "%s%s", root, rel);
    snprintf(cmd, sizeof(cmd), "mkdir -p \"$(dirname '%s')\"", p);
    if (system(cmd)) exit(2);
    FILE *f = fopen(p, "w");
    if (!f) { perror(p); exit(2); }
    if (fwrite(b, 1, n, f) != n) exit(2);
    fclose(f);
}

/* a shell command with the test root in it */
static void sh(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void sh(const char *fmt, ...)
{
    char cmd[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    if (system(cmd)) exit(2);
}

#define PCI "/sys/bus/pci/devices/0000:01:00.1/"
static const unsigned char CFG_OK[4] = {0xb3, 0x15, 0x13, 0x10};     /* 15b3:1013, little endian */
static const unsigned char CFG_DEAD[4] = {0xff, 0xff, 0xff, 0xff};

#define HW "/sys/devices/pci0000:00/0000:01:00.1/hwmon/hwmon2/"
#define PORT "/sys/class/infiniband/rocep1s0f1/ports/1/"

static void temp(double c)
{
    char b[32];
    snprintf(b, sizeof(b), "%ld", (long)(c * 1000));
    put(HW "temp2_input", b);
}

static struct rdx_link_info sample(void)
{
    struct rdx_link_info li;
    rdx_link_sample_once();
    rdx_link_get(&li);
    return li;
}

int main(int argc, char **argv)
{
    if (argc > 1) {                                  /* read-only on a real machine: test_link --live 10.0.0.2 [seconds] */
        if (argc < 3 || strcmp(argv[1], "--live")) { fprintf(stderr, "test_link [--live IP [seconds]]\n"); return 2; }
        rdx_link_start(argv[2], NULL);
        sleep(argc > 3 ? (unsigned)atoi(argv[3]) : 3);
        struct rdx_link_info li;
        rdx_link_get(&li);
        cJSON *j = rdx_link_json(&li);
        char *t = cJSON_Print(j);
        printf("%s\n", t);
        free(t);
        cJSON_Delete(j);
        return 0;
    }
    snprintf(root, sizeof(root), "/tmp/rdx-test-link-%d", (int)getpid());
    put(PORT "gids/0", "fe80:0000:0000:0000:ba59:9fff:fe36:7081");
    put(PORT "gids/2", "0000:0000:0000:0000:0000:ffff:0a00:0002");
    put(PORT "gid_attrs/ndevs/2", "enp1s0f1np1");
    put(PORT "state", "4: ACTIVE");
    put(PORT "phys_state", "5: LinkUp");
    put(PORT "rate", "100 Gb/sec (4X EDR)");
    put(HW "temp1_label", "sensor0");
    put(HW "temp1_input", "84000");
    put(HW "temp2_label", "Module1");
    put(HW "temp2_crit", "53000");
    put(HW "temp2_highest", "58000");
    temp(49.1);
    char lnk[512];
    snprintf(lnk, sizeof(lnk), "%s/sys/class/infiniband/rocep1s0f1/device", root);
    if (symlink("../../../devices/pci0000:00/0000:01:00.1", lnk)) { perror("symlink"); return 2; }

    if (rdx_link_start("10.0.0.2", root)) { printf("FAIL start\n"); return 1; }
    struct rdx_link_info li = sample();
    CHECK(li.known && !strcmp(li.rdma_dev, "rocep1s0f1") && li.port == 1, "resolve: %s %d", li.rdma_dev, li.port);
    CHECK(!strcmp(li.pci, "0000:01:00.1") && !strcmp(li.netdev, "enp1s0f1np1"), "pci %s netdev %s", li.pci, li.netdev);
    CHECK(li.up && li.level == RDX_LINK_OK && !li.message[0], "ok: up %d level %d '%s'", li.up, li.level, li.message);
    CHECK(li.mod.present && fabs(li.mod.temp_c - 49.1) < 0.01 && li.mod.warn_c == 53 && li.mod.highest_c == 58,
          "module %d %.2f %.1f", li.mod.present, li.mod.temp_c, li.mod.warn_c);
    CHECK(!strcmp(li.rate, "100 Gb/sec (4X EDR)") && !strcmp(li.state, "ACTIVE") && !strcmp(li.phys_state, "LinkUp"),
          "rate '%s' state '%s' phys '%s'", li.rate, li.state, li.phys_state);
    CHECK(rdx_link_take(), "event after the first sample");
    CHECK(!rdx_link_take(), "no second event");

    /* the card itself: config space answers -> nothing; all ones ("PCI slot is unavailable") -> failed */
    put_bin(PCI "config", CFG_OK, 4);
    li = sample();
    CHECK(li.level == RDX_LINK_OK && !li.nic_failed, "card answers: level %d", li.level);
    CHECK(!rdx_link_take(), "no event while the card answers");
    put_bin(PCI "config", CFG_DEAD, 4);
    li = sample();
    CHECK(li.level == RDX_LINK_FAILED && li.nic_failed && li.nic_power_cycle && !li.nic_heat && li.downs == 1 && !li.up,
          "dead config: level %d failed %d cycle %d heat %d downs %ld", li.level, li.nic_failed, li.nic_power_cycle, li.nic_heat, li.downs);
    CHECK(!strcmp(li.message, "Network card failed (PCI device not answering): power-cycle needed"), "failed message '%s'", li.message);
    CHECK(strstr(li.nic_detail, "reads all ones"), "detail '%s'", li.nic_detail);
    CHECK(!strcmp(li.down_reason, "the network card failed") && li.have_down && li.back_at == 0, "failed reason '%s'", li.down_reason);
    CHECK(rdx_link_take(), "event on failed");
    char line[256];
    rdx_link_line(&li, NULL, li.down_at + 1, line, sizeof(line));
    CHECK(!strcmp(line, "This computer's network card failed (PCI device not answering): power-cycle needed"), "failed line '%s'", line);
    cJSON *j = rdx_link_json(&li);
    char *txt = cJSON_PrintUnformatted(j);
    CHECK(strstr(txt, "\"level\":\"failed\"") && strstr(txt, "\"nic\":{\"failed\":true,\"likely_overheated\":false,\"power_cycle\":true"),
          "failed json: %s", txt);
    free(txt);
    cJSON_Delete(j);
    li = sample();
    CHECK(li.downs == 1 && !rdx_link_take(), "staying failed counts once: downs %ld", li.downs);
    put_bin(PCI "config", CFG_OK, 4);
    li = sample();
    CHECK(li.level == RDX_LINK_OK && !li.nic_failed && li.back_at > 0 && li.up, "card back: level %d back_at %.0f", li.level, li.back_at);
    CHECK(rdx_link_take(), "event when the card is back");
    rdx_link_line(&li, NULL, li.back_at + 1, line, sizeof(line));
    CHECK(strstr(line, "The link was down for") && strstr(line, "network card failed"), "card back line '%s'", line);

    /* off the bus: its PCI directory is gone (seen before, so that is a verdict) */
    sh("rm -rf '%s'%s", root, PCI);
    li = sample();
    CHECK(li.level == RDX_LINK_FAILED && !strcmp(li.nic_why, "PCI device gone from the bus") && li.downs == 2,
          "gone from the bus: level %d '%s' downs %ld", li.level, li.nic_why, li.downs);
    put_bin(PCI "config", CFG_OK, 4);
    li = sample();
    CHECK(li.level == RDX_LINK_OK, "back on the bus: %d", li.level);

    /* the driver's own words: fatal error, the slot lost, recovery aborted; another function's lines are not ours */
    rdx_link_kmsg_record("3,5000,1,-;mlx5_core 0000:01:00.0: poll_health:785:(pid 0): Fatal error 1 detected");
    li = sample();
    CHECK(li.level == RDX_LINK_OK, "the other function's fatal error is not ours: %d", li.level);
    rdx_link_kmsg_record("3,5001,2,-;mlx5_core 0000:01:00.1: poll_health:785:(pid 3128): Fatal error 1 detected");
    rdx_link_kmsg_record("3,5002,3,-;mlx5_core 0000:01:00.1: print_health_info:425:(pid 3128): PCI slot is unavailable");
    li = sample();
    CHECK(li.level == RDX_LINK_FAILED && !strcmp(li.nic_why, "fatal error reported by the driver") && strstr(li.nic_detail, "Fatal error 1"),
          "fatal: level %d '%s' / '%s'", li.level, li.nic_why, li.nic_detail);
    rdx_link_kmsg_record("6,5003,4,-;mlx5_core 0000:01:00.1: mlx5_health_try_recover:338:(pid 1): health recovery flow aborted, PCI reads still not working");
    li = sample();
    CHECK(li.level == RDX_LINK_FAILED && li.downs == 3, "still failed after the abort line: %d downs %ld", li.level, li.downs);
    rdx_link_kmsg_record("6,5004,5,-;mlx5_core 0000:01:00.1 enp1s0f1np1: Link up");
    li = sample();
    CHECK(li.level == RDX_LINK_OK && li.back_at > 0, "recovered by the kernel's Link up: %d", li.level);

    /* the RDMA device vanishes while the card is on the bus: a grace time, then a failure that says reload or power-cycle */
    sh("mv '%s'/sys/class/infiniband/rocep1s0f1 '%s'/sys/class/infiniband/.away", root, root);
    li = sample();
    CHECK(li.level == RDX_LINK_UNKNOWN && strstr(li.message, "does not answer"), "RDMA device missing, grace: %d '%s'", li.level, li.message);
    setenv("RDX_LINK_NODEV_S", "0", 1);
    li = sample();
    CHECK(li.level == RDX_LINK_FAILED && !li.nic_power_cycle && strstr(li.message, "Network card not available") &&
          strstr(li.message, "reload the driver or power-cycle"), "RDMA device gone for good: %d '%s'", li.level, li.message);
    sh("mv '%s'/sys/class/infiniband/.away '%s'/sys/class/infiniband/rocep1s0f1", root, root);
    li = sample();
    li = sample();
    CHECK(li.level == RDX_LINK_OK && li.known && li.up, "RDMA device back: level %d known %d up %d '%s'", li.level, li.known, li.up, li.message);
    unsetenv("RDX_LINK_NODEV_S");
    rdx_link_take();

    temp(53.6);
    li = sample();
    CHECK(li.level == RDX_LINK_WARN && strstr(li.message, "53.6") && strstr(li.message, "warning threshold of 53"),
          "warn: %d '%s'", li.level, li.message);
    CHECK(rdx_link_take(), "event on warn");
    temp(52.5);                                       /* hysteresis: still warn */
    li = sample();
    CHECK(li.level == RDX_LINK_WARN, "hysteresis keeps warn at 52.5: %d", li.level);
    CHECK(!rdx_link_take(), "no event while the level stays");
    temp(53.9);
    sample();

    /* the kernel takes the link down for the module; an event of the other port is not ours */
    rdx_link_kmsg_record("3,4711,123,-;mlx5_core 0000:01:00.1: port_module:252:(pid 0): Port module event[error]: "
                         "module 1, Cable error, High Temperature");
    rdx_link_kmsg_record("3,4712,124,-;mlx5_core 0000:01:00.0: port_module:252:(pid 0): Port module event[error]: "
                         "module 0, Cable error, Cable unplugged");
    put(PORT "state", "1: DOWN");
    put(PORT "phys_state", "3: Disabled");
    li = sample();
    CHECK(li.level == RDX_LINK_DOWN && li.downs == 5, "down: %d downs %ld", li.level, li.downs);
    CHECK(!strcmp(li.down_reason, "the network module overheated") && strstr(li.down_detail, "High Temperature"),
          "reason '%s' detail '%s'", li.down_reason, li.down_detail);
    CHECK(strstr(li.message, "Link down: the network module overheated (53.9"), "message '%s'", li.message);
    CHECK(rdx_link_take(), "event on down");
    rdx_link_line(&li, NULL, li.down_at + 1, line, sizeof(line));
    CHECK(strstr(line, "Link down: this computer's network module overheated"), "own line '%s'", line);

    /* JSON round trip: the client's view of this host */
    j = rdx_link_json(&li);
    txt = cJSON_PrintUnformatted(j);
    struct rdx_link_info peer;
    CHECK(!rdx_link_from_json(j, &peer) && peer.level == RDX_LINK_DOWN && peer.have_down && peer.back_at == 0 &&
          !strcmp(peer.down_reason, li.down_reason), "json round trip: %s", txt);
    free(txt);
    cJSON_Delete(j);

    put(PORT "state", "4: ACTIVE");
    temp(47.0);
    li = sample();
    CHECK(li.level == RDX_LINK_OK && li.have_down && li.back_at > 0, "back: %d back_at %.0f", li.level, li.back_at);
    j = rdx_link_json(&li);
    txt = cJSON_PrintUnformatted(j);
    CHECK(strstr(txt, "\"down_s\":") && strstr(txt, "\"level\":\"ok\"") && strstr(txt, "\"warn_c\":53"), "json back: %s", txt);
    free(txt);
    cJSON_Delete(j);
    rdx_link_line(&li, NULL, li.back_at + 5, line, sizeof(line));
    CHECK(strstr(line, "The link was down for") && strstr(line, "this computer's network module overheated"), "back line '%s'", line);
    rdx_link_line(&li, NULL, li.back_at + RDX_LINK_RECENT_S + 1, line, sizeof(line));
    CHECK(!line[0], "back line gone after %d s: '%s'", RDX_LINK_RECENT_S, line);

    /* the client: its own port goes down without a reason; the host's module was hot */
    struct rdx_link_info own, host;
    memset(&own, 0, sizeof(own));
    own.mod.temp_c = own.mod.warn_c = own.mod.alarm_c = NAN;
    own.level = RDX_LINK_DOWN;
    own.have_down = 1;
    snprintf(own.down_reason, sizeof(own.down_reason), "no signal from the other side (its computer, module or the cable)");
    memset(&host, 0, sizeof(host));
    host.level = RDX_LINK_WARN;
    host.mod.present = 1;
    host.mod.temp_c = 53.6;
    host.mod.warn_c = 53;
    host.mod.alarm_c = 58;
    rdx_link_line(&own, &host, 0, line, sizeof(line));
    CHECK(!strcmp(line, "Link down: the other computer's network module overheated (53.6 \xc2\xb0""C, warning at 53 \xc2\xb0""C)"),
          "client line '%s'", line);
    own.level = RDX_LINK_OK;
    rdx_link_line(&own, &host, 0, line, sizeof(line));
    CHECK(strstr(line, "The other computer's network module: 53.6") && strstr(line, "off at 58"), "peer warn line '%s'", line);
    host.level = RDX_LINK_OK;
    rdx_link_line(&own, &host, 0, line, sizeof(line));
    CHECK(!line[0], "nothing to say: '%s'", line);

    /* EEPROM: SFF-8636 with page 03h thresholds (high alarm 58, high warning 53), vendor, part */
    unsigned char ee[640];
    memset(ee, 0, sizeof(ee));
    ee[22] = 49;
    ee[23] = 0x40;                                   /* 49.25 C */
    memcpy(ee + 148, "INTEL CORP      ", 16);
    memcpy(ee + 168, "SPTSBP2CLCKS    ", 16);
    ee[512] = 58;
    ee[516] = 53;
    struct rdx_link_module m;
    memset(&m, 0, sizeof(m));
    m.temp_c = m.warn_c = m.alarm_c = NAN;
    CHECK(!rdx_link_parse_eeprom(ETH_MODULE_SFF_8636, ee, sizeof(ee), &m, 1) && m.alarm_c == 58 && m.warn_c == 53 &&
          fabs(m.temp_c - 49.25) < 1e-9 && !strcmp(m.vendor, "INTEL CORP") && !strcmp(m.part, "SPTSBP2CLCKS"),
          "eeprom: alarm %.2f warn %.2f temp %.2f '%s' '%s'", m.alarm_c, m.warn_c, m.temp_c, m.vendor, m.part);
    ee[2] = 0x04;                                    /* flat memory: no page 03h */
    memset(&m, 0, sizeof(m));
    m.warn_c = m.alarm_c = NAN;
    rdx_link_parse_eeprom(ETH_MODULE_SFF_8636, ee, sizeof(ee), &m, 0);
    CHECK(isnan(m.alarm_c), "flat memory has no thresholds");

    /* a failure after heat says so: the module was at its warning threshold minutes ago */
    put_bin(PCI "config", CFG_DEAD, 4);
    li = sample();
    CHECK(li.level == RDX_LINK_FAILED && li.nic_heat && li.nic_power_cycle &&
          !strcmp(li.message, "Network card failed (likely overheated): power-cycle needed") &&
          !strcmp(li.down_reason, "the network card overheated"), "failed after heat: %d heat %d '%s' / '%s'", li.level, li.nic_heat,
          li.message, li.down_reason);
    j = rdx_link_json(&li);
    CHECK(!rdx_link_from_json(j, &peer) && peer.level == RDX_LINK_FAILED && peer.nic_heat && peer.nic_power_cycle &&
          !strcmp(peer.nic_why, "PCI device not answering"), "failed json round trip");
    cJSON_Delete(j);
    own.level = RDX_LINK_OK;
    rdx_link_line(&own, &peer, 0, line, sizeof(line));
    CHECK(!strcmp(line, "The other computer's network card failed (likely overheated): power-cycle needed"), "peer failed line '%s'", line);
    own.level = RDX_LINK_DOWN;                        /* its own port is down too: the card is still the news */
    rdx_link_line(&own, &peer, 0, line, sizeof(line));
    CHECK(strstr(line, "The other computer's network card failed"), "own down + peer failed '%s'", line);
    put_bin(PCI "config", CFG_OK, 4);
    li = sample();
    CHECK(li.level == RDX_LINK_OK, "card answers again: %d", li.level);

    /* the chip's temperature (mlx5 hwmon "sensor0", crit = its limit): a warning 10 degrees below, the alarm at it */
    put(HW "temp1_crit", "105000");
    put(HW "temp1_highest", "80000");
    li = sample();
    CHECK(li.level == RDX_LINK_OK && li.chip.present && li.chip.limit_c == 105 && li.chip.highest_c == 80 && fabs(li.chip.temp_c - 84) < 0.01,
          "chip ok: level %d present %d limit %.1f temp %.1f", li.level, li.chip.present, li.chip.limit_c, li.chip.temp_c);
    put(HW "temp1_input", "96000");
    li = sample();
    CHECK(li.level == RDX_LINK_WARN && li.hot == RDX_HOT_CHIP &&
          !strcmp(li.message, "Network card chip 96.0 \xc2\xb0""C \xe2\x80\x93 close to its limit of 105 \xc2\xb0""C"), "chip warn: %d hot %d '%s'", li.level, li.hot, li.message);
    rdx_link_line(&li, NULL, 0, line, sizeof(line));
    CHECK(!strcmp(line, "This computer's network card chip: 96.0 \xc2\xb0""C (limit 105 \xc2\xb0""C)"), "chip warn line '%s'", line);
    j = rdx_link_json(&li);
    txt = cJSON_PrintUnformatted(j);
    CHECK(strstr(txt, "\"chip\":{\"label\":\"sensor0\",\"temp_c\":96,\"limit_c\":105,\"highest_c\":80") && strstr(txt, "\"hot\":\"chip\""), "chip json: %s", txt);
    free(txt);
    CHECK(!rdx_link_from_json(j, &peer) && peer.hot == RDX_HOT_CHIP && peer.chip.present && peer.chip.limit_c == 105, "chip json round trip");
    cJSON_Delete(j);
    put(HW "temp1_input", "94500");                   /* 1 degree hysteresis: 94.5 is still warn */
    li = sample();
    CHECK(li.level == RDX_LINK_WARN, "chip hysteresis: %d", li.level);
    put(HW "temp1_input", "93500");
    li = sample();
    CHECK(li.level == RDX_LINK_OK && li.hot == RDX_HOT_NONE && !li.message[0], "chip cooled: %d '%s'", li.level, li.message);
    put(HW "temp1_input", "105200");
    li = sample();
    CHECK(li.level == RDX_LINK_ALARM && li.hot == RDX_HOT_CHIP && strstr(li.message, "at its limit of 105"), "chip alarm: %d '%s'", li.level, li.message);
    rdx_link_line(&li, NULL, 0, line, sizeof(line));
    CHECK(strstr(line, "network card chip: 105.2") && strstr(line, "at its limit"), "chip alarm line '%s'", line);
    put(HW "temp1_input", "60000");
    li = sample();
    CHECK(li.level == RDX_LINK_OK, "chip cool again: %d", li.level);

    /* the kernel log's heat reports: module bits alone are the module's business, bit 0 is the chip (a ConnectX-4 has no hwmon) */
    rdx_link_kmsg_record("4,6000,1,-;mlx5_core 0000:01:00.1: temp_warn:168:(pid 0): High temperature on sensors with bit set 2 0");
    li = sample();
    CHECK(li.level == RDX_LINK_OK && !li.chip.event, "module bits are not the chip: %d", li.level);
    rdx_link_kmsg_record("4,6001,2,-;mlx5_core 0000:01:00.1: temp_warn:168:(pid 0): High temperature on sensors with bit set 0 1");
    li = sample();
    CHECK(li.level == RDX_LINK_ALARM && li.hot == RDX_HOT_CHIP && li.chip.event && strstr(li.message, "card may fail") &&
          strstr(li.chip.event_text, "high chip temperature"), "chip event: %d '%s'", li.level, li.message);
    rdx_link_line(&li, NULL, 0, line, sizeof(line));
    CHECK(!strcmp(line, "This computer's network card is too hot (60.0 \xc2\xb0""C) \xe2\x80\x93 it may fail: improve its cooling"), "chip event line '%s'", line);
    sh("rm -f '%s'%stemp1_input '%s'%stemp1_label", root, HW, root, HW);            /* now like a ConnectX-4: no sensor at all */
    rdx_link_kmsg_record("3,6002,3,-;mlx5_core 0000:01:00.1: print_health_info:431:(pid 0): Health issue observed, High temperature, severity(3) ERROR:");
    li = sample();
    CHECK(li.level == RDX_LINK_ALARM && li.hot == RDX_HOT_CHIP && !li.chip.present && li.chip.event &&
          strstr(li.chip.event_text, "firmware reports: High temperature"), "health issue without hwmon: %d '%s' / '%s'", li.level, li.message,
          li.chip.event_text);
    rdx_link_line(&li, NULL, 0, line, sizeof(line));
    CHECK(!strcmp(line, "This computer's network card is too hot \xe2\x80\x93 it may fail: improve its cooling"), "no-sensor line '%s'", line);

    /* the client: its port goes down without a reason; the host's card was too hot */
    memset(&host, 0, sizeof(host));
    host.chip.temp_c = host.chip.limit_c = host.chip.highest_c = NAN;
    host.level = RDX_LINK_ALARM;
    host.hot = RDX_HOT_CHIP;
    host.chip.present = 1;
    host.chip.temp_c = 101.0;
    host.chip.limit_c = 105;
    own.level = RDX_LINK_DOWN;
    rdx_link_line(&own, &host, 0, line, sizeof(line));
    CHECK(!strcmp(line, "Link down: the other computer's network card overheated (101.0 \xc2\xb0""C)"), "client line, hot chip '%s'", line);

    /* no port carries the link IP (the daemon started after the card died): a dead Mellanox function is why */
    rdx_link_target("10.0.99.9");
    li = sample();
    CHECK(!li.known && li.level == RDX_LINK_UNKNOWN && strstr(li.message, "10.0.99.9"), "unknown: '%s'", li.message);
    put("/sys/bus/pci/devices/0000:06:00.0/vendor", "0x8086");                      /* a dead card of another make is not our business */
    put_bin("/sys/bus/pci/devices/0000:06:00.0/config", CFG_DEAD, 4);
    li = sample();
    CHECK(li.level == RDX_LINK_UNKNOWN, "another vendor's dead function: %d", li.level);
    put("/sys/bus/pci/devices/0000:05:00.0/vendor", "0x15b3");
    put_bin("/sys/bus/pci/devices/0000:05:00.0/config", CFG_DEAD, 4);
    li = sample();
    CHECK(li.level == RDX_LINK_FAILED && !strcmp(li.pci, "0000:05:00.0") && strstr(li.nic_detail, "0000:05:00.0") && li.nic_power_cycle,
          "dead Mellanox function, no port: %d pci '%s' '%s'", li.level, li.pci, li.message);
    put_bin("/sys/bus/pci/devices/0000:05:00.0/config", CFG_OK, 4);
    li = sample();
    CHECK(li.level == RDX_LINK_UNKNOWN && !li.nic_failed, "that function answers again: %d", li.level);

    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd)) {}
    printf("%s: test_link (%d failures)\n", fails ? "FAIL" : "ok", fails);
    return fails ? 1 : 0;
}
