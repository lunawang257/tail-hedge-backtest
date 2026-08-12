#include "wbt/data.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <unordered_set>
#include <cstdint>
#include <cstring>
#include <memory>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

namespace wbt {
namespace {

std::vector<std::string> split_csv(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  bool in_q = false;
  for (char c : line) {
    if (c == '"') { in_q = !in_q; continue; }
    if (c == ',' && !in_q) { out.push_back(cur); cur.clear(); }
    else cur.push_back(c);
  }
  out.push_back(cur);
  return out;
}

int col_index(const std::vector<std::string>& cols, const std::string& name) {
  for (size_t i = 0; i < cols.size(); ++i)
    if (cols[i] == name) return static_cast<int>(i);
  return -1;
}

bool ends_with_gz(const std::string& path) {
  return path.size() >= 3 &&
         (path.compare(path.size() - 3, 3, ".gz") == 0 ||
          path.compare(path.size() - 3, 3, ".GZ") == 0);
}

// Memory-mapped whole-file view (plain CSV). Sequential advice for scans.
class MappedFile {
 public:
  explicit MappedFile(const std::string& path) {
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) throw std::runtime_error("cannot open " + path);
    struct stat st {};
    if (::fstat(fd_, &st) != 0) {
      ::close(fd_);
      throw std::runtime_error("cannot stat " + path);
    }
    size_ = static_cast<size_t>(st.st_size);
    if (size_ == 0) {
      data_ = nullptr;
      return;
    }
    void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (p == MAP_FAILED) {
      ::close(fd_);
      throw std::runtime_error("cannot mmap " + path);
    }
    data_ = static_cast<const char*>(p);
    ::madvise(const_cast<char*>(data_), size_, MADV_SEQUENTIAL | MADV_WILLNEED);
  }
  ~MappedFile() {
    if (data_ && size_) ::munmap(const_cast<char*>(data_), size_);
    if (fd_ >= 0) ::close(fd_);
  }
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  const char* data() const { return data_; }
  size_t size() const { return size_; }

 private:
  int fd_ = -1;
  const char* data_ = nullptr;
  size_t size_ = 0;
};

// Line reader for gzip-compressed text files (and fallback).
class LineReader {
 public:
  explicit LineReader(const std::string& path) : path_(path), gzip_(ends_with_gz(path)) {
    if (gzip_) {
      gz_ = gzopen(path.c_str(), "rb");
      if (!gz_) throw std::runtime_error("cannot open gzip file: " + path);
      gzbuffer(gz_, 1 << 20);
    } else {
      file_buf_ = std::make_unique<char[]>(1 << 20);
      plain_.rdbuf()->pubsetbuf(file_buf_.get(), 1 << 20);
      plain_.open(path);
      if (!plain_) throw std::runtime_error("cannot open " + path);
    }
  }
  ~LineReader() {
    if (gz_) gzclose(gz_);
  }
  LineReader(const LineReader&) = delete;
  LineReader& operator=(const LineReader&) = delete;

  bool getline(std::string& out) {
    out.clear();
    if (gzip_) {
      char buf[8192];
      if (!gzgets(gz_, buf, sizeof(buf))) return false;
      out.assign(buf);
      while (!out.empty() && out.back() != '\n' && !gzeof(gz_)) {
        if (!gzgets(gz_, buf, sizeof(buf))) break;
        out.append(buf);
      }
      if (!out.empty() && out.back() == '\n') out.pop_back();
      if (!out.empty() && out.back() == '\r') out.pop_back();
      return true;
    }
    if (!std::getline(plain_, out)) return false;
    if (!out.empty() && out.back() == '\r') out.pop_back();
    return true;
  }

