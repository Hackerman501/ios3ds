#include "gps.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The iOS 4.2 stub SDK used by scripts/ios-cc does not export strtod(), so
   parse the decimal numbers ourselves (plain base-10, optional exponent).
   On a parse failure *end is set to the start, like strtod(). */
static double gps_number(const char *s, char **end)
{
    const char *p = s;
    int neg = 0;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }
    double v = 0.0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10.0 + (double)(*p - '0');
        p++; digits++;
    }
    if (*p == '.') {
        p++;
        double scale = 0.1;
        while (*p >= '0' && *p <= '9') {
            v += (double)(*p - '0') * scale;
            scale *= 0.1;
            p++; digits++;
        }
    }
    if (!digits) {
        if (end) *end = (char *)s;
        return 0.0;
    }
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        int eneg = 0;
        if (*q == '+' || *q == '-') { eneg = (*q == '-'); q++; }
        const char *first = q;
        int e = 0;
        while (*q >= '0' && *q <= '9') { e = e * 10 + (*q - '0'); q++; }
        if (q != first) {
            p = q;
            while (e-- > 0) { if (eneg) v *= 0.1; else v *= 10.0; }
        }
    }
    if (end) *end = (char *)p;
    return neg ? -v : v;
}

int gps_parse(const char *s, double *lat, double *lon, double *acc)
{
    if (!s || !lat || !lon || !acc)
        return -1;
    /* skip leading spaces */
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    if (*s == '\0')
        return -1;

    char *end = NULL;
    double la = gps_number(s, &end);
    if (end == s)
        return -1;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
        end++;
    if (*end != ',')
        return -1;
    if (!isfinite(la) || la < -90.0 || la > 90.0)
        return -1;

    char *p = end + 1;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    double lo = gps_number(p, &end);
    if (end == p)
        return -1;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
        end++;
    /* allow trailing whitespace or a third field */
    if (*end != '\0' && *end != ',')
        return -1;
    if (!isfinite(lo) || lo < -180.0 || lo > 180.0)
        return -1;

    double a = 50.0;
    if (*end == ',') {
        char *q = end + 1;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
            q++;
        double v = gps_number(q, &end);
        if (end == q)
            return -1;
        while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
            end++;
        if (*end != '\0')
            return -1;
        if (!isfinite(v) || v <= 0.0)
            return -1;
        a = v;
    } else {
        if (*end != '\0')
            return -1;
    }

    *lat = la;
    *lon = lo;
    *acc = a;
    return 0;
}