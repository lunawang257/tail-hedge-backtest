#include "wbt/verbose_log.hpp"
#include "wbt/types.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace wbt {
namespace {

std::string html_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c; break;
    }
  }
  return out;
}

std::string fmt_commas(long long n) {
  bool neg = n < 0;
  unsigned long long v = neg ? static_cast<unsigned long long>(-n)
                             : static_cast<unsigned long long>(n);
  std::string digits = std::to_string(v);
  std::string out;
  int cnt = 0;
  for (int i = static_cast<int>(digits.size()) - 1; i >= 0; --i) {
    if (cnt && cnt % 3 == 0) out.push_back(',');
    out.push_back(digits[static_cast<size_t>(i)]);
    ++cnt;
  }
  std::reverse(out.begin(), out.end());
  if (neg) out.insert(out.begin(), '-');
  return out;
}

std::string fmt_usd(double x, int decimals = 0) {
  bool neg = x < 0;
  if (neg) x = -x;
  if (decimals <= 0) {
    long long n = static_cast<long long>(std::llround(x));
    return (neg ? "-$" : "$") + fmt_commas(n);
  }
  long long scale = 1;
  for (int i = 0; i < decimals; ++i) scale *= 10;
  long long n = static_cast<long long>(std::llround(x * static_cast<double>(scale)));
  long long whole = n / scale;
  long long frac = n % scale;
  std::ostringstream os;
  os << (neg ? "-$" : "$") << fmt_commas(whole) << '.'
     << std::setfill('0') << std::setw(decimals) << frac;
  return os.str();
}

std::string fmt_px(double x) {
  return fmt_usd(x, 2);
}

