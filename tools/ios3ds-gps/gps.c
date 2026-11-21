#include "gps.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

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
    double la = strtod(s, &end);
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
    double lo = strtod(p, &end);
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
        double v = strtod(q, &end);
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