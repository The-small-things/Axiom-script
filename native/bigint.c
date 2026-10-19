// bigint.c — `big(x)`: arbitrary-precision integers with JavaScript's BigInt semantics, which is
// what the reference runtime hands a program.
//
// A magnitude is little-endian 32-bit limbs with no leading zero limb (zero has none), plus a
// sign; zero is never negative. Division truncates toward zero and a remainder takes the
// dividend's sign, as in JavaScript. Conversions follow the specification exactly: a double
// converts by its exact binary value (`big(1e30)` is 1000000000000000019884624838656), a big
// converts to the nearest double (ties to even), strings parse by StringToBigInt, and a big
// compares with a double by mathematical value, not through a conversion.

#include "axiom.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_BITS ((uint64_t)1 << 30)   // V8's limit: "Maximum BigInt size exceeded"

static AxBig *big_alloc(uint32_t n) {
  AxBig *b = calloc(1, sizeof(AxBig) + sizeof(uint32_t) * (n ? n : 1));
  if (!b) abort();
  b->hdr.rc = 1;
  b->hdr.type = AX_BIG;
  b->n = n;
  return b;
}

static AxBig *norm(AxBig *b) {
  while (b->n && b->d[b->n - 1] == 0) b->n--;
  if (!b->n) b->neg = false;
  return b;
}

AxValue ax_bigv(AxBig *b) { AxValue v; v.t = AX_BIG; v.o = (AxObj *)b; return v; }

bool ax_big_is_zero(const AxBig *b) { return b->n == 0; }

static uint64_t bit_length(const AxBig *b) {
  if (!b->n) return 0;
  uint32_t top = b->d[b->n - 1];
  int bits = 32 - __builtin_clz(top);
  return (uint64_t)(b->n - 1) * 32 + (uint64_t)bits;
}

static AxBig *from_u64(uint64_t u, bool neg) {
  AxBig *b = big_alloc(2);
  b->d[0] = (uint32_t)u;
  b->d[1] = (uint32_t)(u >> 32);
  b->neg = neg;
  return norm(b);
}

AxBig *ax_big_from_int(int64_t v) {
  return v < 0 ? from_u64((uint64_t)0 - (uint64_t)v, true) : from_u64((uint64_t)v, false);
}

// The exact value of trunc(x). The caller has checked that x is finite.
AxBig *ax_big_from_double(double x) {
  double t = trunc(x);
  bool neg = t < 0;
  double a = fabs(t);
  if (a < 18446744073709551616.0) return from_u64((uint64_t)a, neg);
  int e;
  double m = frexp(a, &e);                         // a = m * 2^e, m in [0.5, 1)
  uint64_t mant = (uint64_t)ldexp(m, 53);          // a = mant * 2^(e - 53), e - 53 >= 11
  int shift = e - 53;
  uint32_t limbs = (uint32_t)((shift + 64) / 32 + 1);
  AxBig *b = big_alloc(limbs);
  int ls = shift / 32, bs = shift % 32;
  uint64_t lo = mant << bs, hi = bs ? mant >> (64 - bs) : 0;   // mant << bs, up to 85 bits
  uint32_t parts[4] = { (uint32_t)lo, (uint32_t)(lo >> 32), (uint32_t)hi, (uint32_t)(hi >> 32) };
  for (int i = 0; i < 4 && ls + i < (int)limbs; i++) b->d[ls + i] = parts[i];
  b->neg = neg;
  return norm(b);
}

// Number(b): the nearest double, ties to even; beyond the double range, ±Infinity.
double ax_big_to_double(const AxBig *b) {
  uint64_t bits = bit_length(b);
  double r;
  if (bits <= 64) {
    uint64_t u = (uint64_t)(b->n > 0 ? b->d[0] : 0) | ((uint64_t)(b->n > 1 ? b->d[1] : 0) << 32);
    r = (double)u;
  } else {
    // The top 64 bits, with every bit below them folded into the lowest (a sticky bit): 11
    // bits under the 53 that survive, so rounding the 64-bit value rounds the whole correctly.
    uint64_t shift = bits - 64;
    uint64_t top = 0;
    for (int i = 63; i >= 0; i--) {
      uint64_t bit = shift + (uint64_t)i;
      if ((b->d[bit / 32] >> (bit % 32)) & 1) top |= (uint64_t)1 << i;
    }
    bool sticky = false;
    for (uint64_t w = 0; w < shift / 32 && !sticky; w++) sticky = b->d[w] != 0;
    if (!sticky && shift % 32) sticky = (b->d[shift / 32] & ((1u << (shift % 32)) - 1)) != 0;
    if (sticky) top |= 1;
    r = shift > 2000 ? INFINITY : ldexp((double)top, (int)shift);
  }
  return b->neg ? -r : r;
}

