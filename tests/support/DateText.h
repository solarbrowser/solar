#pragma once

#include <chrono>
#include <cstdio>
#include <string>

namespace solar::test {

// A date made by calendar arithmetic that shares nothing with the code under test.
inline std::chrono::system_clock::time_point At(int year, int month, int day, int hour = 0, int minute = 0, int second = 0) {
  const int a = (14 - month) / 12;
  const int y = year + 4800 - a;
  const int m = month + 12 * a - 3;
  const long jdn = day + (153 * m + 2) / 5 + 365L * y + y / 4 - y / 100 + y / 400 - 32045;
  const long days = jdn - 2440588;
  return std::chrono::system_clock::time_point(std::chrono::seconds(days * 86400 + hour * 3600 + minute * 60 + second));
}

// "Sun, 06 Nov 1994 08:49:37 GMT".
inline std::string FormatDate(std::chrono::system_clock::time_point time) {
  const long total = std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
  const long days = total / 86400;
  const long rest = total % 86400;
  static const char* const kDays[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
  static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const long z = days + 719468;
  const long era = (z >= 0 ? z : z - 146096) / 146097;
  const long doe = z - era * 146097;
  const long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const long mp = (5 * doy + 2) / 153;
  const long day = doy - (153 * mp + 2) / 5 + 1;
  const long month = mp < 10 ? mp + 3 : mp - 9;
  const long year = yoe + era * 400 + (month <= 2);
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%s, %02ld %s %04ld %02ld:%02ld:%02ld GMT", kDays[((days % 7) + 7) % 7], day, kMonths[month - 1], year,
                rest / 3600, rest / 60 % 60, rest % 60);
  return buffer;
}

}  // namespace solar::test
