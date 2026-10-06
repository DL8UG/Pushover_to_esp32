// PC simulator for the e-paper UI: renders every screen of main/ui.cpp for
// a fixed set of scenarios into 1-bit PBM files (one per scenario), which
// tools/sim_png.py turns into PNGs. Usage: sim <output dir>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "ui.h"

namespace {

// 2026-10-06 14:32 local time (TZ is set to Central Europe in main())
constexpr int64_t SAMPLE_DATE = 1791289920;

UiState home(int count)
{
    UiState s;
    s.now = SAMPLE_DATE + 2 * 3600 + 15 * 60; // 16:47
    s.wifi_ok = true;
    s.pushover_ok = true;
    s.count = count;
    return s;
}

UiState message(const char *title, int index, int total, int priority = 1)
{
    UiState s;
    s.screen = UiState::Screen::Message;
    snprintf(s.title, sizeof(s.title), "%s", title);
    s.date = SAMPLE_DATE;
    s.priority = priority;
    s.index = index;
    s.total = total;
    return s;
}

bool write_pbm(const char *dir, const char *name, const Canvas &c)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.pbm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return false; }
    fprintf(f, "P4\n%d %d\n", CANVAS_W, CANVAS_H);
    for (int y = 0; y < CANVAS_H; y++) {
        for (int bx = 0; bx < CANVAS_W / 8; bx++) {
            unsigned char b = 0;
            for (int i = 0; i < 8; i++) {
                if (c.get(bx * 8 + i, y)) b |= 0x80 >> i;
            }
            fputc(b, f);
        }
    }
    fclose(f);
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <output dir>\n", argv[0]);
        return 2;
    }
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    struct Scenario {
        const char *name;
        UiState state;
    };
    UiState no_wifi = home(0);
    no_wifi.wifi_ok = false;
    no_wifi.pushover_ok = false;
    no_wifi.now = 0; // clock not set yet
    UiState no_pushover = home(2);
    no_pushover.pushover_ok = false;
    UiState muted = home(3);
    muted.alarm_enabled = false;
    UiState unreachable = home(0);
    unreachable.pushover_ok = false;
    unreachable.unreachable = true;
    UiState unreachable_msgs = home(4);
    unreachable_msgs.wifi_ok = false;
    unreachable_msgs.pushover_ok = false;
    unreachable_msgs.unreachable = true;

    const Scenario scenarios[] = {
        {"home-empty", home(0)},
        {"home-one", home(1)},
        {"home-many", home(12)},
        {"home-muted", muted},
        {"home-no-wifi", no_wifi},
        {"home-no-pushover", no_pushover},
        {"home-not-connected", unreachable},
        {"home-not-connected-msgs", unreachable_msgs},
        {"msg-short", message("Server down", 1, 5)},
        {"msg-medium", message("Backup failed on nas01: disk full", 0, 3)},
        {"msg-umlauts", message("Wärmepumpe: Störung Außeneinheit (Fehler 0x1F)", 2, 3)},
        {"msg-long", message("Zabbix PROBLEM: High CPU utilization on web01.example.org for 15 minutes",
                             3, 4)},
        {"msg-longword", message("Disk /dev/disk/by-id/ata-WDC_WD40EFRX-68N32N0 failing", 0, 1)},
        {"msg-emergency", message("Water leak detected in basement!", 0, 2, 2)},
        {"msg-many", message("Door opened", 17, 30)},
    };

    for (const auto &sc : scenarios) {
        Canvas c;
        ui_render(sc.state, c);
        if (!write_pbm(argv[1], sc.name, c)) return 1;
    }
    printf("rendered %zu screens into %s\n", sizeof(scenarios) / sizeof(scenarios[0]), argv[1]);
    return 0;
}