// ---- text ------------------------------------------------------------------------------------

char *ax_big_to_cstr(const AxBig *b) {
  if (!b->n) { char *z = malloc(2); z[0] = '0'; z[1] = '\0'; return z; }
  uint32_t n = b->n;
  uint32_t *t = malloc(sizeof(uint32_t) * n);
  memcpy(t, b->d, sizeof(uint32_t) * n);
  size_t cap = (size_t)n * 10 + 3, len = 0;
  char *out = malloc(cap);
  while (n) {
    // Divide by 10^9, collecting nine digits (least significant first).
    uint64_t rem = 0;
    for (uint32_t i = n; i-- > 0;) {
      uint64_t cur = (rem << 32) | t[i];
      t[i] = (uint32_t)(cur / 1000000000u);
      rem = cur % 1000000000u;
    }
    while (n && t[n - 1] == 0) n--;
    for (int k = 0; k < 9; k++) {
      out[len++] = (char)('0' + rem % 10);
      rem /= 10;
      if (!n && !rem) break;
    }
  }
  while (len > 1 && out[len - 1] == '0') len--;
  if (b->neg) out[len++] = '-';
  out[len] = '\0';
  for (size_t i = 0; i < len / 2; i++) { char c = out[i]; out[i] = out[len - 1 - i]; out[len - 1 - i] = c; }
  free(t);
  return out;
}

static void mul_small_add(AxBig **pb, uint32_t m, uint32_t a) {
  AxBig *b = *pb;
  uint64_t carry = a;
  for (uint32_t i = 0; i < b->n; i++) {
    uint64_t cur = (uint64_t)b->d[i] * m + carry;
    b->d[i] = (uint32_t)cur;
    carry = cur >> 32;
  }
  if (carry) {
    AxBig *nb = big_alloc(b->n + 1);
    memcpy(nb->d, b->d, sizeof(uint32_t) * b->n);
    nb->d[b->n] = (uint32_t)carry;
    free(b);
    *pb = nb;
  }
}

