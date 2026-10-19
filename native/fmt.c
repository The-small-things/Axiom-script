// fmt.c — f-string format specs: `{x:.2f}`, `{n:>5}`, `{n:05d}`, `{x:,.2f}`, `{p:.1%}`, `{n:x}`.
//
// Grammar (Python's mini-language without the space sign):
//   [[fill]align][sign][0][width][,][.precision][type]
//   align < > ^ =    sign + -    type f e % d x X o b s
//
// Numbers round exactly as JavaScript's toFixed/toExponential do — to the nearest decimal of the
// double's exact value, ties away from zero — which printf does not (it rounds ties to even and
// uses the current rounding mode). So the digits come from the exact decimal expansion and are
// rounded here. Widths count characters (code points), not bytes.

#include "axiom.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

typedef struct {
  char fill[8];      // one UTF-8 character, or "" for the default
  char align;        // 0, '<', '>', '^', '='
  char sign;         // 0, '+', '-'
  bool zero, comma;
  int width;         // 0 = none
  int prec;          // -1 = none
  char type;         // 0 or one of f e % d x X o b s
} Spec;

static int utf8_len(const char *s) {
  unsigned char c = (unsigned char)*s;
  if (c < 0x80) return 1;
  if ((c >> 5) == 6) return 2;
  if ((c >> 4) == 14) return 3;
  if ((c >> 3) == 30) return 4;
  return 1;
}

static bool is_align(char c) { return c == '<' || c == '>' || c == '^' || c == '='; }

// Parse a spec; false when it is not one (then the ':' was not a format separator).
bool ax_parse_spec(const char *s, Spec *out) {
  memset(out, 0, sizeof *out);
  out->prec = -1;
  if (!*s) return false;
  int fl = utf8_len(s);
  if (s[0] != '{' && s[0] != '}' && s[fl] && is_align(s[fl]) && strlen(s) > (size_t)fl) {
    memcpy(out->fill, s, fl);
    out->fill[fl] = '\0';
    out->align = s[fl];
    s += fl + 1;
  } else if (is_align(s[0])) {
    out->align = s[0];
    s++;
  }
  if (*s == '+' || *s == '-') out->sign = *s++;
  if (*s == '0') { out->zero = true; s++; }
  if (*s >= '0' && *s <= '9') {
    long w = 0;
    while (*s >= '0' && *s <= '9') { w = w * 10 + (*s - '0'); if (w > 100000) w = 100000; s++; }
    out->width = (int)w;
  }
  if (*s == ',') { out->comma = true; s++; }
  if (*s == '.') {
    s++;
    if (!(*s >= '0' && *s <= '9')) return false;
    long p = 0;
    while (*s >= '0' && *s <= '9') { p = p * 10 + (*s - '0'); if (p > 1000) p = 1000; s++; }
    out->prec = (int)p;
  }
  if (*s && strchr("fedxXobs%", *s)) out->type = *s++;
  return *s == '\0';
}

bool ax_is_format_spec(const char *s) { Spec sp; return ax_parse_spec(s, &sp); }

// Round the decimal digit string `d` (digits only) half-up at position `keep` (digits kept).
// Returns true when rounding carried out of the leading digit (the string grew by one).
static bool round_digits(char *d, int keep) {
  int n = (int)strlen(d);
  if (keep >= n) return false;
  bool up = d[keep] >= '5';
  d[keep] = '\0';
  if (!up) return false;
  for (int i = keep - 1; i >= 0; i--) {
    if (d[i] == '9') { d[i] = '0'; continue; }
    d[i]++;
    return false;
  }
  memmove(d + 1, d, strlen(d) + 1);
  d[0] = '1';
  return true;
}

// Number#toFixed(p) of a non-negative finite a.
static void to_fixed(double a, int p, char *out, size_t outn) {
  if (a >= 1e21) { ax_fmt_num(a, out, outn); return; }
  static char big[1600];
  snprintf(big, sizeof big, "%.1100f", a);             // the exact expansion (glibc prints it)
  char *dot = strchr(big, '.');
  int ilen = dot ? (int)(dot - big) : (int)strlen(big);
  // digits = integer part + fractional part, then round at ilen + p.
  static char digits[1600];
  int k = 0;
  for (int i = 0; i < ilen; i++) digits[k++] = big[i];
  if (dot) for (char *q = dot + 1; *q; q++) digits[k++] = *q;
  digits[k] = '\0';
  bool grew = round_digits(digits, ilen + p);
  int il = ilen + (grew ? 1 : 0);
  // Strip leading zeros of the integer part beyond one.
  int start = 0;
  while (start < il - 1 && digits[start] == '0') start++;
  size_t o = 0;
  for (int i = start; i < il && o + 1 < outn; i++) out[o++] = digits[i];
  if (p > 0 && o + 1 < outn) {
    out[o++] = '.';
    for (int i = il; i < il + p && o + 1 < outn; i++) out[o++] = digits[i];
  }
  out[o] = '\0';
}

// Number#toExponential(p) of a non-negative finite a.
static void to_exponential(double a, int p, char *out, size_t outn) {
  static char big[1200];
  static char digits[1200];
  int e;
  if (a == 0) {
    e = 0;
    memset(digits, '0', p + 1);
    digits[p + 1] = '\0';
  } else {
    snprintf(big, sizeof big, "%.800e", a);             // exact significant digits
    char *ep = strchr(big, 'e');
    e = atoi(ep + 1);
    int k = 0;
    for (char *q = big; q < ep; q++) if (*q >= '0' && *q <= '9') digits[k++] = *q;
    digits[k] = '\0';
    if (round_digits(digits, p + 1)) { e++; digits[p + 1] = '\0'; }
  }
  size_t o = 0;
  out[o++] = digits[0];
  if (p > 0) { out[o++] = '.'; for (int i = 1; i <= p && o + 1 < outn; i++) out[o++] = digits[i]; }
  snprintf(out + o, outn - o, "e%c%d", e < 0 ? '-' : '+', e < 0 ? -e : e);
}

