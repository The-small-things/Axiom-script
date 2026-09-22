// jsmath.c — JavaScript's Math, bit for bit.
//
// V8 implements Math.sin, Math.exp, Math.pow and the rest with fdlibm (via FreeBSD's msun),
// not with the platform's libm. glibc's results differ from fdlibm's in the last bit on a few
// percent of inputs (cbrt on almost half), which is invisible in one call and fatal to a
// simulation that must produce the same numbers on both runtimes for thousands of frames.
// So the native runtime carries the same algorithms. Every function here is checked against
// Node on hundreds of thousands of inputs by tests/mathcheck.sh.
//
// The code follows fdlibm/FreeBSD msun, which is under this notice:
//
//   Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
//   Developed at SunSoft, a Sun Microsystems, Inc. business.
//   Permission to use, copy, modify, and distribute this software is freely granted,
//   provided that this notice is preserved.

#include <stdint.h>
#include <string.h>
#include <math.h>

typedef union { double f; uint64_t u; } DW;

#define EXTRACT_WORDS(hi, lo, v) do { DW w_; w_.f = (v); (hi) = (int32_t)(w_.u >> 32); (lo) = (uint32_t)w_.u; } while (0)
#define GET_HIGH_WORD(hi, v) do { DW w_; w_.f = (v); (hi) = (int32_t)(w_.u >> 32); } while (0)
#define GET_LOW_WORD(lo, v) do { DW w_; w_.f = (v); (lo) = (uint32_t)w_.u; } while (0)
#define INSERT_WORDS(v, hi, lo) do { DW w_; w_.u = ((uint64_t)(uint32_t)(hi) << 32) | (uint32_t)(lo); (v) = w_.f; } while (0)
#define SET_HIGH_WORD(v, hi) do { DW w_; w_.f = (v); w_.u = (w_.u & 0xffffffffULL) | ((uint64_t)(uint32_t)(hi) << 32); (v) = w_.f; } while (0)
#define SET_LOW_WORD(v, lo) do { DW w_; w_.f = (v); w_.u = (w_.u & 0xffffffff00000000ULL) | (uint32_t)(lo); (v) = w_.f; } while (0)

// ---- argument reduction for the trigonometric functions --------------------------------------

static const int32_t two_over_pi[] = {
  0xA2F983, 0x6E4E44, 0x1529FC, 0x2757D1, 0xF534DD, 0xC0DB62, 0x95993C, 0x439041, 0xFE5163,
  0xABDEBB, 0xC561B7, 0x246E3A, 0x424DD2, 0xE00649, 0x2EEA09, 0xD1921C, 0xFE1DEB, 0x1CB129,
  0xA73EE8, 0x8235F5, 0x2EBB44, 0x84E99C, 0x7026B4, 0x5F7E41, 0x3991D6, 0x398353, 0x39F49C,
  0x845F8B, 0xBDF928, 0x3B1FF8, 0x97FFDE, 0x05980F, 0xEF2F11, 0x8B5A0A, 0x6D1F6D, 0x367ECF,
  0x27CB09, 0xB74F46, 0x3F669E, 0x5FEA2D, 0x7527BA, 0xC7EBE5, 0xF17B3D, 0x0739F7, 0x8A5292,
  0xEA6BFB, 0x5FB11F, 0x8D5D08, 0x560330, 0x46FC7B, 0x6BABF0, 0xCFBC20, 0x9AF436, 0x1DA9E3,
  0x91615E, 0xE61B08, 0x659985, 0x5F14A0, 0x68408D, 0xFFD880, 0x4D7327, 0x310606, 0x1556CA,
  0x73A8C9, 0x60E27B, 0xC08C6B,
};

static const int32_t npio2_hw[] = {
  0x3FF921FB, 0x400921FB, 0x4012D97C, 0x401921FB, 0x401F6A7A, 0x4022D97C, 0x4025FDBB, 0x402921FB,
  0x402C463A, 0x402F6A7A, 0x4031475C, 0x4032D97C, 0x40346B9C, 0x4035FDBB, 0x40378FDB, 0x403921FB,
  0x403AB41B, 0x403C463A, 0x403DD85A, 0x403F6A7A, 0x40407E4C, 0x4041475C, 0x4042106C, 0x4042D97C,
  0x4043A28C, 0x40446B9C, 0x404534AC, 0x4045FDBB, 0x4046C6CB, 0x40478FDB, 0x404858EB, 0x404921FB,
};