static bool js_space(const char *s, size_t i, size_t len, size_t *w) {
  unsigned char c = (unsigned char)s[i];
  if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') { *w = 1; return true; }
  // U+00A0, U+FEFF, U+1680, U+2000–U+200A, U+2028, U+2029, U+202F, U+205F, U+3000
  if (c == 0xC2 && i + 1 < len && (unsigned char)s[i + 1] == 0xA0) { *w = 2; return true; }
  if (i + 2 < len) {
    unsigned c1 = (unsigned char)s[i + 1], c2 = (unsigned char)s[i + 2];
    unsigned cp = ((c & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
    if ((c & 0xF0) == 0xE0 && (cp == 0xFEFF || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 ||
                               cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000)) { *w = 3; return true; }
  }
  return false;
}

size_t ax_js_space(const char *s, size_t i, size_t len) {   // bytes of JS white space at s[i], or 0
  size_t w;
  return js_space(s, i, len, &w) ? w : 0;
}

// StringToBigInt: surrounding white space ignored; empty is 0; decimal with an optional sign,
// or 0x / 0o / 0b digits with none. NULL when the text is not an integer.
AxBig *ax_big_parse(const char *s, size_t len) {
  size_t i = 0, j = len, w;
  while (i < j && js_space(s, i, len, &w)) i += w;
  // Trailing white space: scan forward, remembering where the last non-space ended.
  size_t end = i;
  for (size_t k = i; k < j;) {
    if (js_space(s, k, len, &w)) { k += w; continue; }
    k++;
    end = k;
  }
  j = end;
  if (i == j) return big_alloc(0);
  bool neg = false;
  uint32_t base = 10;
  if (j - i > 2 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) { base = 16; i += 2; }
  else if (j - i > 2 && s[i] == '0' && (s[i + 1] == 'o' || s[i + 1] == 'O')) { base = 8; i += 2; }
  else if (j - i > 2 && s[i] == '0' && (s[i + 1] == 'b' || s[i + 1] == 'B')) { base = 2; i += 2; }
  else if (s[i] == '+' || s[i] == '-') { neg = s[i] == '-'; i++; }
  if (i == j) return NULL;
  AxBig *b = big_alloc(0);
  for (; i < j; i++) {
    char c = s[i];
    uint32_t d;
    if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
    else { free(b); return NULL; }
    if (d >= base) { free(b); return NULL; }
    mul_small_add(&b, base, d);
  }
  b->neg = neg;
  return norm(b);
}

// The integer that `n` digits of `radix` spell (all valid, n > 0).
AxBig *ax_big_parse_digits(const char *s, size_t n, unsigned radix) {
  AxBig *b = big_alloc(0);
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    unsigned d = c >= '0' && c <= '9' ? (unsigned)(c - '0') : c >= 'a' && c <= 'z' ? (unsigned)(c - 'a' + 10) : (unsigned)(c - 'A' + 10);
    mul_small_add(&b, radix, d);
  }
  return norm(b);
}

// ---- arithmetic ------------------------------------------------------------------------------

static int mag_cmp(const AxBig *a, const AxBig *b) {
  if (a->n != b->n) return a->n < b->n ? -1 : 1;
  for (uint32_t i = a->n; i-- > 0;) if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
  return 0;
}

int ax_big_cmp(const AxBig *a, const AxBig *b) {
  if (a->neg != b->neg) return a->neg ? -1 : 1;
  int c = mag_cmp(a, b);
  return a->neg ? -c : c;
}

// A big against a double, by mathematical value. NaN is unordered: *unordered is set.
int ax_big_cmp_num(const AxBig *a, double x, bool *unordered) {
  *unordered = false;
  if (isnan(x)) { *unordered = true; return 0; }
  if (isinf(x)) return x > 0 ? -1 : 1;
  AxBig *t = ax_big_from_double(x);
  int c = ax_big_cmp(a, t);
  free(t);
  if (c) return c;
  double frac = x - trunc(x);
  return frac > 0 ? -1 : frac < 0 ? 1 : 0;
}

static AxBig *mag_add(const AxBig *a, const AxBig *b) {
  if (a->n < b->n) { const AxBig *t = a; a = b; b = t; }
  AxBig *r = big_alloc(a->n + 1);
  uint64_t carry = 0;
  for (uint32_t i = 0; i < a->n; i++) {
    uint64_t cur = (uint64_t)a->d[i] + (i < b->n ? b->d[i] : 0) + carry;
    r->d[i] = (uint32_t)cur;
    carry = cur >> 32;
  }
  r->d[a->n] = (uint32_t)carry;
  return r;
}

static AxBig *mag_sub(const AxBig *a, const AxBig *b) {   // |a| >= |b|
  AxBig *r = big_alloc(a->n);
  int64_t borrow = 0;
  for (uint32_t i = 0; i < a->n; i++) {
    int64_t cur = (int64_t)a->d[i] - (i < b->n ? b->d[i] : 0) - borrow;
    borrow = cur < 0;
    r->d[i] = (uint32_t)(cur + (borrow ? ((int64_t)1 << 32) : 0));
  }
  return r;
}

static AxBig *add_signed(const AxBig *a, bool aneg, const AxBig *b, bool bneg) {
  AxBig *r;
  if (aneg == bneg) { r = mag_add(a, b); r->neg = aneg; }
  else if (mag_cmp(a, b) >= 0) { r = mag_sub(a, b); r->neg = aneg; }
  else { r = mag_sub(b, a); r->neg = bneg; }
  return norm(r);
}

AxBig *ax_big_add(const AxBig *a, const AxBig *b) { return add_signed(a, a->neg, b, b->neg); }
AxBig *ax_big_sub(const AxBig *a, const AxBig *b) { return add_signed(a, a->neg, b, !b->neg); }

AxBig *ax_big_neg(const AxBig *a) {
  AxBig *r = big_alloc(a->n);
  memcpy(r->d, a->d, sizeof(uint32_t) * a->n);
  r->neg = a->n ? !a->neg : false;
  return r;
}

// NULL when the product would pass the size limit.
AxBig *ax_big_mul(const AxBig *a, const AxBig *b) {
  if (!a->n || !b->n) return big_alloc(0);
  if (bit_length(a) + bit_length(b) > MAX_BITS + 1) return NULL;
  AxBig *r = big_alloc(a->n + b->n);
  for (uint32_t i = 0; i < a->n; i++) {
    uint64_t carry = 0, ai = a->d[i];
    for (uint32_t j = 0; j < b->n; j++) {
      uint64_t cur = ai * b->d[j] + r->d[i + j] + carry;
      r->d[i + j] = (uint32_t)cur;
      carry = cur >> 32;
    }
    r->d[i + b->n] = (uint32_t)carry;
  }
  r->neg = a->neg != b->neg;
  return norm(r);
}

// Truncating division (Knuth, algorithm D). b is not zero.
static void mag_divmod(const AxBig *a, const AxBig *b, AxBig **q, AxBig **r) {
  if (mag_cmp(a, b) < 0) {
    *q = big_alloc(0);
    *r = big_alloc(a->n);
    memcpy((*r)->d, a->d, sizeof(uint32_t) * a->n);
    return;
  }
  if (b->n == 1) {
    uint64_t rem = 0, dv = b->d[0];
    AxBig *qq = big_alloc(a->n);
    for (uint32_t i = a->n; i-- > 0;) {
      uint64_t cur = (rem << 32) | a->d[i];
      qq->d[i] = (uint32_t)(cur / dv);
      rem = cur % dv;
    }
    *q = norm(qq);
    *r = from_u64(rem, false);
    return;
  }
  uint32_t n = b->n, m = a->n - b->n;
  int s = __builtin_clz(b->d[n - 1]);
  uint32_t *vn = calloc(n, sizeof(uint32_t)), *un = calloc(a->n + 1, sizeof(uint32_t));
  for (uint32_t i = n - 1; i > 0; i--) vn[i] = (b->d[i] << s) | (s ? (uint32_t)((uint64_t)b->d[i - 1] >> (32 - s)) : 0);
  vn[0] = b->d[0] << s;
  un[a->n] = s ? (uint32_t)((uint64_t)a->d[a->n - 1] >> (32 - s)) : 0;
  for (uint32_t i = a->n - 1; i > 0; i--) un[i] = (a->d[i] << s) | (s ? (uint32_t)((uint64_t)a->d[i - 1] >> (32 - s)) : 0);
  un[0] = a->d[0] << s;
  AxBig *qq = big_alloc(m + 1);
  for (uint32_t jj = m + 1; jj-- > 0;) {
    uint32_t j = jj;
    uint64_t num = ((uint64_t)un[j + n] << 32) | un[j + n - 1];
    uint64_t qhat = num / vn[n - 1], rhat = num % vn[n - 1];
    while (qhat >= ((uint64_t)1 << 32) || qhat * vn[n - 2] > ((rhat << 32) | un[j + n - 2])) {
      qhat--;
      rhat += vn[n - 1];
      if (rhat >= ((uint64_t)1 << 32)) break;
    }
    int64_t borrow = 0;
    uint64_t carry = 0;
    for (uint32_t i = 0; i < n; i++) {
      uint64_t p = qhat * vn[i] + carry;
      carry = p >> 32;
      int64_t t = (int64_t)un[i + j] - (int64_t)(uint32_t)p - borrow;
      borrow = t < 0;
      un[i + j] = (uint32_t)t;
    }
    int64_t t = (int64_t)un[j + n] - (int64_t)carry - borrow;
    un[j + n] = (uint32_t)t;
    if (t < 0) {
      // qhat was one too large: add the divisor back.
      qhat--;
      uint64_t c = 0;
      for (uint32_t i = 0; i < n; i++) {
        uint64_t sum = (uint64_t)un[i + j] + vn[i] + c;
        un[i + j] = (uint32_t)sum;
        c = sum >> 32;
      }
      un[j + n] += (uint32_t)c;
    }
    qq->d[j] = (uint32_t)qhat;
  }
  AxBig *rr = big_alloc(n);
  for (uint32_t i = 0; i < n; i++) rr->d[i] = (un[i] >> s) | (s ? (uint32_t)((uint64_t)un[i + 1] << (32 - s)) : 0);
  free(vn);
  free(un);
  *q = norm(qq);
  *r = norm(rr);
}

void ax_big_divmod(const AxBig *a, const AxBig *b, AxBig **q, AxBig **r) {
  AxBig *qq, *rr;
  mag_divmod(a, b, &qq, &rr);
  qq->neg = qq->n ? a->neg != b->neg : false;
  rr->neg = rr->n ? a->neg : false;
  if (q) *q = qq; else free(qq);
  if (r) *r = rr; else free(rr);
}

// a ** e for e >= 0; NULL when the result would pass the size limit.
AxBig *ax_big_pow(const AxBig *a, const AxBig *e) {
  if (!e->n) return from_u64(1, false);
  if (!a->n) return big_alloc(0);
  if (a->n == 1 && a->d[0] == 1) return from_u64(1, a->neg && (e->d[0] & 1));
  if (e->n > 1 || (uint64_t)e->d[0] * (bit_length(a) - 1) > MAX_BITS) return NULL;
  uint32_t k = e->d[0];
  AxBig *result = from_u64(1, false);
  AxBig *base = big_alloc(a->n);
  memcpy(base->d, a->d, sizeof(uint32_t) * a->n);
  base->neg = a->neg;
  while (k) {
    if (k & 1) {
      AxBig *t = ax_big_mul(result, base);
      free(result);
      if (!t) { free(base); return NULL; }
      result = t;
    }
    k >>= 1;
    if (k) {
      AxBig *t = ax_big_mul(base, base);
      free(base);
      if (!t) { free(result); return NULL; }
      base = t;
    }
  }
  free(base);
  return result;
}
