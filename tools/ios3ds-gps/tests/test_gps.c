#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "gps.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

int main(void)
{
    double la, lo, ac;

    CHECK(gps_parse("52.5200,13.4050", &la, &lo, &ac) == 0, "valid basic");
    CHECK(fabs(la - 52.52) < 1e-9 && fabs(lo - 13.405) < 1e-9 && fabs(ac - 50) < 1e-9, "defaults");

    CHECK(gps_parse("0,0,10", &la, &lo, &ac) == 0, "acc");
    CHECK(fabs(ac - 10) < 1e-9, "acc value");

    CHECK(gps_parse("-90,-180,1", &la, &lo, &ac) == 0, "min bounds");
    CHECK(gps_parse("90,180,1", &la, &lo, &ac) == 0, "max bounds");

    CHECK(gps_parse("", &la, &lo, &ac) != 0, "empty");
    CHECK(gps_parse("91,0", &la, &lo, &ac) != 0, "lat high");
    CHECK(gps_parse("-91,0", &la, &lo, &ac) != 0, "lat low");
    CHECK(gps_parse("0,181", &la, &lo, &ac) != 0, "lon high");
    CHECK(gps_parse("0,-181", &la, &lo, &ac) != 0, "lon low");
    CHECK(gps_parse("1,2,0", &la, &lo, &ac) != 0, "acc zero");
    CHECK(gps_parse("1,2,-5", &la, &lo, &ac) != 0, "acc neg");
    CHECK(gps_parse("1,2,abc", &la, &lo, &ac) != 0, "acc junk");
    CHECK(gps_parse("abc,2", &la, &lo, &ac) != 0, "lat junk");
    CHECK(gps_parse("1", &la, &lo, &ac) != 0, "missing lon");
    CHECK(gps_parse("1,2,3,4", &la, &lo, &ac) != 0, "extra field");
    CHECK(gps_parse("nan,0", &la, &lo, &ac) != 0, "nan lat");
    CHECK(gps_parse("inf,0", &la, &lo, &ac) != 0, "inf lat");
    CHECK(gps_parse("0,inf", &la, &lo, &ac) != 0, "inf lon");
    CHECK(gps_parse("1,2,nan", &la, &lo, &ac) != 0, "nan acc");
    CHECK(gps_parse("1,2,,3", &la, &lo, &ac) != 0, "empty acc");
    CHECK(gps_parse("1x,2", &la, &lo, &ac) != 0, "trailing lat junk");
    CHECK(gps_parse("1,2 ", &la, &lo, &ac) == 0, "trailing whitespace");
    CHECK(gps_parse(NULL, &la, &lo, &ac) != 0, "null string");

    /* whitespace tolerance */
    CHECK(gps_parse("  1.5 , -2.5 , 7 ", &la, &lo, &ac) == 0, "spaces");

    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}