static int kernel_rem_pio2(double *x, double *y, int e0, int nx, int prec, const int32_t *ipio2) {
  static const int init_jk[] = { 2, 3, 4, 6 };
  static const double PIo2[] = {
    1.57079625129699707031e+00, 7.54978941586159635335e-08, 5.39030252995776476554e-15,
    3.28200341580791294123e-22, 1.27065575308067607349e-29, 1.22933308981111328932e-36,
    2.73370053816464559624e-44, 2.16741683877804819444e-51,
  };
  const double zero = 0.0, one = 1.0, two24 = 1.67772160000000000000e+07, twon24 = 5.96046447753906250000e-08;
  int32_t jz, jx, jv, jp, jk, carry, n, iq[20], i, j, k, m, q0, ih;
  double z, fw, f[20], fq[20] = { 0 }, q[20];

  jk = init_jk[prec];
  jp = jk;
  jx = nx - 1;
  jv = (e0 - 3) / 24;
  if (jv < 0) jv = 0;
  q0 = e0 - 24 * (jv + 1);
  j = jv - jx;
  m = jx + jk;
  for (i = 0; i <= m; i++, j++) f[i] = (j < 0) ? zero : (double)ipio2[j];
  for (i = 0; i <= jk; i++) {
    for (j = 0, fw = 0.0; j <= jx; j++) fw += x[j] * f[jx + i - j];
    q[i] = fw;
  }
  jz = jk;
recompute:
  for (i = 0, j = jz, z = q[jz]; j > 0; i++, j--) {
    fw = (double)((int32_t)(twon24 * z));
    iq[i] = (int32_t)(z - two24 * fw);
    z = q[j - 1] + fw;
  }
  z = scalbn(z, q0);
  z -= 8.0 * floor(z * 0.125);
  n = (int32_t)z;
  z -= (double)n;
  ih = 0;
  if (q0 > 0) {
    i = (iq[jz - 1] >> (24 - q0));
    n += i;
    iq[jz - 1] -= i << (24 - q0);
    ih = iq[jz - 1] >> (23 - q0);
  } else if (q0 == 0) {
    ih = iq[jz - 1] >> 23;
  } else if (z >= 0.5) {
    ih = 2;
  }
  if (ih > 0) {
    n += 1;
    carry = 0;
    for (i = 0; i < jz; i++) {
      j = iq[i];
      if (carry == 0) {
        if (j != 0) { carry = 1; iq[i] = 0x1000000 - j; }
      } else {
        iq[i] = 0xffffff - j;
      }
    }
    if (q0 > 0) {
      switch (q0) {
        case 1: iq[jz - 1] &= 0x7fffff; break;
        case 2: iq[jz - 1] &= 0x3fffff; break;
      }
    }
    if (ih == 2) {
      z = one - z;
      if (carry != 0) z -= scalbn(one, q0);
    }
  }
  if (z == zero) {
    j = 0;
    for (i = jz - 1; i >= jk; i--) j |= iq[i];
    if (j == 0) {
      for (k = 1; iq[jk - k] == 0; k++) {}
      for (i = jz + 1; i <= jz + k; i++) {
        f[jx + i] = (double)ipio2[jv + i];
        for (j = 0, fw = 0.0; j <= jx; j++) fw += x[j] * f[jx + i - j];
        q[i] = fw;
      }
      jz += k;
      goto recompute;
    }
  }
  if (z == 0.0) {
    jz -= 1;
    q0 -= 24;
    while (iq[jz] == 0) { jz--; q0 -= 24; }
  } else {
    z = scalbn(z, -q0);
    if (z >= two24) {
      fw = (double)((int32_t)(twon24 * z));
      iq[jz] = (int32_t)(z - two24 * fw);
      jz += 1;
      q0 += 24;
      iq[jz] = (int32_t)fw;
    } else {
      iq[jz] = (int32_t)z;
    }
  }
  fw = scalbn(one, q0);
  for (i = jz; i >= 0; i--) { q[i] = fw * (double)iq[i]; fw *= twon24; }
  for (i = jz; i >= 0; i--) {
    for (fw = 0.0, k = 0; k <= jp && k <= jz - i; k++) fw += PIo2[k] * q[i + k];
    fq[jz - i] = fw;
  }
  // prec == 2
  fw = 0.0;
  for (i = jz; i >= 0; i--) fw += fq[i];
  y[0] = (ih == 0) ? fw : -fw;
  fw = fq[0] - fw;
  for (i = 1; i <= jz; i++) fw += fq[i];
  y[1] = (ih == 0) ? fw : -fw;
  return n & 7;
}

static int32_t rem_pio2(double x, double *y) {
  const double zero = 0.00000000000000000000e+00, half = 5.00000000000000000000e-01,
    two24 = 1.67772160000000000000e+07, invpio2 = 6.36619772367581382433e-01,
    pio2_1 = 1.57079632673412561417e+00, pio2_1t = 6.07710050650619224932e-11,
    pio2_2 = 6.07710050630396597660e-11, pio2_2t = 2.02226624879595063154e-21,
    pio2_3 = 2.02226624871116645580e-21, pio2_3t = 8.47842766036889956997e-32;
  double z, w, t, r, fn;
  double tx[3];
  int32_t e0, i, j, nx, n, ix, hx;
  uint32_t low;

  z = 0;
  GET_HIGH_WORD(hx, x);
  ix = hx & 0x7fffffff;
  if (ix <= 0x3fe921fb) { y[0] = x; y[1] = 0; return 0; }
  if (ix < 0x4002d97c) {
    if (hx > 0) {
      z = x - pio2_1;
      if (ix != 0x3ff921fb) { y[0] = z - pio2_1t; y[1] = (z - y[0]) - pio2_1t; }
      else { z -= pio2_2; y[0] = z - pio2_2t; y[1] = (z - y[0]) - pio2_2t; }
      return 1;
    } else {
      z = x + pio2_1;
      if (ix != 0x3ff921fb) { y[0] = z + pio2_1t; y[1] = (z - y[0]) + pio2_1t; }
      else { z += pio2_2; y[0] = z + pio2_2t; y[1] = (z - y[0]) + pio2_2t; }
      return -1;
    }
  }
  if (ix <= 0x413921fb) {
    t = fabs(x);
    n = (int32_t)(t * invpio2 + half);
    fn = (double)n;
    r = t - fn * pio2_1;
    w = fn * pio2_1t;
    if (n < 32 && ix != npio2_hw[n - 1]) {
      y[0] = r - w;
    } else {
      uint32_t high;
      j = ix >> 20;
      y[0] = r - w;
      GET_HIGH_WORD(high, y[0]);
      i = j - ((high >> 20) & 0x7ff);
      if (i > 16) {
        t = r;
        w = fn * pio2_2;
        r = t - w;
        w = fn * pio2_2t - ((t - r) - w);
        y[0] = r - w;
        GET_HIGH_WORD(high, y[0]);
        i = j - ((high >> 20) & 0x7ff);
        if (i > 49) {
          t = r;
          w = fn * pio2_3;
          r = t - w;
          w = fn * pio2_3t - ((t - r) - w);
          y[0] = r - w;
        }
      }
    }
    y[1] = (r - y[0]) - w;
    if (hx < 0) { y[0] = -y[0]; y[1] = -y[1]; return -n; }
    return n;
  }
  if (ix >= 0x7ff00000) { y[0] = y[1] = x - x; return 0; }
  GET_LOW_WORD(low, x);
  SET_LOW_WORD(z, low);
  e0 = (ix >> 20) - 1046;
  SET_HIGH_WORD(z, ix - (e0 << 20));
  for (i = 0; i < 2; i++) {
    tx[i] = (double)((int32_t)(z));
    z = (z - tx[i]) * two24;
  }
  tx[2] = z;
  nx = 3;
  while (tx[nx - 1] == zero) nx--;
  n = kernel_rem_pio2(tx, y, e0, nx, 2, two_over_pi);
  if (hx < 0) { y[0] = -y[0]; y[1] = -y[1]; return -n; }
  return n;
}

static double k_sin(double x, double y, int iy) {
  const double half = 5.00000000000000000000e-01, S1 = -1.66666666666666324348e-01,
    S2 = 8.33333333332248946124e-03, S3 = -1.98412698298579493134e-04,
    S4 = 2.75573137070700676789e-06, S5 = -2.50507602534068634195e-08,
    S6 = 1.58969099521155010221e-10;
  double z, r, v;
  int32_t ix;
  GET_HIGH_WORD(ix, x);
  ix &= 0x7fffffff;
  if (ix < 0x3e400000) { if ((int)x == 0) return x; }
  z = x * x;
  v = z * x;
  r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
  if (iy == 0) return x + v * (S1 + z * r);
  return x - ((z * (half * y - v * r) - y) - v * S1);
}

