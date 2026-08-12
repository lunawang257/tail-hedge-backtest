#include "wbt/types.hpp"
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace wbt {
namespace {

// Howard Hinnant civil calendar (UTC days). Matches timegm-based arithmetic
// for Gregorian dates without libc timezone conversion overhead.
constexpr int days_from_civil(int y, unsigned m, unsigned d) noexcept {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int>(doe) - 719468;
}

constexpr void civil_from_days(int z, int& y, unsigned& m, unsigned& d) noexcept {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = static_cast<int>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y += (m <= 2);
}

inline int to_serial(Date date) {
  return days_from_civil(year_of(date), static_cast<unsigned>(month_of(date)),
                         static_cast<unsigned>(day_of(date)));
}

inline Date from_serial(int serial) {
  int y;
  unsigned m, d;
  civil_from_days(serial, y, m, d);
  return ymd(y, static_cast<int>(m), static_cast<int>(d));
}

}  // namespace

Date parse_date(const std::string& s) {
  int y = 0, m = 0, d = 0;
  if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) == 3) return ymd(y, m, d);
  if (std::sscanf(s.c_str(), "%d/%d/%d", &m, &d, &y) == 3) {
    if (y < 100) y += 2000;
    return ymd(y, m, d);
  }
  throw std::runtime_error("bad date: " + s);
}

std::string format_date(Date d) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year_of(d), month_of(d), day_of(d));
  return buf;
}

Date add_days(Date d, int n) {
  return from_serial(to_serial(d) + n);
}

int days_between(Date a, Date b) {
  return to_serial(b) - to_serial(a);
}

Date third_friday(int year, int month) {
  // weekday: 0=Sun .. 6=Sat via days since 1970-01-01 (Thu)
  Date first = ymd(year, month, 1);
  int serial = to_serial(first);
  int wday = (serial + 4) % 7;  // 1970-01-01 was Thursday
  if (wday < 0) wday += 7;
  // Python datetime.weekday(): Mon=0 ... Fri=4
  int py_wd = (wday + 6) % 7;
  int add = ((4 - py_wd) % 7 + 7) % 7 + 14;
  return from_serial(serial + add);
}

}  // namespace wbt