 private:
  std::string path_;
  bool gzip_ = false;
  gzFile gz_ = nullptr;
  std::ifstream plain_;
  std::unique_ptr<char[]> file_buf_;
};

struct FieldSpan {
  const char* b = nullptr;
  const char* e = nullptr;
  bool empty() const { return b == e; }
};

inline bool next_field(const char*& p, const char* end, FieldSpan& out) {
  if (p >= end) return false;
  out.b = p;
  while (p < end && *p != ',') ++p;
  out.e = p;
  if (p < end && *p == ',') ++p;
  return true;
}

// Hand-rolled decimal parser for option quotes (no exponent). Faster than from_chars.
inline bool parse_double_raw(const char* b, const char* e, double& out) {
  if (b >= e) return false;
  bool neg = false;
  if (*b == '-') {
    neg = true;
    ++b;
    if (b >= e) return false;
  } else if (*b == '+') {
    ++b;
    if (b >= e) return false;
  }

  uint64_t ip = 0;
  const char* p = b;
  while (p < e && *p >= '0' && *p <= '9') {
    ip = ip * 10u + static_cast<uint64_t>(*p - '0');
    ++p;
  }
  double val = static_cast<double>(ip);
  if (p < e && *p == '.') {
    ++p;
    uint64_t fp = 0;
    int n = 0;
    while (p < e && *p >= '0' && *p <= '9') {
      fp = fp * 10u + static_cast<uint64_t>(*p - '0');
      ++n;
      ++p;
    }
    if (n > 0) {
      static const double scale[] = {
          1.0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12};
      if (n < static_cast<int>(sizeof(scale) / sizeof(scale[0])))
        val += static_cast<double>(fp) / scale[n];
      else
        val += static_cast<double>(fp) / std::pow(10.0, n);
    }
  }
  if (p != e) return false;
  out = neg ? -val : val;
  return true;
}

inline Date parse_ymd_raw(const char* b, const char* e) {
  if (e - b >= 10 && b[4] == '-' && b[7] == '-') {
    auto dig = [](char c) -> int {
      return (c >= '0' && c <= '9') ? (c - '0') : -1;
    };
    int y0 = dig(b[0]), y1 = dig(b[1]), y2 = dig(b[2]), y3 = dig(b[3]);
    int m0 = dig(b[5]), m1 = dig(b[6]);
    int d0 = dig(b[8]), d1 = dig(b[9]);
    if ((y0 | y1 | y2 | y3 | m0 | m1 | d0 | d1) >= 0) {
      return ymd(y0 * 1000 + y1 * 100 + y2 * 10 + y3, m0 * 10 + m1, d0 * 10 + d1);
    }
  }
  return 0;
}

inline bool next_line(const char*& p, const char* end, const char*& lb, const char*& le) {
  if (p >= end) return false;
  lb = p;
  while (p < end && *p != '\n') ++p;
  le = p;
  if (p < end) ++p;
  if (le > lb && le[-1] == '\r') --le;
  return true;
}

// Fill fields[0..nfields). Returns field count (may exceed need).
inline int split_fields(const char* lb, const char* le, FieldSpan* fields, int cap) {
  const char* p = lb;
  int n = 0;
  FieldSpan span;
  while (n < cap && next_field(p, le, span)) fields[n++] = span;
  return n;
}

struct ColMap {
  int i_exp = -1, i_ask = -1, i_bid = -1, i_last = -1;
  int i_pc = -1, i_k = -1, i_u = -1, i_d = -1;
  int need = 0;
};

ColMap map_cols(const std::vector<std::string>& cols) {
  ColMap c;
  c.i_exp = col_index(cols, "ExpirationDate");
  c.i_ask = col_index(cols, "AskPrice");
  c.i_bid = col_index(cols, "BidPrice");
  c.i_last = col_index(cols, "LastPrice");
  c.i_pc = col_index(cols, "PutCall");
  c.i_k = col_index(cols, "StrikePrice");
  c.i_u = col_index(cols, "UnderlyingPrice");
  c.i_d = col_index(cols, "DataDate");
  if (c.i_exp < 0 || c.i_ask < 0 || c.i_bid < 0 || c.i_pc < 0 || c.i_k < 0 ||
      c.i_u < 0 || c.i_d < 0)
    throw std::runtime_error("missing required options columns");
  c.need = std::max({c.i_exp, c.i_ask, c.i_bid, c.i_pc, c.i_k, c.i_u, c.i_d, c.i_last});
  return c;
}

struct DaySpot {
  Date date = 0;
  double spot = 0;
};

inline bool parse_quote_fields(const FieldSpan* fields, int nfields, const ColMap& col,
                               OptionQuote& q) {
  if (nfields <= col.need) return false;
  Date data_date = parse_ymd_raw(fields[col.i_d].b, fields[col.i_d].e);
  Date exp_date = parse_ymd_raw(fields[col.i_exp].b, fields[col.i_exp].e);
  if (!data_date || !exp_date) return false;

  auto& fpc = fields[col.i_pc];
  if (fpc.empty()) return false;
  char pc = static_cast<char>(std::tolower(static_cast<unsigned char>(fpc.b[0])));
  OptType type;
  if (pc == 'c') type = OptType::Call;
  else if (pc == 'p') type = OptType::Put;
  else return false;

  double strike, ask, bid, underlying;
  if (!parse_double_raw(fields[col.i_k].b, fields[col.i_k].e, strike)) return false;
  if (!parse_double_raw(fields[col.i_ask].b, fields[col.i_ask].e, ask)) return false;
  if (!parse_double_raw(fields[col.i_bid].b, fields[col.i_bid].e, bid)) return false;
  if (!parse_double_raw(fields[col.i_u].b, fields[col.i_u].e, underlying)) return false;

  q.data_date = data_date;
  q.exp_date = exp_date;
  q.put_call = type;
  q.strike = strike;
  q.ask = ask;
  q.bid = bid;
  q.underlying = underlying;
  q.last = 0;
  if (col.i_last >= 0 && col.i_last < nfields && !fields[col.i_last].empty()) {
    double last = 0;
    if (parse_double_raw(fields[col.i_last].b, fields[col.i_last].e, last))
      q.last = last;
  }
  return true;
}

std::vector<double> split_adj_factors(const std::vector<DaySpot>& days) {
  std::vector<double> adj(days.size(), 1.0);
  if (days.size() < 2) return adj;
  constexpr double GAP = 1.7;
  double cumulative = 1.0;
  adj.back() = 1.0;
  for (int i = static_cast<int>(days.size()) - 2; i >= 0; --i) {
    double older = days[static_cast<size_t>(i)].spot;
    double newer = days[static_cast<size_t>(i + 1)].spot;
    if (older > 0 && newer > 0) {
      double gap = std::max(older, newer) / std::min(older, newer);
      if (gap >= GAP) {
        int ratio = std::max(2, static_cast<int>(std::lround(gap)));
        if (older > newer) cumulative *= (1.0 / ratio);
        else cumulative *= static_cast<double>(ratio);
      }
    }
    adj[static_cast<size_t>(i)] = cumulative;
  }
  return adj;
}

std::unordered_set<Date> keep_dates(const std::vector<DaySpot>& days, int every_n) {
  std::unordered_set<Date> keep;
  if (days.empty()) return keep;
  if (every_n <= 1) {
    keep.reserve(days.size() * 2);
    for (const auto& d : days) keep.insert(d.date);
    return keep;
  }
  keep.reserve(days.size() / static_cast<size_t>(every_n) + 4);
  keep.insert(days.front().date);
  for (size_t i = 0; i < days.size(); ++i) {
    if (i % static_cast<size_t>(every_n) == 0) keep.insert(days[i].date);
  }
  keep.insert(days.back().date);
  return keep;
}

void sort_quotes(std::vector<OptionQuote>& quotes) {
  std::sort(quotes.begin(), quotes.end(),
            [](const OptionQuote& a, const OptionQuote& b) {
              if (a.data_date != b.data_date) return a.data_date < b.data_date;
              if (a.exp_date != b.exp_date) return a.exp_date < b.exp_date;
              if (a.put_call != b.put_call) return a.put_call < b.put_call;
              return a.strike < b.strike;
            });
}

void filter_atm_band(std::vector<OptionQuote>& quotes, double atm_band) {
  if (atm_band <= 0 || quotes.empty()) return;
  std::unordered_map<Date, double> spot_map;
  for (auto& q : quotes) {
    if (!spot_map.count(q.data_date)) spot_map[q.data_date] = q.underlying;
  }
  std::vector<OptionQuote> filtered;
  filtered.reserve(quotes.size());
  for (auto& q : quotes) {
    double s = spot_map[q.data_date];
    if (q.strike >= s * (1.0 - atm_band) && q.strike <= s * (1.0 + atm_band))
      filtered.push_back(q);
  }
  quotes.swap(filtered);
}

inline void apply_adj(OptionQuote& q, double f) {
  if (std::abs(f - 1.0) < 1e-15) return;
  q.underlying *= f;
  q.strike *= f;
  q.bid *= f;
  q.ask *= f;
  q.last *= f;
}

// Options feeds put DataDate last and UnderlyingPrice just before it.
inline bool parse_tail_date_spot(const char* lb, const char* le, Date& date, double& spot) {
  if (le - lb < 12) return false;
  const char* d0 = le - 10;
  if (!(d0 > lb && d0[-1] == ',')) return false;
  date = parse_ymd_raw(d0, le);
  if (!date) return false;
  const char* u_end = d0 - 1;
  const char* u_beg = u_end;
  while (u_beg > lb && u_beg[-1] != ',') --u_beg;
  return parse_double_raw(u_beg, u_end, spot);
}

inline bool parse_tail_date(const char* lb, const char* le, Date& date) {
  if (le - lb < 12) return false;
  const char* d0 = le - 10;
  if (d0 > lb && d0[-1] == ',') {
    date = parse_ymd_raw(d0, le);
    return date != 0;
  }
  return false;
}

// Scan session spots (date-sorted file). Parses only DataDate + UnderlyingPrice.
std::vector<DaySpot> scan_day_spots_mapped(const char* data, size_t size, const ColMap& col,
                                           Date start, Date end) {
  std::vector<DaySpot> days;
  days.reserve(4096);
  const char* p = data;
  const char* endp = data + size;
  const char *lb = nullptr, *le = nullptr;
  if (!next_line(p, endp, lb, le)) return days;  // header

  const bool tail_ok = (col.i_d > col.i_u);  // typical layout: ... Underlying, DataDate
  FieldSpan fields[64];
  Date prev = 0;
  while (next_line(p, endp, lb, le)) {
    if (lb == le) continue;
    Date d = 0;
    double spot = 0;
    bool ok = false;
    if (tail_ok) ok = parse_tail_date_spot(lb, le, d, spot);
    if (!ok) {
      int n = split_fields(lb, le, fields, 64);
      if (n <= col.need) continue;
      d = parse_ymd_raw(fields[col.i_d].b, fields[col.i_d].e);
      if (!d) continue;
      if (!parse_double_raw(fields[col.i_u].b, fields[col.i_u].e, spot)) continue;
    }
    if (start && d < start) continue;
    if (end && d > end) continue;
    if (d == prev) continue;
    days.push_back({d, spot});
    prev = d;
  }
  return days;
}

OptionsDataset load_mapped(const MappedFile& file, const ColMap& col, Date start, Date end,
                           int every_n, double atm_band) {
  const char* data = file.data();
  size_t size = file.size();
  OptionsDataset ds;

  // Fast path: no session thinning — one mmap scan, then split-adjust + sort.
  if (every_n <= 1) {
    size_t est = size / 90 + 1024;
    ds.quotes.reserve(est);
    const char* p = data;
    const char* endp = data + size;
    const char *lb = nullptr, *le = nullptr;
    if (!next_line(p, endp, lb, le)) throw std::runtime_error("empty options file");
    FieldSpan fields[64];
    OptionQuote q;
    while (next_line(p, endp, lb, le)) {
      if (lb == le) continue;
      int n = split_fields(lb, le, fields, 64);
      if (!parse_quote_fields(fields, n, col, q)) continue;
      if (start && q.data_date < start) continue;
      if (end && q.data_date > end) continue;
      if (atm_band > 0) {
        double s = q.underlying;
        if (q.strike < s * (1.0 - atm_band) || q.strike > s * (1.0 + atm_band)) continue;
      }
      ds.quotes.push_back(q);
    }
    adjust_prices_for_splits_and_merges(ds.quotes);
    sort_quotes(ds.quotes);
    ds.rebuild_index();
    return ds;
  }

  // Two-pass when thinning: avoid parsing/sorting quotes we will discard.
  std::vector<DaySpot> days = scan_day_spots_mapped(data, size, col, start, end);
  auto adj = split_adj_factors(days);
  auto keep = keep_dates(days, every_n);

  std::unordered_map<Date, double> adj_map;
  adj_map.reserve(days.size() * 2);
  for (size_t i = 0; i < days.size(); ++i) adj_map[days[i].date] = adj[i];

  size_t est = size / 90;
  est = est / static_cast<size_t>(every_n) + 1024;
  ds.quotes.reserve(est);

  const char* p = data;
  const char* endp = data + size;
  const char *lb = nullptr, *le = nullptr;
  if (!next_line(p, endp, lb, le)) throw std::runtime_error("empty options file");

  FieldSpan fields[64];
  OptionQuote q;
  const bool tail_ok = (col.i_d > col.i_u);
  while (next_line(p, endp, lb, le)) {
    if (lb == le) continue;
    if (tail_ok) {
      Date d = 0;
      if (!parse_tail_date(lb, le, d)) continue;
      if (start && d < start) continue;
      if (end && d > end) continue;
      if (!keep.count(d)) continue;
    }
    int n = split_fields(lb, le, fields, 64);
    if (!parse_quote_fields(fields, n, col, q)) continue;
    if (start && q.data_date < start) continue;
    if (end && q.data_date > end) continue;
    if (!keep.count(q.data_date)) continue;
    if (atm_band > 0) {
      double s = q.underlying;
      if (q.strike < s * (1.0 - atm_band) || q.strike > s * (1.0 + atm_band)) continue;
    }
    auto it = adj_map.find(q.data_date);
    if (it != adj_map.end()) apply_adj(q, it->second);
    ds.quotes.push_back(q);
  }

  sort_quotes(ds.quotes);
  ds.rebuild_index();
  return ds;
}

OptionsDataset load_streamed(const std::string& path, const ColMap& col, Date start,
                             Date end, int every_n, double atm_band) {
  // Gzip / non-mmap path: single pass collect, then adjust/thin/sort (original flow).
  LineReader in(path);
  std::string header;
  if (!in.getline(header)) throw std::runtime_error("empty options file: " + path);

  OptionsDataset ds;
  ds.quotes.reserve(1 << 20);
  std::string line;
  FieldSpan fields[64];
  OptionQuote q;
  while (in.getline(line)) {
    if (line.empty()) continue;
    int n = split_fields(line.data(), line.data() + line.size(), fields, 64);
    if (!parse_quote_fields(fields, n, col, q)) continue;
    if (start && q.data_date < start) continue;
    if (end && q.data_date > end) continue;
    ds.quotes.push_back(q);
  }

  adjust_prices_for_splits_and_merges(ds.quotes);
  sort_quotes(ds.quotes);

  if (every_n > 1) {
    std::vector<Date> all_dates;
    all_dates.reserve(4096);
    for (auto& qq : ds.quotes) {
      if (all_dates.empty() || all_dates.back() != qq.data_date)
        all_dates.push_back(qq.data_date);
    }
    std::unordered_set<Date> keep;
    if (!all_dates.empty()) {
      keep.insert(all_dates.front());
      for (size_t i = 0; i < all_dates.size(); ++i)
        if (i % static_cast<size_t>(every_n) == 0) keep.insert(all_dates[i]);
      keep.insert(all_dates.back());
    }
    std::vector<OptionQuote> filtered;
    filtered.reserve(ds.quotes.size() / static_cast<size_t>(every_n) + 64);
    for (auto& qq : ds.quotes)
      if (keep.count(qq.data_date)) filtered.push_back(qq);
    ds.quotes.swap(filtered);
  }

  filter_atm_band(ds.quotes, atm_band);
  ds.rebuild_index();
  return ds;
}

}  // namespace

