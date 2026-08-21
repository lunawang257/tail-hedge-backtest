#include "wbt/verbose_log.hpp"
#include "wbt/types.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
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
     << " | Commission: " << fmt_usd(d.commission)
     << " | Peak: " << fmt_usd(d.peak)
     << " | MaxDD: " << fmt_pct(d.max_dd)
     << " | MaxDDDate: " << fmt_date_or_dash(d.max_dd_date)
     << " | StLossCarry: " << fmt_usd(d.st_loss_carry)
     << " | LtLossCarry: " << fmt_usd(d.lt_loss_carry)
     << " | StGain: " << fmt_usd(d.st_gain)
     << " | LtGain: " << fmt_usd(d.lt_gain)
     << " | B&H NLV: " << fmt_usd(d.bh_nlv)
     << " | B&H Peak: " << fmt_usd(d.bh_peak)
     << " | B&H maxDD: " << fmt_pct(d.bh_max_dd)
     << " | B&H maxDD Date: " << fmt_date_or_dash(d.bh_max_dd_date);
  return os.str();
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

void write_pos_table(std::ostream& out, const std::vector<VerbosePosRow>& rows) {
  out << "<table class=\"book\">\n"
         "<thead><tr>"
         "<th>Type</th><th class=\"num\">Contracts</th><th class=\"num\">Strike</th>"
         "<th>Expiration</th><th class=\"num\">Price</th>"
         "<th class=\"num\">Cost-Basis</th><th class=\"num\">Total</th>"
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
    if (r.has_price) out << html_escape(fmt_opt_px(r.price));
    out << "</td><td class=\"num\">";
    if (r.has_cost_basis) out << html_escape(fmt_px(r.cost_basis));
    out << "</td><td class=\"num\">" << html_escape(fmt_usd(r.total))
        << "</td></tr>\n";
  }
  out << "</tbody></table>\n";
}

void write_open_table(std::ostream& out, const std::vector<VerboseActionLeg>& legs) {
  out << "<table class=\"book\">\n"
         "<thead><tr>"
         "<th>Type</th><th class=\"num\">Contracts</th><th class=\"num\">Strike</th>"
         "<th>Expiration</th><th class=\"num\">Price</th><th class=\"num\">Total</th>"
         "</tr></thead>\n<tbody>\n";
  double net = 0;
  for (const auto& L : legs) {
    net += L.total;
    out << "<tr><td>" << html_escape(L.type) << "</td>"
        << "<td class=\"num\">" << html_escape(fmt_qty(L.contracts)) << "</td>"
        << "<td class=\"num\">" << html_escape(fmt_strike(L.strike)) << "</td>"
        << "<td>" << format_date(L.exp_date) << "</td>"
        << "<td class=\"num\">" << html_escape(fmt_opt_px(L.price)) << "</td>"
        << "<td class=\"num\">" << html_escape(fmt_usd(L.total)) << "</td></tr>\n";
  }
  out << "<tr class=\"sum\"><td>Total</td><td class=\"num\"></td>"
         "<td class=\"num\"></td><td></td><td class=\"num\"></td>"
      << "<td class=\"num\">" << html_escape(fmt_usd(net)) << "</td></tr>\n";
  out << "</tbody></table>\n";
}

void write_close_table(std::ostream& out, const std::vector<VerboseActionLeg>& legs) {
  out << "<table class=\"book\">\n"
         "<thead><tr>"
         "<th>Type</th><th class=\"num\">Contracts</th><th class=\"num\">Strike</th>"
         "<th>Expiration</th><th class=\"num\">Cost-Basis</th>"
         "<th class=\"num\">Price</th><th class=\"num\">P/L / share</th>"
         "<th class=\"num\">Total P/L</th>"
         "</tr></thead>\n<tbody>\n";
  for (const auto& L : legs) {
    out << "<tr><td>" << html_escape(L.type) << "</td>"
        << "<td class=\"num\">" << html_escape(fmt_qty(L.contracts)) << "</td>"
        << "<td class=\"num\">" << html_escape(fmt_strike(L.strike)) << "</td>"
        << "<td>" << format_date(L.exp_date) << "</td>"
        << "<td class=\"num\">" << html_escape(fmt_opt_px(L.cost_basis)) << "</td>"
        << "<td class=\"num\">" << html_escape(fmt_opt_px(L.price)) << "</td>"
        << "<td class=\"num\">" << html_escape(fmt_opt_px(L.pnl_per)) << "</td>"
        << "<td class=\"num\">" << html_escape(fmt_usd(L.total)) << "</td></tr>\n";
  }
  out << "</tbody></table>\n";
}