std::string fmt_opt_px(double x) {
  bool neg = x < 0;
  if (neg) x = -x;
  std::ostringstream os;
  os << std::fixed << std::setprecision(4) << x;
  std::string s = os.str();
  while (s.size() > 4 && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.push_back('0');
  return (neg ? "-$" : "$") + s;
}

std::string fmt_pct(double x) {
  if (std::abs(x - std::round(x)) < 0.005) {
    return std::to_string(static_cast<long long>(std::llround(x))) + "%";
  }
  std::ostringstream os;
  os << std::fixed << std::setprecision(2) << x << "%";
  return os.str();
}

std::string fmt_strike(double x) {
  std::ostringstream os;
  os << std::defaultfloat << std::setprecision(8) << x;
  return os.str();
}

std::string fmt_qty(double x) {
  if (std::abs(x - std::round(x)) < 1e-6)
    return fmt_commas(static_cast<long long>(std::llround(x)));
  std::ostringstream os;
  os << std::fixed << std::setprecision(2) << x;
  return os.str();
}

std::string fmt_date_or_dash(Date d) {
  return d ? format_date(d) : "—";
}

const char* action_kind_title(VerboseActionKind k) {
  switch (k) {
    case VerboseActionKind::OpenW: return "Open W Strategy";
    case VerboseActionKind::CloseW: return "Close W Strategy";
    case VerboseActionKind::BuyStock: return "buy";
    case VerboseActionKind::SellStock: return "sell";
    case VerboseActionKind::Tax: return "pay tax";
  }
  return "Action";
}

std::string human_reason(const std::string& r) {
  if (r == "overlay_open") return "B&H overlay";
  if (r == "overlay_buy") return "W Strategy Profit";
  if (r == "overlay_sell") return "fund W cash";
  if (r == "tax") return "year-end tax";
  if (r == "dte_roll") return "DTE roll";
  if (r == "expire") return "expire";
  if (r == "profit_take") return "profit take";
  if (r == "loss_cut") return "loss cut";
  if (r == "final") return "final";
  if (r == "open") return "open";
  return r;
}

std::string day_header_text(const VerboseDay& d) {
  std::ostringstream os;
  os << format_date(d.date)
     << " Spot: " << fmt_px(d.spot)
     << " | Reason: " << (d.print_reason.empty() ? "—" : d.print_reason)
     << " | NLV: " << fmt_usd(d.nlv)
     << " | B&H NLV: " << fmt_usd(d.bh_nlv)
     << " | MaxDD: " << fmt_pct(d.max_dd)
     << " | B&H maxDD: " << fmt_pct(d.bh_max_dd);
  return os.str();
}

std::string fmt_comm_pcnt(double commission, double nlv) {
  if (!(std::abs(nlv) > 1e-9)) return "—";
  std::ostringstream os;
  os << std::fixed << std::setprecision(4) << (100.0 * commission / nlv) << "%";
  return os.str();
}

void write_perf_stats_row(std::ostream& out, const char* name, double nlv,
                          double peak, double bh_peak, double max_dd,
                          double bh_max_dd, Date max_dd_date,
                          Date bh_max_dd_date, double max_dd_nlv,
                          double bh_max_dd_nlv) {
  out << "<tr><td>" << html_escape(name) << "</td>"
      << "<td class=\"num\">" << html_escape(fmt_usd(nlv)) << "</td>"
      << "<td class=\"num\">" << html_escape(fmt_usd(peak)) << "</td>"
      << "<td class=\"num\">" << html_escape(fmt_usd(bh_peak)) << "</td>"
      << "<td class=\"num\">" << html_escape(fmt_pct(max_dd)) << "</td>"
      << "<td class=\"num\">" << html_escape(fmt_pct(bh_max_dd)) << "</td>"
      << "<td class=\"num\">" << html_escape(fmt_date_or_dash(max_dd_date))
      << "</td>"
      << "<td class=\"num\">" << html_escape(fmt_date_or_dash(bh_max_dd_date))
      << "</td>"
      << "<td class=\"num\">" << html_escape(fmt_usd(max_dd_nlv)) << "</td>"
      << "<td class=\"num\">" << html_escape(fmt_usd(bh_max_dd_nlv))
      << "</td></tr>\n";
}

void write_comm_gain_stats_row(std::ostream& out, const char* name, double nlv,
                               double commission, double st_loss_carry,
                               double lt_loss_carry, double st_gain,
                               double lt_gain, double call_pnl,
                               double put_pnl, double short_pnl,
                               double long_pnl) {
  out << "<tr><td>" << html_escape(name) << "</td>"
      << "<td class=\"num\">" << html_escape(fmt_usd(commission)) << "</td>"
      << "<td class=\"num\">" << html_escape(fmt_comm_pcnt(commission, nlv))
      << "</td>"
      << "<td class=\"num\">" << html_escape(fmt_usd(st_loss_carry)) << "</td>"
      << "<td class=\"num\">" << html_escape(fmt_usd(lt_loss_carry)) << "</td>"
      << "<td class=\"num\">" << html_escape(fmt_usd(st_gain)) << "</td>"
      << "<td class=\"num\">" << html_escape(fmt_usd(lt_gain)) << "</td>"
      << "<td class=\"num\">" << html_escape(fmt_usd(call_pnl)) << "</td>"
      << "<td class=\"num\">" << html_escape(fmt_usd(put_pnl)) << "</td>"
      << "<td class=\"num\">" << html_escape(fmt_usd(short_pnl)) << "</td>"
      << "<td class=\"num\">" << html_escape(fmt_usd(long_pnl)) << "</td></tr>\n";
}

void write_stats_table(std::ostream& out, const VerboseDay& d) {
  out << "<h3>Stats</h3>\n"
         "<table class=\"book stats\">\n"
         "<thead><tr>"
         "<th>Strat</th>"
         "<th class=\"num\">NLV</th>"
         "<th class=\"num\">Peak</th>"
         "<th class=\"num\">B&amp;H Peak</th>"
         "<th class=\"num\">MaxDD</th>"
         "<th class=\"num\">B&amp;H MaxDD</th>"
         "<th class=\"num\">MaxDD Date</th>"
         "<th class=\"num\">B&amp;H MaxDD Date</th>"
         "<th class=\"num\">MDD NLV</th>"
         "<th class=\"num\">B&amp;H MDD NLV</th>"
         "</tr></thead>\n<tbody>\n";
  write_perf_stats_row(out, "W", d.nlv, d.peak, d.bh_peak, d.max_dd,
                       d.bh_max_dd, d.max_dd_date, d.bh_max_dd_date,
                       d.max_dd_nlv, d.bh_max_dd_nlv);
  write_perf_stats_row(out, "B&H", d.bh_nlv, d.bh_peak, d.bh_peak, d.bh_max_dd,
                       d.bh_max_dd, d.bh_max_dd_date, d.bh_max_dd_date,
                       d.bh_max_dd_nlv, d.bh_max_dd_nlv);
  out << "</tbody></table>\n"
         "<table class=\"book stats\">\n"
         "<thead><tr>"
         "<th>Strat</th>"
         "<th class=\"num\">Comm</th>"
         "<th class=\"num\">Comm%</th>"
         "<th class=\"num\">STLoss</th>"
         "<th class=\"num\">LTLoss</th>"
         "<th class=\"num\">STGain</th>"
         "<th class=\"num\">LTGain</th>"
         "<th class=\"num\">PLcall</th>"
         "<th class=\"num\">PLput</th>"
         "<th class=\"num\">PLshort</th>"
         "<th class=\"num\">PLlong</th>"
         "</tr></thead>\n<tbody>\n";
  write_comm_gain_stats_row(out, "W", d.nlv, d.commission, d.st_loss_carry,
                            d.lt_loss_carry, d.st_gain, d.lt_gain, d.call_pnl,
                            d.put_pnl, d.short_pnl, d.long_pnl);
  // Pure B&H never realizes gains, so carry / YTD gain stay at $0.
  write_comm_gain_stats_row(out, "B&H", d.bh_nlv, d.bh_commission, 0, 0, 0, 0,
                            0, 0, 0, 0);
  out << "</tbody></table>\n";
}

void write_column_legend(std::ostream& out) {
  static const struct {
    const char* abbr;
    const char* full;
    const char* zh;
  } rows[] = {
      {"Strat", "Strategy", "策略"},
      {"W", "W tail-hedge strategy (with stock overlay)",
       "W尾部对冲策略（含股票叠加）"},
      {"B&H", "Buy-and-hold of the underlying", "标的买入持有"},
      {"NLV", "Net liquidation value", "净清算价值"},
      {"B&H NLV", "Buy-and-hold net liquidation value", "买入持有净清算价值"},
      {"Comm", "Cumulative commission", "累计佣金"},
      {"Comm%", "Commission as a percent of NLV", "佣金占净清算价值的百分比"},
      {"Peak", "Peak net liquidation value", "净清算价值峰值"},
      {"B&H Peak", "Buy-and-hold peak net liquidation value",
       "买入持有净清算价值峰值"},
      {"MaxDD", "Maximum drawdown", "最大回撤"},
      {"B&H MaxDD", "Buy-and-hold maximum drawdown", "买入持有最大回撤"},
      {"MaxDD Date", "Date of the maximum-drawdown trough", "最大回撤谷底日期"},
      {"B&H MaxDD Date", "Date of the buy-and-hold maximum-drawdown trough",
       "买入持有最大回撤谷底日期"},
      {"MDD NLV", "Net liquidation value at the maximum-drawdown trough",
       "最大回撤谷底时的净清算价值"},
      {"B&H MDD NLV",
       "Buy-and-hold net liquidation value at its maximum-drawdown trough",
       "买入持有最大回撤谷底时的净清算价值"},
      {"STLoss", "Short-term capital-loss carryover", "短期资本亏损结转"},
      {"LTLoss", "Long-term capital-loss carryover", "长期资本亏损结转"},
      {"STGain", "Year-to-date short-term capital gain",
       "年初至今短期资本利得"},
      {"LTGain", "Year-to-date long-term capital gain", "年初至今长期资本利得"},
      {"PLcall", "Cumulative realized P/L from call legs",
       "看涨期权累计已实现损益"},
      {"PLput", "Cumulative realized P/L from put legs",
       "看跌期权累计已实现损益"},
      {"PLshort", "Cumulative realized P/L from short near-term option legs",
       "近期空头期权腿累计已实现损益"},
      {"PLlong", "Cumulative realized P/L from long longer-term option legs",
       "远期多头期权腿累计已实现损益"},
      {"D2Exp", "Days to expiration", "距到期天数"},
  };
  out << "<details class=\"colkey\">\n"
         "<summary>"
         "<span class=\"pm\"></span>"
         "<span class=\"lbl\">Column names</span>"
         "</summary>\n"
         "<table class=\"book colkey\">\n"
         "<thead><tr><th>Short</th><th>Full name</th>"
         "<th>Simplified Chinese</th></tr></thead>\n"
         "<tbody>\n";
  for (const auto& r : rows) {
    out << "<tr><td>" << html_escape(r.abbr) << "</td><td>"
        << html_escape(r.full) << "</td><td>" << html_escape(r.zh)
        << "</td></tr>\n";
  }
  out << "</tbody></table>\n</details>\n";
}

std::string action_title(const VerboseAction& a) {
  std::ostringstream os;
  os << "Action: " << action_kind_title(a.kind);
  if (a.kind == VerboseActionKind::OpenW) {
    os << ", Cost: " << fmt_usd(a.pnl);
  } else if (a.kind == VerboseActionKind::CloseW) {
    os << ", Reason: " << human_reason(a.reason)
       << ", P/L: " << fmt_usd(a.pnl);
  } else if (a.kind == VerboseActionKind::BuyStock ||
             a.kind == VerboseActionKind::SellStock) {
    os << " " << fmt_qty(std::abs(a.shares)) << " shares @ " << fmt_px(a.price);
    if (!a.reason.empty()) os << ", Reason: " << human_reason(a.reason);
  } else if (a.kind == VerboseActionKind::Tax) {
    os << " " << fmt_usd(a.pnl);
    if (!a.reason.empty()) os << ", Reason: " << human_reason(a.reason);
  }
  return os.str();
}

void write_pos_table(std::ostream& out, Date date,
                     const std::vector<VerbosePosRow>& rows) {
  out << "<table class=\"book\">\n"
         "<thead><tr>"
         "<th>Type</th><th class=\"num\">Contracts</th><th class=\"num\">Strike</th>"
         "<th>Expiration</th><th class=\"num\">D2Exp</th><th class=\"num\">Price</th>"
         "<th class=\"num\">Cost-Basis</th><th class=\"num\">Market Value</th>"
         "<th class=\"num\">P&amp;L</th>"
         "<th class=\"num\">Theta</th>"
         "</tr></thead>\n<tbody>\n";
  for (const auto& r : rows) {
    bool options_nlv = (r.type == "Options NLV");
    bool stocks_nlv = (r.type == "Stocks NLV");
    bool stock_like = (r.type == "Stock" || stocks_nlv);
    bool sum = options_nlv || stocks_nlv || r.type == "Cash" || r.type == "NLV";
    std::string cls;
    if (sum) cls = (r.type == "NLV") ? "sum nlv" : "sum";
    if (options_nlv) {
      if (!cls.empty()) cls += " ";
      cls += "opt-nlv";
    }
    out << "<tr class=\"" << cls << "\"><td>" << html_escape(r.type) << "</td>";
    out << "<td class=\"num\">";
    if (r.has_contracts) out << html_escape(fmt_qty(r.contracts));
    out << "</td><td class=\"num\">";
    if (r.has_strike) out << html_escape(fmt_strike(r.strike));
    else if (stock_like) out << "N/A";
    out << "</td><td>";
    if (r.has_exp) out << format_date(r.exp_date);
    else if (stock_like) out << "N/A";
    out << "</td><td class=\"num\">";
    if (r.has_exp) out << days_between(date, r.exp_date);
    else if (stock_like) out << "N/A";
    out << "</td><td class=\"num\">";
    if (r.has_price) out << html_escape(fmt_opt_px(r.price));
    out << "</td><td class=\"num\">";
    if (r.has_cost_basis) out << html_escape(fmt_usd(r.cost_basis));
    out << "</td><td class=\"num\">" << html_escape(fmt_usd(r.total))
        << "</td><td class=\"num\">";
    if (r.has_pnl) out << html_escape(fmt_usd(r.pnl));
    out << "</td><td class=\"num\">";
    if (r.has_theta) out << html_escape(fmt_usd(r.theta, 2));
    out << "</td></tr>\n";
  }
  out << "</tbody></table>\n";
}

void write_open_table(std::ostream& out, Date date,
                      const std::vector<VerboseActionLeg>& legs) {
  out << "<table class=\"book\">\n"
         "<thead><tr>"
         "<th>Type</th><th class=\"num\">Contracts</th><th class=\"num\">Strike</th>"
         "<th>Expiration</th><th class=\"num\">D2Exp</th>"
         "<th class=\"num\">Price</th><th class=\"num\">Total</th>"
         "</tr></thead>\n<tbody>\n";
  double net = 0;
  for (const auto& L : legs) {
    net += L.total;
    out << "<tr><td>" << html_escape(L.type) << "</td>"
        << "<td class=\"num\">" << html_escape(fmt_qty(L.contracts)) << "</td>"
        << "<td class=\"num\">" << html_escape(fmt_strike(L.strike)) << "</td>"
        << "<td>" << format_date(L.exp_date) << "</td>"
        << "<td class=\"num\">" << days_between(date, L.exp_date) << "</td>"
        << "<td class=\"num\">" << html_escape(fmt_opt_px(L.price)) << "</td>"
        << "<td class=\"num\">" << html_escape(fmt_usd(L.total)) << "</td></tr>\n";
  }
  out << "<tr class=\"sum\"><td>Total</td><td class=\"num\"></td>"
         "<td class=\"num\"></td><td></td><td class=\"num\"></td>"
         "<td class=\"num\"></td>"
      << "<td class=\"num\">" << html_escape(fmt_usd(net)) << "</td></tr>\n";
  out << "</tbody></table>\n";
}

void write_close_table(std::ostream& out, Date date,
                       const std::vector<VerboseActionLeg>& legs) {
  out << "<table class=\"book\">\n"
         "<thead><tr>"
         "<th>Type</th><th class=\"num\">Contracts</th><th class=\"num\">Strike</th>"
         "<th>Expiration</th><th class=\"num\">D2Exp</th>"
         "<th class=\"num\">Cost-Basis</th>"
         "<th class=\"num\">Price</th><th class=\"num\">P/L / share</th>"
         "<th class=\"num\">Total P/L</th>"
         "<th class=\"num\">P/L Call</th><th class=\"num\">P/L Put</th>"
         "</tr></thead>\n<tbody>\n";
  double total = 0, total_call = 0, total_put = 0;
  for (const auto& L : legs) {
    bool is_call = (L.type == "Call");
    total += L.total;
    (is_call ? total_call : total_put) += L.total;
    out << "<tr><td>" << html_escape(L.type) << "</td>"
        << "<td class=\"num\">" << html_escape(fmt_qty(L.contracts)) << "</td>"
        << "<td class=\"num\">" << html_escape(fmt_strike(L.strike)) << "</td>"
        << "<td>" << format_date(L.exp_date) << "</td>"
        << "<td class=\"num\">" << days_between(date, L.exp_date) << "</td>"
        << "<td class=\"num\">" << html_escape(fmt_opt_px(L.cost_basis)) << "</td>"
        << "<td class=\"num\">" << html_escape(fmt_opt_px(L.price)) << "</td>"
        << "<td class=\"num\">" << html_escape(fmt_opt_px(L.pnl_per)) << "</td>"
        << "<td class=\"num\">" << html_escape(fmt_usd(L.total)) << "</td>"
        << "<td class=\"num\">"
        << (is_call ? html_escape(fmt_usd(L.total)) : "") << "</td>"
        << "<td class=\"num\">"
        << (is_call ? "" : html_escape(fmt_usd(L.total))) << "</td></tr>\n";
  }
  out << "<tr class=\"sum\"><td>Total</td><td class=\"num\"></td>"
         "<td class=\"num\"></td><td></td><td class=\"num\"></td>"
         "<td class=\"num\"></td><td class=\"num\"></td><td class=\"num\"></td>"
      << "<td class=\"num\">" << html_escape(fmt_usd(total)) << "</td>"
      << "<td class=\"num\">" << html_escape(fmt_usd(total_call)) << "</td>"
      << "<td class=\"num\">" << html_escape(fmt_usd(total_put))
      << "</td></tr>\n";
  out << "</tbody></table>\n";
}

const char* kCss = R"CSS(
:root { --bg:#f6f7f9; --card:#fff; --line:#d8dee6; --ink:#1b2430; --muted:#5c6b7a; --acc:#2b6cb0; }
* { box-sizing: border-box; }
body { margin:0; font-family: ui-sans-serif, system-ui, -apple-system, Segoe UI, sans-serif;
       color:var(--ink); background:var(--bg); line-height:1.45; }
.wrap { max-width: 1280px; margin: 0 auto; padding: 16px 20px 48px; }
.toolbar { position: sticky; top: 0; z-index: 5; display:flex; gap:8px; align-items:center;
           flex-wrap:wrap; background:var(--bg); padding:10px 0 12px; border-bottom:1px solid var(--line); }
.toolbar h1 { font-size: 1.05rem; margin:0 12px 0 0; font-weight:650; }
button { font: inherit; cursor:pointer; background:#fff; border:1px solid var(--line);
         border-radius:6px; padding:4px 10px; }
button:hover { border-color: var(--acc); color: var(--acc); }
.meta { color:var(--muted); font-size:0.9rem; margin:12px 0 16px; }
.meta div { margin: 2px 0; }
details.cmdbox, details.colkey { background:var(--card); border:1px solid var(--line);
  border-radius:8px; padding: 2px 8px 2px 4px; margin: 0 0 12px; }
details.cmdbox > summary, details.colkey > summary { align-items: center; width: 100%; }
details.cmdbox > summary .copy { margin-left: auto; flex: 0 0 auto; }
table.book.colkey { min-width: 0; margin: 4px 8px 12px 28px; }
table.book.colkey th { text-transform: none; }
pre.cmd { font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
          font-size: 0.82rem; color:var(--ink); background:#fff;
          border:1px solid var(--line); border-radius:6px; padding:10px 12px;
          overflow-x: auto; white-space: pre-wrap; word-break: break-word;
          margin: 4px 8px 12px 28px; }
details { margin: 4px 0; }
details.year, details.day { background:var(--card); border:1px solid var(--line);
  border-radius:8px; padding: 2px 8px 2px 4px; }
details.day { margin: 6px 0 6px 12px; }
summary { cursor:pointer; list-style:none; display:flex; align-items:flex-start; gap:8px;
          padding: 6px 4px; user-select:none; }
summary::-webkit-details-marker, summary::marker { display:none; }
.pm { flex:0 0 auto; font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
      font-weight:700; color:var(--acc); min-width: 2.2em; display:inline-block;
      border:1px solid var(--line); border-radius:4px; text-align:center;
      background:#fff; line-height:1.4; }
.pm::before { content: "[+]"; }
details[open] > summary > .pm::before { content: "[−]"; }
.lbl { font-weight: 650; }
.year summary .end, .day summary .lbl { font-weight: 500; font-size: 0.92rem;
  font-family: ui-monospace, SFMono-Regular, Menlo, monospace; white-space: pre-wrap; }
.year summary .end { flex: 1; min-width: 0; }
.body { padding: 4px 8px 12px 28px; overflow-x: auto; }
h3 { font-size: 0.95rem; margin: 14px 0 6px; }
table.book { border-collapse: collapse; width: auto; min-width: 640px; font-size: 0.88rem;
             font-family: ui-monospace, SFMono-Regular, Menlo, monospace; margin: 0 0 10px; }
table.book.stats { min-width: 0; }
table.book.stats th { white-space: nowrap; text-transform: none; }
table.book th, table.book td { border-bottom: 1px solid var(--line); padding: 3px 10px; text-align: left; }
table.book th { font-size: 0.75rem; text-transform: uppercase; letter-spacing: 0.03em; color:var(--muted); }
table.book .num { text-align: right; font-variant-numeric: tabular-nums; }
table.book tr.sum td { border-top: 1px solid var(--ink); font-weight: 650; }
table.book tr.opt-nlv td { border-bottom: 2px solid var(--ink); }
table.book tr.nlv td { border-bottom: 2px solid var(--ink); }
.action { margin-top: 10px; font-weight: 650; }
)CSS";

const char* kJs = R"JS(
function setOpen(sel, open) {
  document.querySelectorAll(sel).forEach(function(d) { d.open = open; });
}
function foldAll() { setOpen('details', false); }
function unfoldAll() { setOpen('details.year, details.day', true); }
function copyReproduce(ev) {
  ev.preventDefault();
  ev.stopPropagation();
  var pre = document.getElementById('reproduce-cmd');
  var btn = ev.currentTarget;
  var text = pre ? pre.textContent.replace(/\\\s*\n\s*/g, ' ').trim() : '';
  function done(ok) {
    btn.textContent = ok ? 'Copied' : 'Copy failed';
    setTimeout(function() { btn.textContent = 'Copy'; }, 1500);
  }
  function fallback() {
    var ta = document.createElement('textarea');
    ta.value = text;
    ta.setAttribute('readonly', '');
    ta.style.position = 'fixed';
    ta.style.left = '-9999px';
    document.body.appendChild(ta);
    ta.select();
    try { done(document.execCommand('copy')); } catch (e) { done(false); }
    document.body.removeChild(ta);
  }
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(text).then(function() { done(true); }).catch(fallback);
  } else {
    fallback();
  }
}
)JS";