// Math.trunc(a).toString(base) for a non-negative finite a (exact: division by a power of two).
static void to_base(double a, int base, bool upper, char *out, size_t outn) {
  double t = trunc(a);
  char tmp[1100];
  int k = 0;
  if (t == 0) tmp[k++] = '0';
  while (t >= 1 && k < (int)sizeof tmp - 1) {
    double q = floor(t / base);
    int dgt = (int)(t - q * base);
    tmp[k++] = (char)(dgt < 10 ? '0' + dgt : (upper ? 'A' : 'a') + dgt - 10);
    t = q;
  }
  size_t o = 0;
  while (k > 0 && o + 1 < outn) out[o++] = tmp[--k];
  out[o] = '\0';
}

static void group_thousands(const char *in, char *out, size_t outn) {
  int n = 0;
  while (in[n] >= '0' && in[n] <= '9') n++;
  size_t o = 0;
  for (int i = 0; i < n && o + 2 < outn; i++) {
    if (i > 0 && (n - i) % 3 == 0) out[o++] = ',';
    out[o++] = in[i];
  }
  for (const char *r = in + n; *r && o + 1 < outn; r++) out[o++] = *r;
  out[o] = '\0';
}

static int cp_count(const char *s) {
  int n = 0;
  for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) n++;
  return n;
}

AxStr *ax_format_spec(AxValue v, const char *spec_text) {
  Spec sp;
  if (!ax_parse_spec(spec_text, &sp)) return ax_to_str(v);
  if (v.t == AX_TIMER) v = ax_num(((AxTimer *)v.o)->remaining);
  if (sp.prec > 100) sp.prec = 100;
  char sign[2] = "";
  char *body = NULL;
  bool numeric = false;
  if (v.t == AX_NUM && sp.type != 's') {
    numeric = true;
    double x = sp.type == '%' ? v.num * 100 : v.num;
    double a = fabs(x);
    if (x < 0) sign[0] = '-';
    else if (sp.sign == '+') sign[0] = '+';
    char num[1700];
    if (isnan(a)) snprintf(num, sizeof num, "NaN");
    else if (isinf(a)) snprintf(num, sizeof num, "Infinity");
    else if (sp.type == 'f' || sp.type == '%') to_fixed(a, sp.prec < 0 ? 6 : sp.prec, num, sizeof num);
    else if (sp.type == 'e') to_exponential(a, sp.prec < 0 ? 6 : sp.prec, num, sizeof num);
    else if (sp.type == 'd') to_fixed(a, 0, num, sizeof num);
    else if (sp.type == 'x' || sp.type == 'X' || sp.type == 'o' || sp.type == 'b')
      to_base(a, sp.type == 'o' ? 8 : sp.type == 'b' ? 2 : 16, sp.type == 'X', num, sizeof num);
    else if (sp.prec >= 0) to_fixed(a, sp.prec, num, sizeof num);
    else ax_fmt_num(a, num, sizeof num);
    char grouped[2400];
    if (sp.comma && isfinite(a)) group_thousands(num, grouped, sizeof grouped);
    else snprintf(grouped, sizeof grouped, "%s", num);
    size_t bl = strlen(grouped);
    body = malloc(bl + 2);
    memcpy(body, grouped, bl + 1);
    if (sp.type == '%') strcat(body, "%");
  } else {
    AxStr *s = ax_to_str(v);
    body = malloc(s->len + 1);
    memcpy(body, s->data, s->len + 1);
    ax_release(ax_strv(s));
    if (sp.prec >= 0) {
      int n = 0;
      char *q = body;
      while (*q && n < sp.prec) { q += utf8_len(q); n++; }
      *q = '\0';
    }
  }
  char align = sp.align;
  const char *fill = sp.fill[0] ? sp.fill : " ";
  if (sp.zero && !align) { fill = "0"; align = '='; }
  if (!align) align = numeric ? '>' : '<';
  int len = cp_count(sign) + cp_count(body);
  char *buf = NULL;
  size_t bl = 0, cap = 0;
  ax_str_append(&buf, &bl, &cap, "", 0);
  int pad = sp.width > len ? sp.width - len : 0;
  size_t fl = strlen(fill);
  int left = 0, right = 0;
  bool sign_first = false;
  if (pad) {
    if (align == '<') right = pad;
    else if (align == '^') { left = pad / 2; right = pad - left; }
    else if (align == '=' && numeric) { left = pad; sign_first = true; }
    else left = pad;
  }
  if (sign_first) ax_str_append(&buf, &bl, &cap, sign, strlen(sign));
  for (int i = 0; i < left; i++) ax_str_append(&buf, &bl, &cap, fill, fl);
  if (!sign_first) ax_str_append(&buf, &bl, &cap, sign, strlen(sign));
  ax_str_append(&buf, &bl, &cap, body, strlen(body));
  for (int i = 0; i < right; i++) ax_str_append(&buf, &bl, &cap, fill, fl);
  AxStr *out = ax_str_new(buf, bl);
  free(buf);
  free(body);
  return out;
}