static double k_cos(double x, double y) {
  const double one = 1.0, C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03,
    C3 = 2.48015872894767294178e-05, C4 = -2.75573143513906633035e-07,
    C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;
  double a, iz, z, r, qx;
  int32_t ix;
  GET_HIGH_WORD(ix, x);
  ix &= 0x7fffffff;
  if (ix < 0x3e400000) { if ((int)x == 0) return one; }
  z = x * x;
  r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
  if (ix < 0x3FD33333) return one - (0.5 * z - (z * r - x * y));
  if (ix > 0x3fe90000) qx = 0.28125;
  else INSERT_WORDS(qx, ix - 0x00200000, 0);
  iz = 0.5 * z - qx;
  a = one - qx;
  return a - (iz - (z * r - x * y));
}

static double k_tan(double x, double y, int iy) {
  static const double T[] = {
    3.33333333333334091986e-01, 1.33333333333201242699e-01, 5.39682539762260521377e-02,
    2.18694882948595424599e-02, 8.86323982359930005737e-03, 3.59207910759131235356e-03,
    1.45620945432529025516e-03, 5.88041240820264096874e-04, 2.46463134818469906812e-04,
    7.81794442939557092300e-05, 7.14072491382608190305e-05, -1.85586374855275456654e-05,
    2.59073051863633712884e-05,
  };
  const double one = 1.0, pio4 = 7.85398163397448278999e-01, pio4lo = 3.06161699786838301793e-17;
  double z, r, v, w, s;
  int32_t ix, hx;
  GET_HIGH_WORD(hx, x);
  ix = hx & 0x7fffffff;
  if (ix < 0x3e300000) {
    if ((int)x == 0) {
      uint32_t low;
      GET_LOW_WORD(low, x);
      if (((ix | low) | (iy + 1)) == 0) return one / fabs(x);
      if (iy == 1) return x;
      double a, t;
      z = w = x + y;
      SET_LOW_WORD(z, 0);
      v = y - (z - x);
      t = a = -one / w;
      SET_LOW_WORD(t, 0);
      s = one + t * z;
      return t + a * (s + t * v);
    }
  }
  if (ix >= 0x3FE59428) {
    if (hx < 0) { x = -x; y = -y; }
    z = pio4 - x;
    w = pio4lo - y;
    x = z + w;
    y = 0.0;
  }
  z = x * x;
  w = z * z;
  r = T[1] + w * (T[3] + w * (T[5] + w * (T[7] + w * (T[9] + w * T[11]))));
  v = z * (T[2] + w * (T[4] + w * (T[6] + w * (T[8] + w * (T[10] + w * T[12])))));
  s = z * x;
  r = y + z * (s * (r + v) + y);
  r += T[0] * s;
  w = x + r;
  if (ix >= 0x3FE59428) {
    v = iy;
    return (1 - ((hx >> 30) & 2)) * (v - 2.0 * (x - (w * w / (w + v) - r)));
  }
  if (iy == 1) return w;
  double a, t;
  z = w;
  SET_LOW_WORD(z, 0);
  v = r - (z - x);
  t = a = -1.0 / w;
  SET_LOW_WORD(t, 0);
  s = 1.0 + t * z;
  return t + a * (s + t * v);
}

double js_sin(double x) {
  double y[2];
  int32_t ix;
  GET_HIGH_WORD(ix, x);
  ix &= 0x7fffffff;
  if (ix <= 0x3fe921fb) return k_sin(x, 0.0, 0);
  if (ix >= 0x7ff00000) return x - x;
  int32_t n = rem_pio2(x, y);
  switch (n & 3) {
    case 0: return k_sin(y[0], y[1], 1);
    case 1: return k_cos(y[0], y[1]);
    case 2: return -k_sin(y[0], y[1], 1);
    default: return -k_cos(y[0], y[1]);
  }
}

double js_cos(double x) {
  double y[2];
  int32_t ix;
  GET_HIGH_WORD(ix, x);
  ix &= 0x7fffffff;
  if (ix <= 0x3fe921fb) return k_cos(x, 0.0);
  if (ix >= 0x7ff00000) return x - x;
  int32_t n = rem_pio2(x, y);
  switch (n & 3) {
    case 0: return k_cos(y[0], y[1]);
    case 1: return -k_sin(y[0], y[1], 1);
    case 2: return -k_cos(y[0], y[1]);
    default: return k_sin(y[0], y[1], 1);
  }
}

double js_tan(double x) {
  double y[2];
  int32_t ix;
  GET_HIGH_WORD(ix, x);
  ix &= 0x7fffffff;
  if (ix <= 0x3fe921fb) return k_tan(x, 0.0, 1);
  if (ix >= 0x7ff00000) return x - x;
  int32_t n = rem_pio2(x, y);
  return k_tan(y[0], y[1], 1 - ((n & 1) << 1));
}

// ---- inverse trigonometric -------------------------------------------------------------------

static const double pS0 = 1.66666666666666657415e-01, pS1 = -3.25565818622400915405e-01,
  pS2 = 2.01212532134862925881e-01, pS3 = -4.00555345006794114027e-02,
  pS4 = 7.91534994289814532176e-04, pS5 = 3.47933107596021167570e-05,
  qS1 = -2.40339491173441421878e+00, qS2 = 2.02094576023350569471e+00,
  qS3 = -6.88283971605453293030e-01, qS4 = 7.70381505559019352791e-02;
static const double pio2_hi = 1.57079632679489655800e+00, pio2_lo = 6.12323399573676603587e-17;

double js_asin(double x) {
  const double one = 1.0, huge = 1.000e+300, pio4_hi = 7.85398163397448278999e-01;
  double t = 0.0, w, p, q, c, r, s;
  int32_t hx, ix;
  GET_HIGH_WORD(hx, x);
  ix = hx & 0x7fffffff;
  if (ix >= 0x3ff00000) {
    uint32_t lx;
    GET_LOW_WORD(lx, x);
    if (((ix - 0x3ff00000) | lx) == 0) return x * pio2_hi + x * pio2_lo;
    return (x - x) / (x - x);
  } else if (ix < 0x3fe00000) {
    if (ix < 0x3e400000) { if (huge + x > one) return x; }
    t = x * x;
    p = t * (pS0 + t * (pS1 + t * (pS2 + t * (pS3 + t * (pS4 + t * pS5)))));
    q = one + t * (qS1 + t * (qS2 + t * (qS3 + t * qS4)));
    w = p / q;
    return x + x * w;
  }
  w = one - fabs(x);
  t = w * 0.5;
  p = t * (pS0 + t * (pS1 + t * (pS2 + t * (pS3 + t * (pS4 + t * pS5)))));
  q = one + t * (qS1 + t * (qS2 + t * (qS3 + t * qS4)));
  s = sqrt(t);
  if (ix >= 0x3FEF3333) {
    w = p / q;
    t = pio2_hi - (2.0 * (s + s * w) - pio2_lo);
  } else {
    w = s;
    SET_LOW_WORD(w, 0);
    c = (t - w * w) / (s + w);
    r = p / q;
    p = 2.0 * s * r - (pio2_lo - 2.0 * c);
    q = pio4_hi - 2.0 * w;
    t = pio4_hi - (p - q);
  }
  return hx > 0 ? t : -t;
}

