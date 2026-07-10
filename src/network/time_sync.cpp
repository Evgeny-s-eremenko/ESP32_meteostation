#include "time_sync.h"
#include "../state.h"
#include "../config.h"
#include <sunset.h>

extern SunSet sun;

static double astroDegToRad(double deg) {
  return M_PI * deg / 180.0;
}

static double astroRadToDeg(double rad) {
  return 180.0 * rad / M_PI;
}

static double astroCalcJD(int y, int m, int d) {
  if (m <= 2) { y--; m += 12; }
  double A = floor(y / 100.0);
  double B = 2.0 - A + floor(A / 4.0);
  return floor(365.25 * (y + 4716)) + floor(30.6001 * (m + 1)) + d + B - 1524.5;
}

static double astroCalcEquationOfTime(double jd) {
  double T = (jd - 2451545.0) / 36525.0;

  double L0 = 280.46646 + T * (36000.76983 + 0.0003032 * T);
  L0 = fmod(L0, 360.0);
  if (L0 < 0) L0 += 360.0;

  double M = 357.52911 + T * (35999.05029 - 0.0001537 * T);
  M = fmod(M, 360.0);
  if (M < 0) M += 360.0;

  double epsilon = 23.0 + (26.0 + (21.448 - T * (46.8150 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0;
  double omega = 125.04 - 1934.136 * T;
  double e = epsilon + 0.00256 * cos(astroDegToRad(omega));

  double y = tan(astroDegToRad(e) / 2.0);
  y *= y;

  double sin2L0 = sin(2.0 * astroDegToRad(L0));
  double sinM   = sin(astroDegToRad(M));
  double cos2L0 = cos(2.0 * astroDegToRad(L0));
  double sin4L0 = sin(4.0 * astroDegToRad(L0));
  double sin2M  = sin(2.0 * astroDegToRad(M));

  double Etime = y * sin2L0 - 2.0 * 0.016708634 * sinM
               + 4.0 * 0.016708634 * y * sinM * cos2L0
               - 0.5 * y * y * sin4L0
               - 1.25 * 0.016708634 * 0.016708634 * sin2M;

  return astroRadToDeg(Etime) * 4.0;
}

static double astroCalcSunDeclination(double jd) {
  double T = (jd - 2451545.0) / 36525.0;

  double epsilon0 = 23.0 + (26.0 + (21.448 - T * (46.8150 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0;
  double omega = 125.04 - 1934.136 * T;
  double epsilon = epsilon0 + 0.00256 * cos(astroDegToRad(omega));

  double L0 = 280.46646 + T * (36000.76983 + 0.0003032 * T);
  L0 = fmod(L0, 360.0);
  if (L0 < 0) L0 += 360.0;

  double M = 357.52911 + T * (35999.05029 - 0.0001537 * T);
  M = fmod(M, 360.0);
  if (M < 0) M += 360.0;

  double C = sin(astroDegToRad(M)) * (1.914602 - T * (0.004817 + 0.000014 * T))
           + sin(astroDegToRad(2.0 * M)) * (0.019993 - 0.000101 * T)
           + sin(astroDegToRad(3.0 * M)) * 0.000289;

  double sunLong = L0 + C;
  double lambda = sunLong - 0.00569 - 0.00478 * sin(astroDegToRad(omega));

  double sint = sin(astroDegToRad(epsilon)) * sin(astroDegToRad(lambda));
  return astroRadToDeg(asin(sint));
}

double calcSunElevation(double latitude, double longitude, time_t timestamp) {
  struct tm *timeinfo = localtime(&timestamp);

  int y = timeinfo->tm_year + 1900;
  int m = timeinfo->tm_mon + 1;
  int d = timeinfo->tm_mday;
  double jd = astroCalcJD(y, m, d);

  double eqTime = astroCalcEquationOfTime(jd);
  double decl   = astroCalcSunDeclination(jd);

  double hour = timeinfo->tm_hour + timeinfo->tm_min / 60.0 + timeinfo->tm_sec / 3600.0;
  double solarTime = hour + (longitude - tzOffset * 15.0) * 4.0 / 60.0 + eqTime / 60.0;
  double hourAngle = (solarTime - 12.0) * 15.0;

  double latRad  = astroDegToRad(latitude);
  double decRad  = astroDegToRad(decl);
  double haRad   = astroDegToRad(hourAngle);

  double sinElev = sin(latRad) * sin(decRad) + cos(latRad) * cos(decRad) * cos(haRad);
  return astroRadToDeg(asin(sinElev));
}

double calcSolarNoon(double longitude, time_t timestamp) {
  struct tm *timeinfo = localtime(&timestamp);

  int y = timeinfo->tm_year + 1900;
  int m = timeinfo->tm_mon + 1;
  int d = timeinfo->tm_mday;
  double jd = astroCalcJD(y, m, d);

  double eqTime = astroCalcEquationOfTime(jd);

  double solarNoonUTC = 720.0 - eqTime - longitude * 4.0;
  double solarNoonLocal = (solarNoonUTC + tzOffset * 60.0) * 60.0;

  while (solarNoonLocal < 0) solarNoonLocal += 86400.0;
  while (solarNoonLocal >= 86400.0) solarNoonLocal -= 86400.0;

  return solarNoonLocal;
}

void taskGetTime(void *pvParameters) {
  static int currentDay = 32;
  struct tm  timeinfo;

  for (;;) {
    if (getLocalTime(&timeinfo)) {
      if (currentDay != timeinfo.tm_mday) {
        sun.setCurrentDate(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
        sun.setPosition(latitude, longitude, tzOffset);
        sunriseTime = sun.calcSunrise();
        sunsetTime  = sun.calcSunset();
        currentDay  = timeinfo.tm_mday;
      }
      month = timeinfo.tm_mon + 1;
    } else {
      ESP_LOGE("NTP", "Не удалось получить время по NTP");
    }
    vTaskDelay(pdMS_TO_TICKS(60000));
  }
}