void adjust_prices_for_splits_and_merges(std::vector<OptionQuote>& quotes) {
  if (quotes.empty()) return;

  // File order is date-sorted; collect first-seen spot per session in one pass.
  std::vector<DaySpot> days;
  days.reserve(4096);
  for (const auto& q : quotes) {
    if (days.empty() || days.back().date != q.data_date)
      days.push_back({q.data_date, q.underlying});
  }
  // If input wasn't grouped by date, fall back to unique-sort path.
  bool grouped = true;
  for (size_t i = 1; i < days.size(); ++i) {
    if (days[i].date <= days[i - 1].date) { grouped = false; break; }
  }
  if (!grouped) {
    std::vector<Date> dates;
    dates.reserve(quotes.size());
    for (auto& q : quotes) dates.push_back(q.data_date);
    std::sort(dates.begin(), dates.end());
    dates.erase(std::unique(dates.begin(), dates.end()), dates.end());
    std::unordered_map<Date, double> spot;
    spot.reserve(dates.size() * 2);
    for (auto& q : quotes) {
      if (!spot.count(q.data_date)) spot[q.data_date] = q.underlying;
    }
    days.clear();
    days.reserve(dates.size());
    for (Date d : dates) days.push_back({d, spot[d]});
  }

  auto adj = split_adj_factors(days);
  std::unordered_map<Date, double> adj_map;
  adj_map.reserve(days.size() * 2);
  for (size_t i = 0; i < days.size(); ++i) adj_map[days[i].date] = adj[i];

  for (auto& q : quotes) {
    auto it = adj_map.find(q.data_date);
    if (it != adj_map.end()) apply_adj(q, it->second);
  }
}