double js_acos(double x) {
  const double one = 1.0, pi = 3.14159265358979311600e+00;
  double z, p, q, r, w, s, c, df;
  int32_t hx, ix;
  GET_HIGH_WORD(hx, x);
  ix = hx & 0x7fffffff;
  if (ix >= 0x3ff00000) {
    uint32_t lx;
    GET_LOW_WORD(lx, x);
    if (((ix - 0x3ff00000) | lx) == 0) {
      if (hx > 0) return 0.0;
      return pi + 2.0 * pio2_lo;
    }
    return (x - x) / (x - x);
  }
  if (ix < 0x3fe00000) {
    if (ix <= 0x3c600000) return pio2_hi + pio2_lo;
    z = x * x;
    p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
    q = one + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
    r = p / q;
    return pio2_hi - (x - (pio2_lo - x * r));
  } else if (hx < 0) {
    z = (one + x) * 0.5;
    p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
    q = one + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
    s = sqrt(z);
    r = p / q;
    w = r * s - pio2_lo;
    return pi - 2.0 * (s + w);
  }
  z = (one - x) * 0.5;
  s = sqrt(z);
  df = s;
  SET_LOW_WORD(df, 0);
  c = (z - df * df) / (s + df);
  p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
  q = one + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
  r = p / q;
  w = r * s + c;
  return 2.0 * (df + w);
}

double js_atan(double x) {
  static const double atanhi[] = {
    4.63647609000806093515e-01, 7.85398163397448278999e-01, 9.82793723247329054082e-01, 1.57079632679489655800e+00,
  };
  static const double atanlo[] = {
    2.26987774529616870924e-17, 3.06161699786838301793e-17, 1.39033110312309984516e-17, 6.12323399573676603587e-17,
  };
  static const double aT[] = {
    3.33333333333329318027e-01, -1.99999999998764832476e-01, 1.42857142725034663711e-01,
    -1.11111104054623557880e-01, 9.09088713343650656196e-02, -7.69187620504482999495e-02,
    6.66107313738753120669e-02, -5.83357013379057348645e-02, 4.97687799461593236017e-02,
    -3.65315727442169155270e-02, 1.62858201153657823623e-02,
  };
  const double one = 1.0, huge = 1.0e300;
  double w, s1, s2, z;
  int32_t ix, hx, id;
  GET_HIGH_WORD(hx, x);
  ix = hx & 0x7fffffff;
  if (ix >= 0x44100000) {
    uint32_t low;
    GET_LOW_WORD(low, x);
    if (ix > 0x7ff00000 || (ix == 0x7ff00000 && (low != 0))) return x + x;
    if (hx > 0) return atanhi[3] + atanlo[3];
    return -atanhi[3] - atanlo[3];
  }
  if (ix < 0x3fdc0000) {
    if (ix < 0x3e400000) { if (huge + x > one) return x; }
    id = -1;
  } else {
    x = fabs(x);
    if (ix < 0x3ff30000) {
      if (ix < 0x3fe60000) { id = 0; x = (2.0 * x - one) / (2.0 + x); }
      else { id = 1; x = (x - one) / (x + one); }
    } else {
      if (ix < 0x40038000) { id = 2; x = (x - 1.5) / (one + 1.5 * x); }
      else { id = 3; x = -1.0 / x; }
    }
  }
  z = x * x;
  w = z * z;
  s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
  s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
  if (id < 0) return x - x * (s1 + s2);
  z = atanhi[id] - ((x * (s1 + s2) - atanlo[id]) - x);
  return (hx < 0) ? -z : z;
}

double js_atan2(double y, double x) {
  const double tiny = 1.0e-300, zero = 0.0, pi_o_4 = 7.8539816339744827900E-01,
    pi_o_2 = 1.5707963267948965580E+00, pi = 3.1415926535897931160E+00, pi_lo = 1.2246467991473531772E-16;
  double z;
  int32_t k, m, hx, hy, ix, iy;
  uint32_t lx, ly;
  EXTRACT_WORDS(hx, lx, x);
  ix = hx & 0x7fffffff;
  EXTRACT_WORDS(hy, ly, y);
  iy = hy & 0x7fffffff;
  if (((uint32_t)ix | ((lx | -lx) >> 31)) > 0x7ff00000 || ((uint32_t)iy | ((ly | -ly) >> 31)) > 0x7ff00000) return x + y;
  if (hx == 0x3ff00000 && lx == 0) return js_atan(y);
  m = ((hy >> 31) & 1) | ((hx >> 30) & 2);
  if ((iy | ly) == 0) {
    switch (m) {
      case 0: case 1: return y;
      case 2: return pi + tiny;
      default: return -pi - tiny;
    }
  }
  if ((ix | lx) == 0) return (hy < 0) ? -pi_o_2 - tiny : pi_o_2 + tiny;
  if (ix == 0x7ff00000) {
    if (iy == 0x7ff00000) {
      switch (m) {
        case 0: return pi_o_4 + tiny;
        case 1: return -pi_o_4 - tiny;
        case 2: return 3.0 * pi_o_4 + tiny;
        default: return -3.0 * pi_o_4 - tiny;
      }
    } else {
      switch (m) {
        case 0: return zero;
        case 1: return -zero;
        case 2: return pi + tiny;
        default: return -pi - tiny;
      }
    }
  }
  if (iy == 0x7ff00000) return (hy < 0) ? -pi_o_2 - tiny : pi_o_2 + tiny;
  k = (iy - ix) >> 20;
  if (k > 60) { z = pi_o_2 + 0.5 * pi_lo; m &= 1; }
  else if (hx < 0 && k < -60) z = 0.0;
  else z = js_atan(fabs(y / x));
  switch (m) {
    case 0: return z;
    case 1: return -z;
    case 2: return pi - (z - pi_lo);
    default: return (z - pi_lo) - pi;
  }
}

// ---- exponentials and logarithms -------------------------------------------------------------

