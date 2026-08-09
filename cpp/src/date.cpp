#include "wbt/types.hpp"
#include <ctime>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace wbt {

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

static std::tm to_tm(Date d) {
  std::tm tm{};
  tm.tm_year = year_of(d) - 1900;
  tm.tm_mon = month_of(d) - 1;
  tm.tm_mday = day_of(d);
  tm.tm_isdst = -1;
  return tm;
}

static Date from_tm(const std::tm& tm) {
  return ymd(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

Date add_days(Date d, int n) {
  std::tm tm = to_tm(d);
  std::time_t t = timegm(&tm);
  t += static_cast<std::time_t>(n) * 86400;
  std::tm* out = gmtime(&t);
  if (!out) throw std::runtime_error("add_days failed");
  return from_tm(*out);
}

int days_between(Date a, Date b) {
  std::tm ta = to_tm(a), tb = to_tm(b);
  std::time_t ea = timegm(&ta);
  std::time_t eb = timegm(&tb);
  return static_cast<int>((eb - ea) / 86400);
}

Date third_friday(int year, int month) {
  // 1st of month + ((4 - weekday) % 7) + 14
  Date first = ymd(year, month, 1);
  std::tm tm = to_tm(first);
  timegm(&tm);  // normalize wday
  int wday = tm.tm_wday;  // 0=Sun ... 5=Fri
  // Python datetime.weekday(): Mon=0 ... Fri=4
  int py_wd = (wday + 6) % 7;
  int add = ((4 - py_wd) % 7 + 7) % 7 + 14;
  return add_days(first, add);
}

}  // namespace wbt