OptionsDataset load_options_csv(const std::string& path, Date start, Date end,
                                int trade_every_n_days, double atm_band) {
  OptionsDataset ds;
  if (!ends_with_gz(path)) {
    MappedFile file(path);
    const char* data = file.data();
    size_t size = file.size();
    const char* p = data;
    const char* endp = data + size;
    const char *lb = nullptr, *le = nullptr;
    if (!next_line(p, endp, lb, le)) throw std::runtime_error("empty options file: " + path);
    ColMap col = map_cols(split_csv(std::string(lb, le)));
    ds = load_mapped(file, col, start, end, trade_every_n_days, atm_band);
  } else {
    LineReader probe(path);
    std::string header;
    if (!probe.getline(header)) throw std::runtime_error("empty options file: " + path);
    ColMap col = map_cols(split_csv(header));
    ds = load_streamed(path, col, start, end, trade_every_n_days, atm_band);
  }

  std::cerr << "Loaded options rows=" << ds.quotes.size()
            << " days=" << ds.dates.size() << " from " << path << "\n";
  return ds;
}

void OptionsDataset::rebuild_index() {
  dates.clear();
  day_range.clear();
  day_exps.clear();
  for (size_t i = 0; i < quotes.size();) {
    Date d = quotes[i].data_date;
    size_t j = i;
    while (j < quotes.size() && quotes[j].data_date == d) ++j;
    day_range[d] = {i, j};
    dates.push_back(d);

    std::vector<Date> exps;
    for (size_t k = i; k < j;) {
      Date exp = quotes[k].exp_date;
      exps.push_back(exp);
      while (k < j && quotes[k].exp_date == exp) ++k;
    }
    day_exps.emplace(d, std::move(exps));
    i = j;
  }
}

