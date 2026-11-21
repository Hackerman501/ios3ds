/* no CoreFoundation dependency */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int mkdir(const char *, int);
unsigned int notify_post(const char *);
#include <unistd.h>

#include "gps.h"

static const char *PLIST = "/var/mobile/Library/Preferences/com.ios3ds.location.plist";
/* same data in a world-readable place: sandboxed apps cannot read every
   path under /var/mobile, but /private/var/tmp is allowed by iOS 4's profile */
static const char *MIRROR = "/private/var/tmp/com.ios3ds.location.plist";
static const char *NOTIFY = "com.ios3ds.location.changed";

static void write_plist_enabled(const char *path, int enabled, double lat, double lon, double acc)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        perror(path);
        exit(1);
    }
    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f, "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n");
    fprintf(f, "<plist version=\"1.0\"><dict>\n");
    fprintf(f, "  <key>Enabled</key><%s/>\n", enabled ? "true" : "false");
    if (enabled) {
        fprintf(f, "  <key>Latitude</key><real>%.10g</real>\n", lat);
        fprintf(f, "  <key>Longitude</key><real>%.10g</real>\n", lon);
        fprintf(f, "  <key>Accuracy</key><real>%.10g</real>\n", acc);
    }
    fprintf(f, "</dict></plist>\n");
    fclose(f);
}

static void do_set(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: ios3ds-gps set LAT LON [ACC]\n");
        exit(2);
    }
    char buf[256];
    if (argc >= 5)
        snprintf(buf, sizeof(buf), "%s,%s,%s", argv[2], argv[3], argv[4]);
    else
        snprintf(buf, sizeof(buf), "%s,%s", argv[2], argv[3]);
    double lat, lon, acc;
    if (gps_parse(buf, &lat, &lon, &acc) != 0) {
        fprintf(stderr, "ios3ds-gps: invalid location (lat -90..90, lon -180..180, acc > 0)\n");
        exit(1);
    }
    mkdir("/var/mobile/Library", 0755);
    mkdir("/var/mobile/Library/Preferences", 0755);
    write_plist_enabled(PLIST, 1, lat, lon, acc);
    write_plist_enabled(MIRROR, 1, lat, lon, acc);
    printf("location set to %.6f, %.6f (accuracy %.1f m)\n", lat, lon, acc);
    notify_post(NOTIFY);
}

static void do_off(void)
{
    mkdir("/var/mobile/Library", 0755);
    mkdir("/var/mobile/Library/Preferences", 0755);
    write_plist_enabled(PLIST, 0, 0, 0, 0);
    write_plist_enabled(MIRROR, 0, 0, 0, 0);
    notify_post(NOTIFY);
    printf("location off\n");
}

static void do_show(void)
{
    FILE *f = fopen(PLIST, "r");
    if (!f) {
        printf("location: off (no preferences; boot arg may still enable it)\n");
        return;
    }
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);

    int enabled = 0;
    char *p = strstr(buf, "<key>Enabled</key>");
    if (p) {
        if (strstr(p, "<true/>")) enabled = 1;
    }
    if (!enabled) {
        printf("location: off\n");
        return;
    }
    double lat = 0, lon = 0, acc = 50;
    char *q;
    q = strstr(buf, "<key>Latitude</key>");
    if (q) { q = strstr(q, "<real>"); if (q) sscanf(q, "<real>%lf</real>", &lat); }
    q = strstr(buf, "<key>Longitude</key>");
    if (q) { q = strstr(q, "<real>"); if (q) sscanf(q, "<real>%lf</real>", &lon); }
    q = strstr(buf, "<key>Accuracy</key>");
    if (q) { q = strstr(q, "<real>"); if (q) sscanf(q, "<real>%lf</real>", &acc); }
    printf("location: %.6f, %.6f (accuracy %.1f m)\n", lat, lon, acc);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: ios3ds-gps set LAT LON [ACC] | off | show\n");
        return 2;
    }
    if (!strcmp(argv[1], "set"))
        do_set(argc, argv);
    else if (!strcmp(argv[1], "off"))
        do_off();
    else if (!strcmp(argv[1], "show"))
        do_show();
    else {
        fprintf(stderr, "usage: ios3ds-gps set LAT LON [ACC] | off | show\n");
        return 2;
    }
    return 0;
}