std::string wing_label(WingMode m) {
  switch (m) {
    case WingMode::Fixed: return "fixed";
    case WingMode::Credit: return "credit";
    case WingMode::Theta: return "theta";
  }
  return "?";
}

std::string sh_quote(const std::string& s) {
  bool need = s.empty();
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '.' || c == '_' || c == '-' || c == '/' ||
        c == '+' || c == '=' || c == ',' || c == ':' || c == '@')
      continue;
    need = true;
    break;
  }
  if (!need) return s;
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  out += "'";
  return out;
}

std::string fmt_num(double x) {
  std::ostringstream os;
  os << std::defaultfloat << std::setprecision(10) << x;
  return os.str();
}

// Path relative to the process cwd, using / separators. Falls back to `p`
// if it cannot be expressed relative to pwd (e.g. different volume).
std::string relpath_from_cwd(const std::string& p) {
  if (p.empty()) return p;
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path abs = fs::absolute(fs::path(p), ec);
  if (ec) return p;
  fs::path cwd = fs::current_path(ec);
  if (ec) return p;
  fs::path rel = fs::relative(abs, cwd, ec);
  if (ec || rel.empty()) return p;
  std::string s = rel.generic_string();
  if (s.empty() || s == ".") return p;
  return s;
}

std::string wing_cli(const WParam& param) {
  if (param.wing_mode == WingMode::Credit) return "credit";
  if (param.wing_mode == WingMode::Theta) return "theta";
  return "fixed:" + fmt_num(param.m_call);
}

}  // namespace