double js_exp(double x) {
  const double one = 1.0, halF[2] = { 0.5, -0.5 }, huge = 1.0e+300,
    o_threshold = 7.09782712893383973096e+02, u_threshold = -7.45133219101941108420e+02,
    ln2HI[2] = { 6.93147180369123816490e-01, -6.93147180369123816490e-01 },
    ln2LO[2] = { 1.90821492927058770002e-10, -1.90821492927058770002e-10 },
    invln2 = 1.44269504088896338700e+00, P1 = 1.66666666666666019037e-01,
    P2 = -2.77777777770155933842e-03, P3 = 6.61375632143793436117e-05,
    P4 = -1.65339022054652515390e-06, P5 = 4.13813679705723846039e-08,
    twom1000 = 9.33263618503218878990e-302;
  double y, hi = 0.0, lo = 0.0, c, t, twopk;
  int32_t k = 0, xsb;
  uint32_t hx;
  GET_HIGH_WORD(hx, x);
  xsb = (hx >> 31) & 1;
  hx &= 0x7fffffff;
  if (hx >= 0x40862E42) {
    if (hx >= 0x7ff00000) {
      uint32_t lx;
      GET_LOW_WORD(lx, x);
      if (((hx & 0xfffff) | lx) != 0) return x + x;
      return (xsb == 0) ? x : 0.0;
    }
    if (x > o_threshold) return huge * huge;
    if (x < u_threshold) return twom1000 * twom1000;
  }
  if (hx > 0x3fd62e42) {
    if (hx < 0x3FF0A2B2) {
      hi = x - ln2HI[xsb];
      lo = ln2LO[xsb];
      k = 1 - xsb - xsb;
    } else {
      k = (int32_t)(invln2 * x + halF[xsb]);
      t = k;
      hi = x - t * ln2HI[0];
      lo = t * ln2LO[0];
    }
    x = hi - lo;
  } else if (hx < 0x3e300000) {
    if (huge + x > one) return one + x;
  } else {
    k = 0;
  }
  t = x * x;
  if (k >= -1021) INSERT_WORDS(twopk, 0x3ff00000 + (k << 20), 0);
  else INSERT_WORDS(twopk, 0x3ff00000 + ((k + 1000) << 20), 0);
  c = x - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
  if (k == 0) return one - ((x * c) / (c - 2.0) - x);
  y = one - ((lo - (x * c) / (2.0 - c)) - hi);
  if (k >= -1021) {
    if (k == 1024) return y * 2.0 * 0x1p1023;
    return y * twopk;
  }
  return y * twopk * twom1000;
}

static const double Lg1 = 6.666666666666735130e-01, Lg2 = 3.999999999940941908e-01,
  Lg3 = 2.857142874366239149e-01, Lg4 = 2.222219843214978396e-01, Lg5 = 1.818357216161805012e-01,
  Lg6 = 1.531383769920937332e-01, Lg7 = 1.479819860511658591e-01;
static const double ln2_hi = 6.93147180369123816490e-01, ln2_lo = 1.90821492927058770002e-10,
  two54 = 1.80143985094819840000e+16;

double js_log(double x) {
  double hfsq, f, s, z, R, w, t1, t2, dk;
  int32_t k, hx, i, j;
  uint32_t lx;
  EXTRACT_WORDS(hx, lx, x);
  k = 0;
  if (hx < 0x00100000) {
    if (((hx & 0x7fffffff) | lx) == 0) return -INFINITY;
    if (hx < 0) return NAN;
    k -= 54;
    x *= two54;
    GET_HIGH_WORD(hx, x);
  }
  if (hx >= 0x7ff00000) return x + x;
  k += (hx >> 20) - 1023;
  hx &= 0x000fffff;
  i = (hx + 0x95f64) & 0x100000;
  SET_HIGH_WORD(x, hx | (i ^ 0x3ff00000));
  k += (i >> 20);
  f = x - 1.0;
  if ((0x000fffff & (2 + hx)) < 3) {
    if (f == 0.0) {
      if (k == 0) return 0.0;
      dk = (double)k;
      return dk * ln2_hi + dk * ln2_lo;
    }
    R = f * f * (0.5 - 0.33333333333333333 * f);
    if (k == 0) return f - R;
    dk = (double)k;
    return dk * ln2_hi - ((R - dk * ln2_lo) - f);
  }
  s = f / (2.0 + f);
  dk = (double)k;
  z = s * s;
  i = hx - 0x6147a;
  w = z * z;
  j = 0x6b851 - hx;
  t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
  t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
  i |= j;
  R = t2 + t1;
  if (i > 0) {
    hfsq = 0.5 * f * f;
    if (k == 0) return f - (hfsq - s * (hfsq + R));
    return dk * ln2_hi - ((hfsq - (s * (hfsq + R) + dk * ln2_lo)) - f);
  }
  if (k == 0) return f - s * (f - R);
  return dk * ln2_hi - ((s * (f - R) - dk * ln2_lo) - f);
}

static double k_log1p(double f) {
  double hfsq, s, z, R, w, t1, t2;
  s = f / (2.0 + f);
  z = s * s;
  w = z * z;
  t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
  t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
  R = t2 + t1;
  hfsq = 0.5 * f * f;
  return s * (hfsq + R);
}

double js_log2(double x) {
  const double ivln2hi = 1.44269504072144627571e+00, ivln2lo = 1.67517131648865118353e-10;
  double f, hfsq, hi, lo, r, val_hi, val_lo, w, y;
  int32_t i, k, hx;
  uint32_t lx;
  EXTRACT_WORDS(hx, lx, x);
  k = 0;
  if (hx < 0x00100000) {
    if (((hx & 0x7fffffff) | lx) == 0) return -INFINITY;
    if (hx < 0) return NAN;
    k -= 54;
    x *= two54;
    GET_HIGH_WORD(hx, x);
  }
  if (hx >= 0x7ff00000) return x + x;
  if (hx == 0x3ff00000 && lx == 0) return 0.0;
  k += (hx >> 20) - 1023;
  hx &= 0x000fffff;
  i = (hx + 0x95f64) & 0x100000;
  SET_HIGH_WORD(x, hx | (i ^ 0x3ff00000));
  k += (i >> 20);
  y = (double)k;
  f = x - 1.0;
  hfsq = 0.5 * f * f;
  r = k_log1p(f);
  hi = f - hfsq;
  SET_LOW_WORD(hi, 0);
  lo = (f - hi) - hfsq + r;
  val_hi = hi * ivln2hi;
  val_lo = (lo + hi) * ivln2lo + lo * ivln2hi;
  w = y + val_hi;
  val_lo += (y - w) + val_hi;
  val_hi = w;
  return val_lo + val_hi;
}