const char* kCss = R"CSS(
:root { --bg:#f6f7f9; --card:#fff; --line:#d8dee6; --ink:#1b2430; --muted:#5c6b7a; --acc:#2b6cb0; }
* { box-sizing: border-box; }
body { margin:0; font-family: ui-sans-serif, system-ui, -apple-system, Segoe UI, sans-serif;
       color:var(--ink); background:var(--bg); line-height:1.45; }
.wrap { max-width: 1200px; margin: 0 auto; padding: 16px 20px 48px; }
.toolbar { position: sticky; top: 0; z-index: 5; display:flex; gap:8px; align-items:center;
           flex-wrap:wrap; background:var(--bg); padding:10px 0 12px; border-bottom:1px solid var(--line); }
.toolbar h1 { font-size: 1.05rem; margin:0 12px 0 0; font-weight:650; }
button { font: inherit; cursor:pointer; background:#fff; border:1px solid var(--line);
         border-radius:6px; padding:4px 10px; }
button:hover { border-color: var(--acc); color: var(--acc); }
.meta { color:var(--muted); font-size:0.9rem; margin:12px 0 16px; }
.meta div { margin: 2px 0; }
details.cmdbox { background:var(--card); border:1px solid var(--line);
  border-radius:8px; padding: 2px 8px 2px 4px; margin: 0 0 12px; }
details.cmdbox > summary { align-items: center; width: 100%; }
details.cmdbox > summary .copy { margin-left: auto; flex: 0 0 auto; }
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
.day summary .lbl { font-weight: 500; font-size: 0.92rem; font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
                    white-space: pre-wrap; }
.body { padding: 4px 8px 12px 28px; }
h3 { font-size: 0.95rem; margin: 14px 0 6px; }
table.book { border-collapse: collapse; width: auto; min-width: 640px; font-size: 0.88rem;
             font-family: ui-monospace, SFMono-Regular, Menlo, monospace; margin: 0 0 10px; }
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
  var text = pre ? pre.textContent : '';
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

std::string wing_cli(const WParam& param) {
  if (param.wing_mode == WingMode::Credit) return "credit";
  if (param.wing_mode == WingMode::Theta) return "theta";
  return "fixed:" + fmt_num(param.m_call);
}

std::string reproduce_command(const std::string& symbol, const WParam& param,
                              const VerboseRunInfo& run) {
  const std::string bin =
      run.argv0.empty() ? (run.search ? "w_search" : "w_backtest") : run.argv0;
  std::ostringstream os;
  auto flag = [&](const std::string& f) { os << " \\\n  " << f; };
  auto kv = [&](const std::string& f, const std::string& v) {
    os << " \\\n  " << f << " " << v;
  };

  os << sh_quote(bin);
  if (run.search) {
    kv("--symbol", sh_quote(symbol));
    if (!run.input.empty()) kv("-i", sh_quote(run.input));
    if (!run.yield.empty()) kv("-y", sh_quote(run.yield));
    if (!run.cache.empty()) kv("--cache", sh_quote(run.cache));
    if (!run.iv_smile.empty()) kv("--iv-smile", sh_quote(run.iv_smile));
    if (!run.merge_split.empty()) kv("--merge-split", sh_quote(run.merge_split));
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
    if (!run.verbose_log.empty())
      kv("--verbose-log", sh_quote(run.verbose_log));
    return os.str();
  }

  if (!run.input.empty()) kv("-i", sh_quote(run.input));
  if (!run.yield.empty()) kv("-y", sh_quote(run.yield));
  if (!run.cache.empty()) kv("--cache", sh_quote(run.cache));
  if (!run.iv_smile.empty()) kv("--iv-smile", sh_quote(run.iv_smile));
  if (!run.merge_split.empty()) kv("--merge-split", sh_quote(run.merge_split));
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
  if (!run.verbose_log.empty())
    kv("--verbose-log", sh_quote(run.verbose_log));
  return os.str();
}

}  // namespace

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

  for (const auto& day : result.verbose_log) {
    int y = year_of(day.date);
    if (y != open_year) {
      close_year();
      out << "<details class=\"year\">\n<summary><span class=\"pm\"></span>"
          << "<span class=\"lbl\">" << y << "</span></summary>\n";
      open_year = y;
    }
    out << "<details class=\"day\">\n<summary><span class=\"pm\"></span>"
        << "<span class=\"lbl\">" << html_escape(day_header_text(day))
        << "</span></summary>\n<div class=\"body\">\n";
    for (const auto& a : day.actions) {
      out << "<div class=\"action\">" << html_escape(action_title(a)) << "</div>\n";
      if (a.kind == VerboseActionKind::OpenW && !a.legs.empty())
        write_open_table(out, a.legs);
      else if (a.kind == VerboseActionKind::CloseW && !a.legs.empty())
        write_close_table(out, a.legs);
    }
    out << "<h3>Position Table</h3>\n";
    write_pos_table(out, day.positions);
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
