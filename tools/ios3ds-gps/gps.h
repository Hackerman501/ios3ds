/* Shared GPS parsing/validation for ios3ds. */
#ifndef IOS3DS_GPS_H
#define IOS3DS_GPS_H

/* Parse "LAT,LON" or "LAT,LON,ACC" (decimal degrees; ACC metres, default 50).
 * Returns 0 on success, -1 on any parse/range error. */
int gps_parse(const char *s, double *lat, double *lon, double *acc);

#endif