// V8 keeps fdlibm's original log10 (via log), not FreeBSD's later rewrite.
double js_log10(double x) {
  const double ivln10 = 4.34294481903251816668e-01, log10_2hi = 3.01029995663611771306e-01,
    log10_2lo = 3.69423907715893078616e-13;
  double y;
  int32_t i, k, hx;
  uint32_t lx;
  EXTRACT_WORDS(hx, lx, x);
  k = 0;
  if (hx < 0x00100000) {
    if (((hx & 0x7fffffff) | lx) == 0) return -INFINITY;
    if (hx < 0) return NAN;
    k -= 54;
    x *= two54;
    GET_HIGH_WORD(hx, x);
    GET_LOW_WORD(lx, x);
  }
  if (hx >= 0x7ff00000) return x + x;
  if (hx == 0x3ff00000 && lx == 0) return 0.0;
  k += (hx >> 20) - 1023;
  i = (int32_t)(((uint32_t)k & 0x80000000) >> 31);
  hx = (hx & 0x000fffff) | ((0x3ff - i) << 20);
  y = k + i;
  SET_HIGH_WORD(x, hx);
  SET_LOW_WORD(x, lx);
  double z = y * log10_2lo + ivln10 * js_log(x);
  return z + y * log10_2hi;
}

double js_expm1(double x) {
  const double one = 1.0, tiny = 1.0e-300, huge = 1.0e+300,
    o_threshold = 7.09782712893383973096e+02, invln2 = 1.44269504088896338700e+00,
    Q1 = -3.33333333333331316428e-02, Q2 = 1.58730158725481460165e-03,
    Q3 = -7.93650757867487942473e-05, Q4 = 4.00821782732936239552e-06,
    Q5 = -2.01099218183624371326e-07;
  double y, hi, lo, c = 0, t, e, hxs, hfx, r1, twopk;
  int32_t k, xsb;
  uint32_t hx;
  GET_HIGH_WORD(hx, x);
  xsb = hx & 0x80000000;
  hx &= 0x7fffffff;
  if (hx >= 0x4043687A) {
    if (hx >= 0x40862E42) {
      if (hx >= 0x7ff00000) {
        uint32_t low;
        GET_LOW_WORD(low, x);
        if (((hx & 0xfffff) | low) != 0) return x + x;
        return (xsb == 0) ? x : -1.0;
      }
      if (x > o_threshold) return huge * huge;
    }
    if (xsb != 0) {
      if (x + tiny < 0.0) return tiny - one;
    }
  }
  if (hx > 0x3fd62e42) {
    if (hx < 0x3FF0A2B2) {
      if (xsb == 0) { hi = x - ln2_hi; lo = ln2_lo; k = 1; }
      else { hi = x + ln2_hi; lo = -ln2_lo; k = -1; }
    } else {
      k = (int32_t)(invln2 * x + ((xsb == 0) ? 0.5 : -0.5));
      t = k;
      hi = x - t * ln2_hi;
      lo = t * ln2_lo;
    }
    x = hi - lo;
    c = (hi - x) - lo;
  } else if (hx < 0x3c900000) {
    t = huge + x;
    return x - (t - (huge + x));
  } else {
    k = 0;
  }
  hfx = 0.5 * x;
  hxs = x * hfx;
  r1 = one + hxs * (Q1 + hxs * (Q2 + hxs * (Q3 + hxs * (Q4 + hxs * Q5))));
  t = 3.0 - r1 * hfx;
  e = hxs * ((r1 - t) / (6.0 - x * t));
  if (k == 0) return x - (x * e - hxs);
  INSERT_WORDS(twopk, 0x3ff00000 + (k << 20), 0);
  e = (x * (e - c) - c);
  e -= hxs;
  if (k == -1) return 0.5 * (x - e) - 0.5;
  if (k == 1) {
    if (x < -0.25) return -2.0 * (e - (x + 0.5));
    return one + 2.0 * (x - e);
  }
  if (k <= -2 || k > 56) {
    y = one - (e - x);
    if (k == 1024) y = y * 2.0 * 0x1p1023;
    else y = y * twopk;
    return y - one;
  }
  t = one;
  if (k < 20) {
    SET_HIGH_WORD(t, 0x3ff00000 - (0x200000 >> k));
    y = t - (e - x);
    y = y * twopk;
  } else {
    SET_HIGH_WORD(t, ((0x3ff - k) << 20));
    y = x - (e + t);
    y += one;
    y = y * twopk;
  }
  return y;
}

double js_log1p(double x) {
  const double Lp1 = 6.666666666666735130e-01, Lp2 = 3.999999999940941908e-01,
    Lp3 = 2.857142874366239149e-01, Lp4 = 2.222219843214978396e-01, Lp5 = 1.818357216161805012e-01,
    Lp6 = 1.531383769920937332e-01, Lp7 = 1.479819860511658591e-01;
  double hfsq, f = 0, c = 0, s, z, R, u;
  int32_t k, hx, hu = 0, ax;
  GET_HIGH_WORD(hx, x);
  ax = hx & 0x7fffffff;
  k = 1;
  if (hx < 0x3FDA827A) {
    if (ax >= 0x3ff00000) {
      if (x == -1.0) return -INFINITY;
      return NAN;
    }
    if (ax < 0x3e200000) {
      if (two54 + x > 0.0 && ax < 0x3c900000) return x;
      return x - x * x * 0.5;
    }
    if (hx > 0 || hx <= ((int32_t)0xbfd2bec4)) { k = 0; f = x; hu = 1; }
  }
  if (hx >= 0x7ff00000) return x + x;
  if (k != 0) {
    if (hx < 0x43400000) {
      u = 1.0 + x;
      GET_HIGH_WORD(hu, u);
      k = (hu >> 20) - 1023;
      c = (k > 0) ? 1.0 - (u - x) : x - (u - 1.0);
      c /= u;
    } else {
      u = x;
      GET_HIGH_WORD(hu, u);
      k = (hu >> 20) - 1023;
      c = 0;
    }
    hu &= 0x000fffff;
    if (hu < 0x6a09e) {
      SET_HIGH_WORD(u, hu | 0x3ff00000);
    } else {
      k += 1;
      SET_HIGH_WORD(u, hu | 0x3fe00000);
      hu = (0x00100000 - hu) >> 2;
    }
    f = u - 1.0;
  }
  hfsq = 0.5 * f * f;
  if (hu == 0) {
    if (f == 0.0) {
      if (k == 0) return 0.0;
      c += k * ln2_lo;
      return k * ln2_hi + c;
    }
    R = hfsq * (1.0 - 0.66666666666666666 * f);
    if (k == 0) return f - R;
    return k * ln2_hi - ((R - (k * ln2_lo + c)) - f);
  }
  s = f / (2.0 + f);
  z = s * s;
  R = z * (Lp1 + z * (Lp2 + z * (Lp3 + z * (Lp4 + z * (Lp5 + z * (Lp6 + z * Lp7))))));
  if (k == 0) return f - (hfsq - s * (hfsq + R));
  return k * ln2_hi - ((hfsq - (s * (hfsq + R) + (k * ln2_lo + c))) - f);
}