void save_dataset_cache(const OptionsDataset& ds, const std::string& path) {
  std::ofstream out(path, std::ios::binary);
  if (!out) throw std::runtime_error("cannot write cache " + path);
  const char magic[8] = {'W', 'B', 'T', 'C', 'A', 'C', 'H', '1'};
  out.write(magic, 8);
  uint64_t n = ds.quotes.size();
  out.write(reinterpret_cast<const char*>(&n), sizeof(n));
  out.write(reinterpret_cast<const char*>(ds.quotes.data()),
            static_cast<std::streamsize>(n * sizeof(OptionQuote)));
  std::cerr << "Wrote cache " << path << " rows=" << n << "\n";
}

OptionsDataset load_dataset_cache(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read cache " + path);
  char magic[8];
  in.read(magic, 8);
  if (std::string(magic, magic + 8) != "WBTCACH1")
    throw std::runtime_error("bad cache magic");
  uint64_t n = 0;
  in.read(reinterpret_cast<char*>(&n), sizeof(n));
  OptionsDataset ds;
  ds.quotes.resize(static_cast<size_t>(n));
  in.read(reinterpret_cast<char*>(ds.quotes.data()),
          static_cast<std::streamsize>(n * sizeof(OptionQuote)));
  ds.rebuild_index();
  std::cerr << "Loaded cache " << path << " rows=" << n
            << " days=" << ds.dates.size() << "\n";
  return ds;
}