std::string reproduce_command(const std::string& symbol, const WParam& param,
                              const VerboseRunInfo& run, bool compact) {
  const std::string bin =
      run.argv0.empty() ? (run.search ? "w_search" : "w_backtest") : run.argv0;
  std::ostringstream os;
  const char* sep = compact ? " " : " \\\n  ";
  auto flag = [&](const std::string& f) { os << sep << f; };
  auto kv = [&](const std::string& f, const std::string& v) {
    os << sep << f << " " << v;
  };

  auto path = [&](const std::string& p) { return sh_quote(relpath_from_cwd(p)); };

  os << path(bin);
  if (run.search) {
    kv("--symbol", sh_quote(symbol));
    if (!run.input.empty()) kv("-i", path(run.input));
    if (!run.yield.empty()) kv("-y", path(run.yield));
    if (!run.cache.empty()) kv("--cache", path(run.cache));
    if (!run.iv_smile.empty()) kv("--iv-smile", path(run.iv_smile));
    if (!run.merge_split.empty()) kv("--merge-split", path(run.merge_split));
    if (run.start) kv("--start-date", format_date(run.start));
    if (run.end) kv("--end-date", format_date(run.end));
    kv("--every-n", std::to_string(run.every_n));
    kv("--atm-band", fmt_num(run.atm_band));
    kv("--units", std::to_string(param.units));
    if (param.long_pos_mil > 0) {
      kv("--long-pos-mil", fmt_num(param.long_pos_mil));
      kv("--hedge-ratio", fmt_num(param.hedge_ratio));
    }
    kv("--call-put-ratio-pct", fmt_num(param.call_put_ratio_pct));
    kv("--commission", fmt_num(param.fixed.opt_comm_per_contract));
    kv("--slippage-pct", fmt_num(param.fixed.opt_slippage_percent * 100.0));
    kv("--lt-tax-pct", fmt_num(param.fixed.long_term_tax * 100.0));
    kv("--st-tax-pct", fmt_num(param.fixed.short_term_tax * 100.0));
    kv("--short-dtes", std::to_string(param.short_dte));
    kv("--long-dtes", std::to_string(param.long_dte));
    kv("--short-deltas", fmt_num(param.short_delta));
    kv("--long-deltas", fmt_num(param.long_delta));
    kv("--wings", wing_cli(param));
    kv("--roll-dtes", std::to_string(param.roll_dte));
    kv("--profit-takes",
       param.enable_profit_take ? fmt_num(param.profit_take_pct) : "off");
    kv("--loss-cuts",
       param.enable_loss_cut ? fmt_num(param.loss_cut_pct) : "off");
    if (!run.html_log.empty())
      kv("--html-log", path(run.html_log));
    return os.str();
  }

  if (!run.input.empty()) kv("-i", path(run.input));
  if (!run.yield.empty()) kv("-y", path(run.yield));
  if (!run.cache.empty()) kv("--cache", path(run.cache));
  if (!run.iv_smile.empty()) kv("--iv-smile", path(run.iv_smile));
  if (!run.merge_split.empty()) kv("--merge-split", path(run.merge_split));
  if (run.start) kv("--start-date", format_date(run.start));
  if (run.end) kv("--end-date", format_date(run.end));
  if (run.every_n != 1)
    kv("--trade-every-n-days", std::to_string(run.every_n));
  if (run.atm_band > 0) kv("--atm-band", fmt_num(run.atm_band));
  kv("--short-dte", std::to_string(param.short_dte));
  kv("--long-dte", std::to_string(param.long_dte));
  kv("--short-delta", fmt_num(param.short_delta));
  kv("--long-delta", fmt_num(param.long_delta));
  kv("--units", std::to_string(param.units));
  kv("--m-call", fmt_num(param.m_call));
  kv("--m-put", fmt_num(param.m_put));
  if (param.long_pos_mil > 0) {
    kv("--long-pos-mil", fmt_num(param.long_pos_mil));
    kv("--hedge-ratio", fmt_num(param.hedge_ratio));
  }
  kv("--call-put-ratio-pct", fmt_num(param.call_put_ratio_pct));
  kv("--commission", fmt_num(param.fixed.opt_comm_per_contract));
  kv("--slippage-pct", fmt_num(param.fixed.opt_slippage_percent * 100.0));
  kv("--lt-tax-pct", fmt_num(param.fixed.long_term_tax * 100.0));
  kv("--st-tax-pct", fmt_num(param.fixed.short_term_tax * 100.0));
  kv("--roll-dte", std::to_string(param.roll_dte));
  flag(param.enable_dte_roll ? "--dte-roll" : "--no-dte-roll");
  flag(param.roll_on_expiry ? "--roll-on-expiry" : "--no-roll-on-expiry");
  if (param.enable_profit_take)
    kv("--profit-take-pct", fmt_num(param.profit_take_pct));
  else
    flag("--no-profit-take");
  if (param.enable_loss_cut)
    kv("--loss-cut-pct", fmt_num(param.loss_cut_pct));
  else
    flag("--no-loss-cut");
  flag(param.enable_gamma_scalp ? "--gamma-scalp" : "--no-gamma-scalp");
  if (!run.html_log.empty())
    kv("--html-log", path(run.html_log));
  return os.str();
}