// ---- hyperbolic ------------------------------------------------------------------------------

double js_sinh(double x) {
  const double KSINH_OVERFLOW = 710.4758600739439, TWO_M28 = 3.725290298461914e-9,
    LOG_MAXD = 709.7822265625, shuge = 1.0e307;
  double h = (x < 0) ? -0.5 : 0.5;
  double ax = fabs(x);
  if (ax < 22) {
    if (ax < TWO_M28) return x;
    double t = js_expm1(ax);
    if (ax < 1) return h * (2.0 * t - t * t / (t + 1.0));
    return h * (t + t / (t + 1.0));
  }
  if (ax < LOG_MAXD) return h * js_exp(ax);
  if (ax <= KSINH_OVERFLOW) {
    double w = js_exp(0.5 * ax);
    double t = h * w;
    return t * w;
  }
  return x * shuge;
}

double js_cosh(double x) {
  const double KCOSH_OVERFLOW = 710.4758600739439, one = 1.0, half = 0.5, huge = 1.0e+300;
  int32_t ix;
  GET_HIGH_WORD(ix, x);
  ix &= 0x7fffffff;
  if (ix < 0x3fd62e43) {
    double t = js_expm1(fabs(x));
    double w = one + t;
    if (ix < 0x3c800000) return w;
    return one + (t * t) / (w + w);
  }
  if (ix < 0x40360000) {
    double t = js_exp(fabs(x));
    return half * t + half / t;
  }
  if (ix < 0x40862e42) return half * js_exp(fabs(x));
  if (fabs(x) <= KCOSH_OVERFLOW) {
    double w = js_exp(half * fabs(x));
    double t = half * w;
    return t * w;
  }
  if (ix >= 0x7ff00000) return x * x;
  return huge * huge;
}

double js_tanh(double x) {
  const double tiny = 1.0e-300, one = 1.0, two = 2.0, huge = 1.0e300;
  double t, z;
  int32_t jx, ix;
  GET_HIGH_WORD(jx, x);
  ix = jx & 0x7fffffff;
  if (ix >= 0x7ff00000) {
    if (jx >= 0) return one / x + one;
    return one / x - one;
  }
  if (ix < 0x40360000) {
    if (ix < 0x3e300000) { if (huge + x > one) return x; }
    if (ix >= 0x3ff00000) {
      t = js_expm1(two * fabs(x));
      z = one - two / (t + two);
    } else {
      t = js_expm1(-two * fabs(x));
      z = -t / (t + two);
    }
  } else {
    z = one - tiny;
  }
  return (jx >= 0) ? z : -z;
}

// ---- roots and powers ------------------------------------------------------------------------

double js_cbrt(double x) {
  const uint32_t B1 = 715094163, B2 = 696219795;
  const double P0 = 1.87595182427177009643, P1 = -1.88497979543377169875,
    P2 = 1.621429720105354466140, P3 = -0.758397934778766047437, P4 = 0.145996192886612446982;
  int32_t hx;
  DW u;
  double r, s, t = 0.0, w;
  uint32_t sign, high, low;
  EXTRACT_WORDS(hx, low, x);
  sign = (uint32_t)hx & 0x80000000;
  hx ^= (int32_t)sign;
  if (hx >= 0x7ff00000) return x + x;
  if (hx < 0x00100000) {
    if ((hx | (int32_t)low) == 0) return x;
    SET_HIGH_WORD(t, 0x43500000);
    t *= x;
    GET_HIGH_WORD(high, t);
    INSERT_WORDS(t, sign | ((high & 0x7fffffff) / 3 + B2), 0);
  } else {
    INSERT_WORDS(t, sign | ((uint32_t)hx / 3 + B1), 0);
  }
  r = (t * t) * (t / x);
  t = t * ((P0 + r * (P1 + r * P2)) + ((r * r) * r) * (P3 + r * P4));
  u.f = t;
  u.u = (u.u + 0x80000000) & 0xffffffffc0000000ULL;
  t = u.f;
  s = t * t;
  r = x / s;
  w = t + t;
  r = (r - t) / (w + r);
  t = t + t * r;
  return t;
}