const OptionQuote* OptionsDataset::begin(Date d) const {
  auto it = day_range.find(d);
  if (it == day_range.end()) return nullptr;
  return quotes.data() + it->second.first;
}
const OptionQuote* OptionsDataset::end(Date d) const {
  auto it = day_range.find(d);
  if (it == day_range.end()) return nullptr;
  return quotes.data() + it->second.second;
}
double OptionsDataset::spot(Date d) const {
  auto b = begin(d);
  if (!b) return 0;
  return b->underlying;
}
std::vector<Date> OptionsDataset::expirations(Date d) const {
  auto it = day_exps.find(d);
  if (it == day_exps.end()) return {};
  return it->second;
}

std::pair<const OptionQuote*, const OptionQuote*>
OptionsDataset::quotes_for(Date d, Date exp, OptType type) const {
  auto day_b = begin(d), day_e = end(d);
  if (!day_b) return {nullptr, nullptr};

  auto exp_lo = std::lower_bound(
      day_b, day_e, exp,
      [](const OptionQuote& q, Date e) { return q.exp_date < e; });
  auto exp_hi = std::upper_bound(
      exp_lo, day_e, exp,
      [](Date e, const OptionQuote& q) { return e < q.exp_date; });
  if (exp_lo == exp_hi) return {nullptr, nullptr};

  auto type_lo = std::lower_bound(
      exp_lo, exp_hi, type,
      [](const OptionQuote& q, OptType t) { return q.put_call < t; });
  auto type_hi = std::upper_bound(
      type_lo, exp_hi, type,
      [](OptType t, const OptionQuote& q) { return t < q.put_call; });
  if (type_lo == type_hi) return {nullptr, nullptr};
  return {type_lo, type_hi};
}

}  // namespace wbt