bool write_verbose_html(const std::string& path, const std::string& symbol,
                        const WParam& param, const BacktestResult& result,
                        const VerboseRunInfo& run) {
  std::ofstream out(path);
  if (!out) return false;

  out << "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
      << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
      << "<title>Verbose log — " << html_escape(symbol) << "</title>\n"
      << "<style>" << kCss << "</style>\n"
      << "<script>" << kJs << "</script>\n"
      << "</head><body>\n<div class=\"wrap\">\n"
      << "<div class=\"toolbar\">\n"
      << "<h1>Verbose trade log — " << html_escape(symbol) << "</h1>\n"
      << "<button type=\"button\" onclick=\"foldAll()\">Fold all</button>\n"
      << "<button type=\"button\" onclick=\"unfoldAll()\">Unfold all</button>\n"
      << "</div>\n<div class=\"meta\">\n";

  write_column_legend(out);

  out << "<details class=\"cmdbox\">\n"
         "<summary>"
         "<span class=\"pm\"></span>"
         "<span class=\"lbl\">Reproduce command</span>"
         "<button type=\"button\" class=\"copy\" onclick=\"copyReproduce(event)\">Copy</button>"
         "</summary>\n"
         "<pre class=\"cmd\" id=\"reproduce-cmd\">"
      << html_escape(reproduce_command(symbol, param, run))
      << "</pre>\n</details>\n";

  out << "<div>shortDTE=" << param.short_dte
      << " longDTE=" << param.long_dte
      << " shortΔ=" << param.short_delta
      << " longΔ=" << param.long_delta
      << " wing=" << wing_label(param.wing_mode)
      << " mCall=" << param.m_call
      << " mPut=" << param.m_put
      << " units=" << param.units
      << " c/p=" << param.call_put_ratio_pct << "%</div>\n";
  if (param.long_pos_mil > 0) {
    out << "<div>longPosMil=" << param.long_pos_mil
        << " hedgeRatio=" << param.hedge_ratio << "</div>\n";
  }
  out << "<div>annualReturn=" << fmt_pct(result.annual_return)
      << " maxDrawdown=" << fmt_pct(result.max_drawdown)
      << " finalNLV=" << fmt_usd(result.final_nlv)
      << " rolls=" << result.num_rolls
      << " tax=" << fmt_usd(result.total_tax) << "</div>\n";
  if (param.long_pos_mil > 0) {
    out << "<div>B&amp;H annual=" << fmt_pct(result.baseline_annual_return)
        << " maxDD=" << fmt_pct(result.baseline_max_drawdown)
        << " finalNLV=" << fmt_usd(result.baseline_final_nlv)
        << "</div>\n";
  }
  out << "</div>\n";

  int open_year = 0;
  auto close_year = [&]() {
    if (open_year) {
      out << "</details>\n";
      open_year = 0;
    }
  };

  const auto& days = result.verbose_log;
  for (size_t i = 0; i < days.size(); ++i) {
    const auto& day = days[i];
    int y = year_of(day.date);
    if (y != open_year) {
      close_year();
      size_t last_i = i;
      while (last_i + 1 < days.size() &&
             year_of(days[last_i + 1].date) == y)
        ++last_i;
      out << "<details class=\"year\">\n<summary><span class=\"pm\"></span>"
          << "<span class=\"lbl\">" << y << "</span>"
          << "<span class=\"end\">"
          << html_escape(day_header_text(days[last_i]))
          << "</span></summary>\n";
      open_year = y;
    }
    out << "<details class=\"day\">\n<summary><span class=\"pm\"></span>"
        << "<span class=\"lbl\">" << html_escape(day_header_text(day))
        << "</span></summary>\n<div class=\"body\">\n";
    write_stats_table(out, day);
    for (const auto& a : day.actions) {
      out << "<div class=\"action\">" << html_escape(action_title(a)) << "</div>\n";
      if (a.kind == VerboseActionKind::OpenW && !a.legs.empty())
        write_open_table(out, day.date, a.legs);
      else if (a.kind == VerboseActionKind::CloseW && !a.legs.empty())
        write_close_table(out, day.date, a.legs);
    }
    out << "<h3>Position Table</h3>\n";
    write_pos_table(out, day.date, day.positions);
    out << "</div></details>\n";
  }
  close_year();

  if (result.verbose_log.empty()) {
    out << "<p>No buy/sell actions were recorded.</p>\n";
  }

  out << "</div></body></html>\n";
  return static_cast<bool>(out);
}

}  // namespace wbt