double js_pow(double x, double y) {
  static const double bp[] = { 1.0, 1.5 }, dp_h[] = { 0.0, 5.84962487220764160156e-01 },
    dp_l[] = { 0.0, 1.35003920212974897128e-08 };
  const double zero = 0.0, half = 0.5, qrtr = 0.25, thrd = 3.3333333333333331e-01, one = 1.0,
    two = 2.0, two53 = 9007199254740992.0, huge = 1.0e300, tiny = 1.0e-300,
    L1 = 5.99999999999994648725e-01, L2 = 4.28571428578550184252e-01, L3 = 3.33333329818377432918e-01,
    L4 = 2.72728123808534006489e-01, L5 = 2.30660745775561754067e-01, L6 = 2.06975017800338417784e-01,
    P1 = 1.66666666666666019037e-01, P2 = -2.77777777770155933842e-03, P3 = 6.61375632143793436117e-05,
    P4 = -1.65339022054652515390e-06, P5 = 4.13813679705723846039e-08,
    lg2 = 6.93147180559945286227e-01, lg2_h = 6.93147182464599609375e-01, lg2_l = -1.90465429995776804525e-09,
    ovt = 8.0085662595372944372e-017, cp = 9.61796693925975554329e-01, cp_h = 9.61796700954437255859e-01,
    cp_l = -7.02846165095275826516e-09, ivln2 = 1.44269504088896338700e+00,
    ivln2_h = 1.44269502162933349609e+00, ivln2_l = 1.92596299112661746887e-08;
  double z, ax, z_h, z_l, p_h, p_l;
  double y1, t1, t2, r, s, t, u, v, w;
  int32_t i, j, k, yisint, n;
  int32_t hx, hy, ix, iy;
  uint32_t lx, ly;

  EXTRACT_WORDS(hx, lx, x);
  EXTRACT_WORDS(hy, ly, y);
  ix = hx & 0x7fffffff;
  iy = hy & 0x7fffffff;
  if ((iy | ly) == 0) return one;
  if (ix > 0x7ff00000 || ((ix == 0x7ff00000) && (lx != 0)) || iy > 0x7ff00000 || ((iy == 0x7ff00000) && (ly != 0))) return x + y;
  yisint = 0;
  if (hx < 0) {
    if (iy >= 0x43400000) yisint = 2;
    else if (iy >= 0x3ff00000) {
      k = (iy >> 20) - 0x3ff;
      if (k > 20) {
        j = (int32_t)(ly >> (52 - k));
        if (((uint32_t)j << (52 - k)) == ly) yisint = 2 - (j & 1);
      } else if (ly == 0) {
        j = iy >> (20 - k);
        if ((j << (20 - k)) == iy) yisint = 2 - (j & 1);
      }
    }
  }
  if (ly == 0) {
    if (iy == 0x7ff00000) {
      if (((ix - 0x3ff00000) | lx) == 0) return y - y;   // (±1)^±∞ is NaN in JavaScript
      else if (ix >= 0x3ff00000) return (hy >= 0) ? y : zero;
      else return (hy < 0) ? -y : zero;
    }
    if (iy == 0x3ff00000) {
      if (hy < 0) return one / x;
      return x;
    }
    if (hy == 0x40000000) return x * x;
    if (hy == 0x3fe00000) {
      if (hx >= 0) return sqrt(x);
    }
  }
  ax = fabs(x);
  if (lx == 0) {
    if (ix == 0x7ff00000 || ix == 0 || ix == 0x3ff00000) {
      z = ax;
      if (hy < 0) z = one / z;
      if (hx < 0) {
        if (((ix - 0x3ff00000) | yisint) == 0) z = (z - z) / (z - z);
        else if (yisint == 1) z = -z;
      }
      return z;
    }
  }
  n = (hx >> 31) + 1;
  if ((n | yisint) == 0) return (x - x) / (x - x);
  s = one;
  if ((n | (yisint - 1)) == 0) s = -one;
  if (iy > 0x41e00000) {
    if (iy > 0x43f00000) {
      if (ix <= 0x3fefffff) return (hy < 0) ? huge * huge : tiny * tiny;
      if (ix >= 0x3ff00000) return (hy > 0) ? huge * huge : tiny * tiny;
    }
    if (ix < 0x3fefffff) return (hy < 0) ? s * huge * huge : s * tiny * tiny;
    if (ix > 0x3ff00000) return (hy > 0) ? s * huge * huge : s * tiny * tiny;
    t = ax - one;
    w = (t * t) * (half - t * (thrd - t * qrtr));
    u = ivln2_h * t;
    v = t * ivln2_l - w * ivln2;
    t1 = u + v;
    SET_LOW_WORD(t1, 0);
    t2 = v - (t1 - u);
  } else {
    double ss, s2, s_h, s_l, t_h, t_l;
    n = 0;
    if (ix < 0x00100000) { ax *= two53; n -= 53; GET_HIGH_WORD(ix, ax); }
    n += ((ix) >> 20) - 0x3ff;
    j = ix & 0x000fffff;
    ix = j | 0x3ff00000;
    if (j <= 0x3988E) k = 0;
    else if (j < 0xBB67A) k = 1;
    else { k = 0; n += 1; ix -= 0x00100000; }
    SET_HIGH_WORD(ax, ix);
    u = ax - bp[k];
    v = one / (ax + bp[k]);
    ss = u * v;
    s_h = ss;
    SET_LOW_WORD(s_h, 0);
    t_h = zero;
    SET_HIGH_WORD(t_h, ((ix >> 1) | 0x20000000) + 0x00080000 + (k << 18));
    t_l = ax - (t_h - bp[k]);
    s_l = v * ((u - s_h * t_h) - s_h * t_l);
    s2 = ss * ss;
    r = s2 * s2 * (L1 + s2 * (L2 + s2 * (L3 + s2 * (L4 + s2 * (L5 + s2 * L6)))));
    r += s_l * (s_h + ss);
    s2 = s_h * s_h;
    t_h = 3.0 + s2 + r;
    SET_LOW_WORD(t_h, 0);
    t_l = r - ((t_h - 3.0) - s2);
    u = s_h * t_h;
    v = s_l * t_h + t_l * ss;
    p_h = u + v;
    SET_LOW_WORD(p_h, 0);
    p_l = v - (p_h - u);
    z_h = cp_h * p_h;
    z_l = cp_l * p_h + p_l * cp + dp_l[k];
    t = n;
    t1 = (((z_h + z_l) + dp_h[k]) + t);
    SET_LOW_WORD(t1, 0);
    t2 = z_l - (((t1 - t) - dp_h[k]) - z_h);
  }
  y1 = y;
  SET_LOW_WORD(y1, 0);
  p_l = (y - y1) * t1 + y * t2;
  p_h = y1 * t1;
  z = p_l + p_h;
  uint32_t uj;
  EXTRACT_WORDS(j, uj, z);
  i = (int32_t)uj;
  if (j >= 0x40900000) {
    if (((j - 0x40900000) | i) != 0) return s * huge * huge;
    if (p_l + ovt > z - p_h) return s * huge * huge;
  } else if ((j & 0x7fffffff) >= 0x4090cc00) {
    if (((j - (int32_t)0xc090cc00) | i) != 0) return s * tiny * tiny;
    if (p_l <= z - p_h) return s * tiny * tiny;
  }
  i = j & 0x7fffffff;
  k = (i >> 20) - 0x3ff;
  n = 0;
  if (i > 0x3fe00000) {
    n = j + (0x00100000 >> (k + 1));
    k = ((n & 0x7fffffff) >> 20) - 0x3ff;
    t = zero;
    SET_HIGH_WORD(t, n & ~(0x000fffff >> k));
    n = ((n & 0x000fffff) | 0x00100000) >> (20 - k);
    if (j < 0) n = -n;
    p_h -= t;
  }
  t = p_l + p_h;
  SET_LOW_WORD(t, 0);
  u = t * lg2_h;
  v = (p_l - (t - p_h)) * lg2 + t * lg2_l;
  z = u + v;
  w = v - (z - u);
  t = z * z;
  t1 = z - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
  // V8's form of fdlibm's last step (it divides by the whole correction), kept for bit-parity.
  r = (z * t1) / ((t1 - two) - (w + z * w));
  z = one - (r - z);
  GET_HIGH_WORD(j, z);
  j += (n << 20);
  if ((j >> 20) <= 0) z = scalbn(z, n);
  else SET_HIGH_WORD(z, j);
  return s * z;
}
