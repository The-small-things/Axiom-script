// jsmath.h — route libm's transcendental functions to jsmath.c, V8's algorithms, so every
// Math result is the one JavaScript produces. Include after <math.h>.
#ifndef AXIOM_JSMATH_H
#define AXIOM_JSMATH_H
double js_sin(double), js_cos(double), js_tan(double), js_asin(double), js_acos(double),
  js_atan(double), js_atan2(double, double), js_exp(double), js_log(double), js_log2(double),
  js_log10(double), js_expm1(double), js_log1p(double), js_sinh(double), js_cosh(double),
  js_tanh(double), js_cbrt(double), js_pow(double, double);
#define sin js_sin
#define cos js_cos
#define tan js_tan
#define asin js_asin
#define acos js_acos
#define atan js_atan
#define atan2 js_atan2
#define exp js_exp
#define log js_log
#define log2 js_log2
#define log10 js_log10
#define expm1 js_expm1
#define log1p js_log1p
#define sinh js_sinh
#define cosh js_cosh
#define tanh js_tanh
#define cbrt js_cbrt
#define pow js_pow
#endif
