// engine.c — the engine half of AxiomScript: entities, frame blocks, physics, collisions,
// events, tweens, queries and actions.
//
// This is a port of the reference engine in interpreter.js, not a reinterpretation of it. Every
// rule below has a counterpart there (named in the comments), including the ones that look
// arbitrary — the order a broadcast reaches entities, when a `&tick` event is delivered, how a
// timer reads in arithmetic — because a simulation that runs on both runtimes has to produce
// the same numbers on both. `difftest.sh` checks exactly that: it steps the same program in
// both runtimes and compares the final state field by field.
//
// Two places needed more than a transliteration:
//   * Math.hypot. V8 computes it with a scaled, compensated sum, which differs from C's hypot()
//     in the last bit often enough to make a long simulation drift. `js_hypot` reproduces V8.
//   * Aliasing. A Vec3 in the reference is a mutable object: `pose.pos.y = 0` changes the vector
//     every holder of it sees. Vectors here are refcounted heap objects for the same reason.

#include "axiom.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "jsmath.h"
#include <stdarg.h>

// ---------------------------------------------------------------------------------------------
// Interned names used on hot paths
// ---------------------------------------------------------------------------------------------

static AxStr *K_pose, *K_wpose, *K_pos, *K_vel, *K_rot, *K_scl, *K_x, *K_y, *K_z, *K_w,
  *K_mag, *K_norm, *K_angle, *K_xy, *K_xz, *K_yz, *K_euler, *K_conj, *K_normalized, *K_inv, *K_T,
  *K_hit, *K_mass, *K_drag, *K_parent, *K_position, *K_velocity, *K_facing, *K_nosave,
  *K_collision_layer, *K_collision_mask, *K_physics, *K_tick, *K_render, *K_on, *K_Collide,
  *K_other, *K_other_entity, *K_normal, *K_depth, *K_source, *K_self, *K_move, *K_jump, *K_fire,
  *K_aim, *K_remaining, *K_total, *K_unit, *K_kind, *K_params, *K_d, *K_hud, *K_name, *K_path,
  *K_Body3D, *K_NavMesh3D, *K_sphere, *K_box, *K_capsule, *K_mesh, *K_to, *K_lerp, *K_clamp,
  *K_reflect, *K_rotate, *K_slerp, *K_toMat4, *K_observe, *K_tagName, *K_len, *K_at;

static void init_keys(void) {
  if (K_pose) return;
#define K(v, s) v = ax_internz(s)
  K(K_pose, "pose"); K(K_wpose, "wpose"); K(K_pos, "pos"); K(K_vel, "vel"); K(K_rot, "rot");
  K(K_scl, "scl"); K(K_x, "x"); K(K_y, "y"); K(K_z, "z"); K(K_w, "w"); K(K_mag, "mag");
  K(K_norm, "norm"); K(K_angle, "angle"); K(K_xy, "xy"); K(K_xz, "xz"); K(K_yz, "yz");
  K(K_euler, "euler"); K(K_conj, "conj"); K(K_normalized, "normalized"); K(K_inv, "inv");
  K(K_T, "T"); K(K_hit, "hit"); K(K_mass, "mass"); K(K_drag, "drag"); K(K_parent, "parent");
  K(K_position, "position"); K(K_velocity, "velocity"); K(K_facing, "facing");
  K(K_nosave, "nosave"); K(K_collision_layer, "collision_layer");
  K(K_collision_mask, "collision_mask"); K(K_physics, "physics"); K(K_tick, "tick");
  K(K_render, "render"); K(K_on, "on"); K(K_Collide, "Collide"); K(K_other, "other");
  K(K_other_entity, "other_entity"); K(K_normal, "normal"); K(K_depth, "depth");
  K(K_source, "source"); K(K_self, "self"); K(K_move, "move"); K(K_jump, "jump");
  K(K_fire, "fire"); K(K_aim, "aim"); K(K_remaining, "remaining"); K(K_total, "total");
  K(K_unit, "unit"); K(K_kind, "kind"); K(K_params, "params"); K(K_d, "d"); K(K_hud, "__hud");
  K(K_name, "name"); K(K_path, "path"); K(K_Body3D, "Body3D"); K(K_NavMesh3D, "NavMesh3D");
  K(K_sphere, "sphere"); K(K_box, "box"); K(K_capsule, "capsule"); K(K_mesh, "mesh");
  K(K_to, "to"); K(K_lerp, "lerp"); K(K_clamp, "clamp"); K(K_reflect, "reflect");
  K(K_rotate, "rotate"); K(K_slerp, "slerp"); K(K_toMat4, "toMat4"); K(K_observe, "observe");
  K(K_tagName, "_tagName"); K(K_len, "len"); K(K_at, "at");
#undef K
}

// ---------------------------------------------------------------------------------------------
// Math — the reference's Vec2/Vec3/Quat/Mat4/Transform, operation for operation
// ---------------------------------------------------------------------------------------------

// V8's Math.hypot: the largest magnitude scales the rest, and a Kahan-compensated sum of
// squares keeps the rounding identical to the reference's.
static double js_hypot(int n, const double *vals) {
  double abs_v[8];
  double max = 0;
  bool nan = false;
  for (int i = 0; i < n; i++) {
    if (isnan(vals[i])) { nan = true; abs_v[i] = 0; continue; }
    abs_v[i] = fabs(vals[i]);
    if (abs_v[i] > max) max = abs_v[i];
  }
  if (isinf(max)) return INFINITY;
  if (nan) return NAN;
  if (max == 0) return 0;
  double sum = 0, comp = 0;
  for (int i = 0; i < n; i++) {
    double r = abs_v[i] / max;
    double summand = r * r - comp;
    double prelim = sum + summand;
    comp = (prelim - sum) - summand;
    sum = prelim;
  }
  return sqrt(sum) * max;
}
static double hyp2(double a, double b) { double v[2] = { a, b }; return js_hypot(2, v); }
static double hyp3(double a, double b, double c) { double v[3] = { a, b, c }; return js_hypot(3, v); }
static double hyp4(double a, double b, double c, double d) { double v[4] = { a, b, c, d }; return js_hypot(4, v); }
double ax_js_hypot(int n, const double *vals) { return js_hypot(n, vals); }

typedef struct { double x, y, z; } V3;

static V3 v3of(AxValue v) {
  V3 r = { 0, 0, 0 };
  if (v.t == AX_VEC3 || v.t == AX_QUAT) { AxVec *p = ax_vecp(v); r.x = p->x; r.y = p->y; r.z = p->z; }
  else if (v.t == AX_VEC2) { AxVec *p = ax_vecp(v); r.x = p->x; r.y = p->y; r.z = NAN; }
  else { r.x = r.y = r.z = NAN; }
  return r;
}
static AxValue mk3(V3 a) { return ax_vec3(a.x, a.y, a.z); }
static V3 v3(double x, double y, double z) { V3 r = { x, y, z }; return r; }
static V3 v3add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static V3 v3sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static V3 v3mul(V3 a, double s) { return v3(a.x * s, a.y * s, a.z * s); }
static V3 v3div(V3 a, double s) { return v3(a.x / s, a.y / s, a.z / s); }
static double v3dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 v3cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static double v3mag(V3 a) { return hyp3(a.x, a.y, a.z); }
static V3 v3norm(V3 a) { double m = v3mag(a); return m < 1e-9 ? v3(0, 0, 0) : v3(a.x / m, a.y / m, a.z / m); }

typedef struct { double x, y, z, w; } Q;
static Q qof(AxValue v) {
  Q q = { 0, 0, 0, 1 };
  if (v.t == AX_QUAT) { AxVec *p = ax_vecp(v); q.x = p->x; q.y = p->y; q.z = p->z; q.w = p->w; }
  return q;
}
static AxValue mkq(Q q) { return ax_quat(q.x, q.y, q.z, q.w); }
static Q qmul(Q a, Q q) {
  Q r = {
    a.w * q.x + a.x * q.w + a.y * q.z - a.z * q.y,
    a.w * q.y - a.x * q.z + a.y * q.w + a.z * q.x,
    a.w * q.z + a.x * q.y - a.y * q.x + a.z * q.w,
    a.w * q.w - a.x * q.x - a.y * q.y - a.z * q.z,
  };
  return r;
}
static Q qconj(Q a) { Q r = { -a.x, -a.y, -a.z, a.w }; return r; }
static V3 qrot(Q q, V3 v) {
  Q qv = { v.x, v.y, v.z, 0 };
  Q r = qmul(qmul(q, qv), qconj(q));
  return v3(r.x, r.y, r.z);
}
static Q qnormalized(Q a) {
  double m = hyp4(a.x, a.y, a.z, a.w);
  if (m < 1e-9) { Q i = { 0, 0, 0, 1 }; return i; }
  Q r = { a.x / m, a.y / m, a.z / m, a.w / m };
  return r;
}
static Q q_axis_angle(V3 axis, double angle) {
  double ha = angle / 2, s = sin(ha);
  V3 a = v3norm(axis);
  Q r = { a.x * s, a.y * s, a.z * s, cos(ha) };
  return r;
}
static Q q_euler(double pitch, double yaw, double roll) {
  double cp = cos(pitch / 2), sp = sin(pitch / 2);
  double cy = cos(yaw / 2), sy = sin(yaw / 2);
  double cr = cos(roll / 2), sr = sin(roll / 2);
  Q r = {
    sp * cy * cr - cp * sy * sr,
    cp * sy * cr + sp * cy * sr,
    cp * cy * sr - sp * sy * cr,
    cp * cy * cr + sp * sy * sr,
  };
  return r;
}
static V3 q_to_euler(Q q) {
  double sinp = 2 * (q.w * q.x - q.y * q.z);
  double pitch = fabs(sinp) >= 1 ? (sinp > 0 ? 1 : (sinp < 0 ? -1 : 0)) * M_PI / 2 : asin(sinp);
  double yaw = atan2(2 * (q.w * q.y + q.z * q.x), 1 - 2 * (q.y * q.y + q.x * q.x));
  double roll = atan2(2 * (q.w * q.z + q.x * q.y), 1 - 2 * (q.z * q.z + q.x * q.x));
  return v3(pitch, yaw, roll);
}
static double clampd(double x, double lo, double hi) { return fmax(lo, fmin(hi, x)); }
// Math.max/Math.min propagate NaN, unlike fmax/fmin.
static double js_max(double a, double b) { if (isnan(a) || isnan(b)) return NAN; return a > b ? a : (b > a ? b : (a == 0 && signbit(a) ? b : a)); }
static double js_min(double a, double b) { if (isnan(a) || isnan(b)) return NAN; return a < b ? a : (b < a ? b : (a == 0 && !signbit(a) ? b : a)); }
static double js_clamp(double x, double lo, double hi) { return js_max(lo, js_min(hi, x)); }

static Q qslerp(Q a, Q q, double t) {
  double dot = a.x * q.x + a.y * q.y + a.z * q.z + a.w * q.w;
  Q q2 = q;
  if (dot < 0) { q2.x = -q.x; q2.y = -q.y; q2.z = -q.z; q2.w = -q.w; dot = -dot; }
  if (dot > 0.9995) {
    Q r = { a.x + t * (q2.x - a.x), a.y + t * (q2.y - a.y), a.z + t * (q2.z - a.z), a.w + t * (q2.w - a.w) };
    return qnormalized(r);
  }
  double theta = acos(js_clamp(dot, -1, 1));
  double sinT = sin(theta);
  double ka = sin((1 - t) * theta) / sinT, kb = sin(t * theta) / sinT;
  Q r = { ka * a.x + kb * q2.x, ka * a.y + kb * q2.y, ka * a.z + kb * q2.z, ka * a.w + kb * q2.w };
  return r;
}

static void m4_identity(double *d) { memset(d, 0, sizeof(double) * 16); d[0] = d[5] = d[10] = d[15] = 1; }
static void m4_mul(const double *a, const double *b, double *r) {
  for (int c = 0; c < 4; c++) for (int row = 0; row < 4; row++) {
    double s = 0;
    for (int k = 0; k < 4; k++) s += a[k * 4 + row] * b[c * 4 + k];
    r[c * 4 + row] = s;
  }
}
static V3 m4_mul_v3(const double *d, V3 v) {
  double w = d[3] * v.x + d[7] * v.y + d[11] * v.z + d[15];
  if (fabs(w) < 1e-9) return v3(0, 0, 0);
  return v3((d[0] * v.x + d[4] * v.y + d[8] * v.z + d[12]) / w,
            (d[1] * v.x + d[5] * v.y + d[9] * v.z + d[13]) / w,
            (d[2] * v.x + d[6] * v.y + d[10] * v.z + d[14]) / w);
}
static void m4_inv(const double *m, double *out) {
  double inv[16];
  inv[0] = m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];
  inv[4] = -m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
  inv[8] = m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];
  inv[12] = -m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
  inv[1] = -m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];
  inv[5] = m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10];
  inv[9] = -m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];
  inv[13] = m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
  inv[2] = m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];
  inv[6] = -m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
  inv[10] = m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];
  inv[14] = -m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
  inv[3] = -m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];
  inv[7] = m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
  inv[11] = -m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];
  inv[15] = m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];
  double det = m[0]*inv[0]+m[1]*inv[4]+m[2]*inv[8]+m[3]*inv[12];
  if (fabs(det) < 1e-9) { m4_identity(out); return; }
  det = 1 / det;
  for (int i = 0; i < 16; i++) out[i] = inv[i] * det;
}
static void m4_look_at(V3 eye, V3 target, V3 up, double *d) {
  V3 z = v3norm(v3sub(eye, target));
  V3 upv = up;
  double dot = fabs(upv.x * z.x + upv.y * z.y + upv.z * z.z);
  if (dot > 0.9999) {
    upv = v3(0, 0, 1);
    double dot2 = fabs(upv.x * z.x + upv.y * z.y + upv.z * z.z);
    if (dot2 > 0.9999) upv = v3(1, 0, 0);
  }
  V3 x = v3norm(v3cross(upv, z));
  V3 y = v3cross(z, x);
  memset(d, 0, sizeof(double) * 16);
  d[0] = x.x; d[4] = x.y; d[8] = x.z;  d[12] = -v3dot(x, eye);
  d[1] = y.x; d[5] = y.y; d[9] = y.z;  d[13] = -v3dot(y, eye);
  d[2] = z.x; d[6] = z.y; d[10] = z.z; d[14] = -v3dot(z, eye);
  d[15] = 1;
}
static void xform_to_m4(AxXform *t, double *out) {
  double m[16];
  m4_identity(m);
  V3 s = v3of(t->scl);
  m[0] = s.x; m[5] = s.y; m[10] = s.z;
  Q q = qof(t->rot);
  double xx = q.x*q.x, yy = q.y*q.y, zz = q.z*q.z, xy = q.x*q.y, xz = q.x*q.z, yz = q.y*q.z, wx = q.w*q.x, wy = q.w*q.y, wz = q.w*q.z;
  double rm[16];
  m4_identity(rm);
  rm[0]=1-2*(yy+zz); rm[4]=2*(xy-wz);   rm[8]=2*(xz+wy);
  rm[1]=2*(xy+wz);   rm[5]=1-2*(xx+zz); rm[9]=2*(yz-wx);
  rm[2]=2*(xz-wy);   rm[6]=2*(yz+wx);   rm[10]=1-2*(xx+yy);
  m4_mul(rm, m, out);
  V3 p = v3of(t->pos);
  out[12] = p.x; out[13] = p.y; out[14] = p.z;
}

static AxValue clone_value(AxValue v) {
  switch (v.t) {
    case AX_VEC2: return ax_vec2(ax_vecp(v)->x, ax_vecp(v)->y);
    case AX_VEC3: return ax_vec3(ax_vecp(v)->x, ax_vecp(v)->y, ax_vecp(v)->z);
    case AX_QUAT: return ax_quat(ax_vecp(v)->x, ax_vecp(v)->y, ax_vecp(v)->z, ax_vecp(v)->w);
    case AX_XFORM: {
      AxXform *s = (AxXform *)v.o;
      AxValue out = ax_xform_new();
      AxXform *t = (AxXform *)out.o;
      ax_release(t->pos); t->pos = clone_value(s->pos);
      ax_release(t->rot); t->rot = clone_value(s->rot);
      ax_release(t->scl); t->scl = clone_value(s->scl);
      ax_release(t->vel); t->vel = clone_value(s->vel);
      return out;
    }
    default: return ax_copy(v);
  }
}

// ---------------------------------------------------------------------------------------------
// World state
// ---------------------------------------------------------------------------------------------

typedef struct {
  AxStr *name;          // event name
  AxDict *payload;
  int mode;             // 0 global, 1 to, 2 within
  AxStr *target;
  double radius;
  V3 origin;
} PendingEvent;

typedef struct {
  AxValue target;       // entity or object
  AxStr *path[8];
  int npath;
  AxValue start;        // captured on the first tick (AX_NULL until then)
  bool started;
  AxValue end;
  double duration, elapsed;
  char ease[16];
  bool done;
} Tween;

typedef struct {
  char code[32];
  char severity[16];     // "fatal" for faults; !load's schema check can be "advisory"
  char title[64];
  char human[512];
  AxStr *entity, *block;
  int line, col;
  char snippet[256];
  bool has_snippet;
} Diag;

typedef struct {
  AxStr *entity, *block;
  AxArr *args;
  double t;
} DebugEntry;

#define DEBUG_RING 256

typedef struct AxWorld {
  AxEntity **ents;
  int nents, capents;
  AxDict *tags;               // tag → entity (the latest entity registered under the name)
  AxDict *resources;          // name → descriptor dict
  AxNode **events; int nevents;
  AxNode **mixins; int nmixins;
  AxNode **materials; int nmaterials;
  // Composed declarations, for !spawn.
  struct { AxStr *name; AxStr *base; AxNode **members; int nmembers; } *decls;
  int ndecls;
  PendingEvent *pending; int npending, cappending;
  Tween *tweens; int ntweens, captweens;
  AxArr *log;                 // messages (the JSON `log`)
  Diag *diags; int ndiags, capdiags;
  AxDict *tick_acc;           // "Decl::blockfreq" → accumulator
  double physics_acc, sim_time;
  int spawn_counter;
  AxDict *input;              // {move, jump, fire, aim}
  AxDict *channels;           // ^emit path → latest value
  AxArr *draw_list;           // !mesh commands of the current frame
  AxDict *save_slots;
  DebugEntry ring[DEBUG_RING];
  int ring_head, ring_count;
  const char *source;
  const char *version;
  bool suppress_console;
  struct { AxStr *name; const AxGlbPrim *prim; } *meshes;   // #Mesh3D resources loaded from .glb
  int nmeshes;
} AxWorld;

static AxWorld *W(AxVM *vm) { return vm->world; }

static AxValue ent_val(AxEntity *e) { AxValue v; v.t = AX_ENTITY; v.o = (AxObj *)e; ax_retain(v); return v; }
static AxValue str_val(AxStr *s) { ax_retain(ax_strv(s)); return ax_strv(s); }

static AxXform *ent_pose(AxEntity *e) {
  AxValue p;
  if (!ax_dict_get(e->locals, K_pose, &p)) return NULL;
  AxXform *x = p.t == AX_XFORM ? (AxXform *)p.o : NULL;
  ax_release(p);   // the dict keeps it alive
  return x;
}

static bool field_get(AxEntity *e, AxStr *name, AxValue *out) { return ax_dict_get(e->fields, name, out); }
static double field_num_or(AxEntity *e, AxStr *name, double dflt) {
  // `fields.get(name) || dflt` — 0, null, false and NaN all fall back.
  AxValue v;
  if (!field_get(e, name, &v)) return dflt;
  double d = dflt;
  if (v.t == AX_NUM) d = (v.num == 0 || isnan(v.num)) ? dflt : v.num;
  else if (v.t == AX_BOOL) d = v.b ? 1 : dflt;
  else if (v.t == AX_TIMER) d = ((AxTimer *)v.o)->remaining ? ((AxTimer *)v.o)->remaining : dflt;
  else if (ax_truthy(v)) d = NAN;
  ax_release(v);
  return d;
}

static AxEntity *resolve_tag(AxWorld *w, AxStr *name) {
  AxValue v;
  if (!w || !ax_dict_get(w->tags, name, &v)) return NULL;
  AxEntity *e = (AxEntity *)v.o;
  ax_release(v);
  return e->pending_remove ? NULL : e;
}

AxValue ax_engine_resolve_tag(AxVM *vm, AxStr *tag) {
  init_keys();
  AxEntity *e = resolve_tag(W(vm), tag);
  return e ? ent_val(e) : ax_null();
}

AxValue ax_engine_input(AxVM *vm) {
  if (!W(vm)) return ax_null();
  AxValue v; v.t = AX_DICT; v.o = (AxObj *)W(vm)->input;
  return ax_copy(v);
}

// Transform.worldPose: compose with the `~parent` chain (a tag name or an entity).
static AxValue world_pose(AxWorld *w, AxXform *x, int depth) {
  AxValue self; self.t = AX_XFORM; self.o = (AxObj *)x;
  if (!x->entity || depth > 32) return ax_copy(self);
  AxValue pref;
  if (!field_get(x->entity, K_parent, &pref)) return ax_copy(self);
  AxEntity *pe = NULL;
  if (pref.t == AX_STR) pe = resolve_tag(w, (AxStr *)pref.o);
  else if (pref.t == AX_ENTITY) pe = (AxEntity *)pref.o;
  ax_release(pref);
  if (!pe) return ax_copy(self);
  AxXform *pp0 = ent_pose(pe);
  AxValue ppv;
  if (pp0) { pp0->entity = pe; ppv = world_pose(w, pp0, depth + 1); }
  else ppv = ax_xform_new();
  AxXform *pp = (AxXform *)ppv.o;
  AxValue out = ax_xform_new();
  AxXform *r = (AxXform *)out.o;
  V3 rotated = qrot(qof(x->rot), v3of(x->pos));
  V3 ps = v3of(pp->scl);
  ax_release(r->pos); r->pos = mk3(v3add(v3of(pp->pos), v3(rotated.x * ps.x, rotated.y * ps.y, rotated.z * ps.z)));
  ax_release(r->rot); r->rot = mkq(qnormalized(qmul(qof(pp->rot), qof(x->rot))));
  V3 s = v3of(x->scl);
  ax_release(r->scl); r->scl = ax_vec3(ps.x * s.x, ps.y * s.y, ps.z * s.z);
  ax_release(r->vel); r->vel = mk3(v3add(v3of(pp->vel), v3of(x->vel)));
  ax_release(ppv);
  return out;
}

// EntityInstance.get — `pose` and `wpose` first, then fields, then locals, then the pose
// shorthands (pos/vel/rot/scl).
bool ax_entity_get(AxEntity *e, AxStr *name, AxValue *out) {
  init_keys();
  if (name == K_pose) {
    if (ax_dict_get(e->locals, K_pose, out)) return true;
    *out = ax_null();
    return true;
  }
  if (name == K_wpose) {
    AxXform *p = ent_pose(e);
    *out = p ? world_pose(e->world, p, 0) : ax_xform_new();
    return true;
  }
  if (ax_dict_get(e->fields, name, out)) return true;
  if (ax_dict_get(e->locals, name, out)) return true;
  AxXform *p = ent_pose(e);
  if (p) {
    if (name == K_pos) { *out = ax_copy(p->pos); return true; }
    if (name == K_vel) { *out = ax_copy(p->vel); return true; }
    if (name == K_rot) { *out = ax_copy(p->rot); return true; }
    if (name == K_scl) { *out = ax_copy(p->scl); return true; }
  }
  return false;
}

static void xform_set(AxXform *x, AxStr *name, AxValue v) {
  AxValue *slot = name == K_pos ? &x->pos : name == K_rot ? &x->rot : name == K_scl ? &x->scl : &x->vel;
  ax_release(*slot);
  *slot = v;
}

static AxValue make_timer_from(AxValue v, const char *unit) {
  double rem = v.t == AX_TIMER ? ((AxTimer *)v.o)->remaining : ax_to_num(v);
  if (v.t == AX_NULL) rem = 0;
  return ax_timer_new(rem, unit);
}

// EntityInstance.set — pos/rot/scl/vel route into the pose (unless a field of that name was
// declared); assigning a timer field restarts it; anything else lands in locals.
void ax_entity_set(AxEntity *e, AxStr *name, AxValue v) {
  init_keys();
  bool is_field = ax_dict_has(e->fields, name);
  if (!is_field && (name == K_pos || name == K_rot || name == K_scl || name == K_vel)) {
    AxXform *p = ent_pose(e);
    if (p) { xform_set(p, name, v); return; }
  }
  if (is_field) {
    AxValue cur;
    ax_dict_get(e->fields, name, &cur);
    if (cur.t == AX_TIMER) {
      AxValue t = make_timer_from(v, ((AxTimer *)cur.o)->unit);
      ax_release(cur);
      ax_release(v);
      ax_dict_set(e->fields, name, t);
      return;
    }
    ax_release(cur);
    if (v.t == AX_TIMER) {
      AxValue t = make_timer_from(v, ((AxTimer *)v.o)->unit);
      ax_release(v);
      ax_dict_set(e->fields, name, t);
      return;
    }
    ax_dict_set(e->fields, name, v);
    return;
  }
  ax_dict_set(e->locals, name, v);
}

// ---------------------------------------------------------------------------------------------
// Rendering values as text
// ---------------------------------------------------------------------------------------------

static void app(char **buf, size_t *len, size_t *cap, const char *s) { ax_str_append(buf, len, cap, s, strlen(s)); }
static void appnum(char **buf, size_t *len, size_t *cap, double d) {
  char tmp[64];
  ax_fmt_num(d, tmp, sizeof tmp);
  app(buf, len, cap, tmp);
}

// The f-string rendering of an engine value (interpreter.js stringifyFStringVal).
void ax_render_engine(AxValue v, char **buf, size_t *len, size_t *cap) {
  switch (v.t) {
    case AX_VEC2: case AX_VEC3: case AX_QUAT: {
      AxVec *p = ax_vecp(v);
      app(buf, len, cap, "(");
      appnum(buf, len, cap, p->x); app(buf, len, cap, ",");
      appnum(buf, len, cap, p->y);
      if (v.t != AX_VEC2) { app(buf, len, cap, ","); appnum(buf, len, cap, p->z); }
      if (v.t == AX_QUAT) { app(buf, len, cap, ","); appnum(buf, len, cap, p->w); }
      app(buf, len, cap, ")");
      return;
    }
    case AX_ENTITY: app(buf, len, cap, ((AxEntity *)v.o)->name->data); return;
    case AX_TIMER: {
      char *j = NULL;
      ax_json_write(v, 0, &j);
      app(buf, len, cap, j);
      free(j);
      return;
    }
    case AX_SHAPE: app(buf, len, cap, "ColliderShape"); return;
    case AX_XFORM: app(buf, len, cap, "Transform"); return;
    case AX_MAT4: app(buf, len, cap, "Mat4"); return;
    case AX_HOST: {
      static const char *names[] = { "Pool", "BVec", "BMap", "Distribution" };
      AxHost *h = (AxHost *)v.o;
      app(buf, len, cap, h->kind >= 0 && h->kind < 4 ? names[h->kind] : "Object");
      return;
    }
    default: return;
  }
}

// ---------------------------------------------------------------------------------------------
// JSON — two flavours, both matching the reference byte for byte:
//   * JSON.stringify of a value (str(), json_stringify(), timers in f-strings), and
//   * the `--sim --json` state dump (main.js serializeForJson), where vectors become arrays.
// ---------------------------------------------------------------------------------------------

typedef struct { char *buf; size_t len, cap; bool bigint; } JB;   // bigint: JSON.stringify would throw
static void jb(JB *b, const char *s) { ax_str_append(&b->buf, &b->len, &b->cap, s, strlen(s)); }
static void jbn(JB *b, const char *s, size_t n) { ax_str_append(&b->buf, &b->len, &b->cap, s, n); }
static void jnum(JB *b, double d) {
  if (isnan(d) || isinf(d)) { jb(b, "null"); return; }
  char tmp[64];
  ax_fmt_num(d, tmp, sizeof tmp);
  jb(b, tmp);
}
static void jstr(JB *b, const char *s, size_t n) {
  jb(b, "\"");
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    switch (c) {
      case '"': jb(b, "\\\""); break;
      case '\\': jb(b, "\\\\"); break;
      case '\n': jb(b, "\\n"); break;
      case '\r': jb(b, "\\r"); break;
      case '\t': jb(b, "\\t"); break;
      case '\b': jb(b, "\\b"); break;
      case '\f': jb(b, "\\f"); break;
      default:
        if (c < 0x20) { char u[8]; snprintf(u, sizeof u, "\\u%04x", c); jb(b, u); }
        else jbn(b, (const char *)&c, 1);
    }
  }
  jb(b, "\"");
}
static void jpad(JB *b, int indent, int depth) {
  if (!indent) return;
  jb(b, "\n");
  for (int i = 0; i < indent * depth; i++) jb(b, " ");
}

// A small ordered key/value list used to describe an object before writing it.
typedef struct { const char *k; AxValue v; bool owned; } KV;

static void json_value(JB *b, AxValue v, int indent, int depth, bool sim);

static void json_object(JB *b, KV *kv, int n, int indent, int depth, bool sim) {
  // JSON.stringify omits undefined members; callers pass only defined ones.
  if (!n) { jb(b, "{}"); return; }
  jb(b, "{");
  for (int i = 0; i < n; i++) {
    if (i) jb(b, ",");
    jpad(b, indent, depth + 1);
    jstr(b, kv[i].k, strlen(kv[i].k));
    jb(b, indent ? ": " : ":");
    json_value(b, kv[i].v, indent, depth + 1, sim);
  }
  jpad(b, indent, depth);
  jb(b, "}");
}

static void json_numarr(JB *b, const double *xs, int n, int indent, int depth) {
  jb(b, "[");
  for (int i = 0; i < n; i++) {
    if (i) jb(b, ",");
    jpad(b, indent, depth + 1);
    jnum(b, xs[i]);
  }
  jpad(b, indent, depth);
  jb(b, "]");
}

static void json_value(JB *b, AxValue v, int indent, int depth, bool sim) {
  if (depth > 64) { jb(b, "null"); return; }
  switch (v.t) {
    case AX_NULL: jb(b, "null"); return;
    case AX_BOOL: jb(b, v.b ? "true" : "false"); return;
    case AX_NUM: jnum(b, v.num); return;
    case AX_STR: { AxStr *s = (AxStr *)v.o; jstr(b, s->data, s->len); return; }
    case AX_ATOM: {
      AxStr *s = (AxStr *)v.o;
      if (sim) { KV kv[1] = { { "atom", ax_strv(s), false } }; json_object(b, kv, 1, indent, depth, sim); }
      else jstr(b, s->data, s->len);   // an atom serializes as its name
      return;
    }
    case AX_ARR: {
      AxArr *a = (AxArr *)v.o;
      if (!a->len) { jb(b, "[]"); return; }
      jb(b, "[");
      for (uint32_t i = 0; i < a->len; i++) {
        if (i) jb(b, ",");
        jpad(b, indent, depth + 1);
        json_value(b, a->items[i], indent, depth + 1, sim);
      }
      jpad(b, indent, depth);
      jb(b, "]");
      return;
    }
    case AX_DICT: {
      AxDict *d = (AxDict *)v.o;
      if (sim && ax_dict_has(d, K_hud)) {
        // {hud: {kind, pos: [x, y, z] | null}}
        AxValue kind, pos;
        if (!ax_dict_get(d, K_kind, &kind)) kind = ax_null();
        if (!ax_dict_get(d, K_pos, &pos)) pos = ax_null();
        JB inner = { 0 };
        (void)inner;
        jb(b, "{");
        jpad(b, indent, depth + 1);
        jb(b, indent ? "\"hud\": {" : "\"hud\":{");
        jpad(b, indent, depth + 2);
        jb(b, indent ? "\"kind\": " : "\"kind\":");
        json_value(b, kind, indent, depth + 2, sim);
        jb(b, ",");
        jpad(b, indent, depth + 2);
        jb(b, indent ? "\"pos\": " : "\"pos\":");
        if (pos.t == AX_VEC3 || pos.t == AX_VEC2) { V3 p = v3of(pos); double xs[3] = { p.x, p.y, p.z }; json_numarr(b, xs, 3, indent, depth + 2); }
        else jb(b, "null");
        jpad(b, indent, depth + 1);
        jb(b, "}");
        jpad(b, indent, depth);
        jb(b, "}");
        ax_release(kind); ax_release(pos);
        return;
      }
      int n = (int)d->live + (d->type_tag ? 1 : 0);
      if (!n) { jb(b, "{}"); return; }
      jb(b, "{");
      bool first = true;
      if (d->type_tag) {
        jpad(b, indent, depth + 1);
        jb(b, indent ? "\"__type\": " : "\"__type\":");
        jstr(b, d->type_tag->data, d->type_tag->len);
        first = false;
      }
      for (uint32_t i = 0; i < d->len; i++) {
        if (d->entries[i].dead) continue;
        // JSON.stringify drops functions held in an object.
        if (!sim && d->entries[i].val.t == AX_FN) continue;
        if (!first) jb(b, ",");
        first = false;
        jpad(b, indent, depth + 1);
        jstr(b, d->entries[i].key->data, d->entries[i].key->len);
        jb(b, indent ? ": " : ":");
        json_value(b, d->entries[i].val, indent, depth + 1, sim);
      }
      jpad(b, indent, depth);
      jb(b, "}");
      return;
    }
    case AX_VEC2: case AX_VEC3: case AX_QUAT: {
      AxVec *p = ax_vecp(v);
      if (sim) {
        double xs[4] = { p->x, p->y, p->z, p->w };
        json_numarr(b, xs, v.t == AX_VEC2 ? 2 : v.t == AX_VEC3 ? 3 : 4, indent, depth);
      } else {
        KV kv[4] = { { "x", ax_num(p->x), false }, { "y", ax_num(p->y), false }, { "z", ax_num(p->z), false }, { "w", ax_num(p->w), false } };
        json_object(b, kv, v.t == AX_VEC2 ? 2 : v.t == AX_VEC3 ? 3 : 4, indent, depth, sim);
      }
      return;
    }
    case AX_ENTITY: {
      AxEntity *e = (AxEntity *)v.o;
      KV kv[1] = { { "entity", ax_strv(e->name), false } };
      json_object(b, kv, 1, indent, depth, sim);
      return;
    }
    case AX_TIMER: {
      AxTimer *t = (AxTimer *)v.o;
      if (sim) {
        jb(b, "{");
        jpad(b, indent, depth + 1);
        jb(b, indent ? "\"timer\": " : "\"timer\":");
        KV kv[2] = { { "remaining", ax_num(t->remaining), false }, { "total", ax_num(t->total), false } };
        json_object(b, kv, 2, indent, depth + 1, sim);
        jpad(b, indent, depth);
        jb(b, "}");
      } else {
        AxValue unit = ax_str_from(t->unit);
        KV kv[4] = { { "__timer", ax_bool(true), false }, { "remaining", ax_num(t->remaining), false },
                     { "total", ax_num(t->total), false }, { "unit", unit, false } };
        json_object(b, kv, 4, indent, depth, sim);
        ax_release(unit);
      }
      return;
    }
    case AX_SHAPE: {
      AxShape *s = (AxShape *)v.o;
      AxArr *ps = ax_arr_new(2);
      for (int i = 0; i < s->nparams; i++) ax_arr_push(ps, ax_copy(s->params[i]));
      KV kv[2] = { { "kind", ax_strv(s->kind), false }, { "params", ax_arrv(ps), false } };
      json_object(b, kv, 2, indent, depth, sim);
      ax_release(ax_arrv(ps));
      return;
    }
    case AX_XFORM: {
      AxXform *x = (AxXform *)v.o;
      KV kv[5] = { { "pos", x->pos, false }, { "rot", x->rot, false }, { "scl", x->scl, false }, { "vel", x->vel, false } };
      int n = 4;
      AxValue ev = ax_null();
      if (x->entity) { ev.t = AX_ENTITY; ev.o = (AxObj *)x->entity; kv[4].k = "_entity"; kv[4].v = ev; n = 5; }
      json_object(b, kv, n, indent, depth, sim);
      return;
    }
    case AX_MAT4: {
      AxMat4 *m = (AxMat4 *)v.o;
      jb(b, "{");
      jpad(b, indent, depth + 1);
      jb(b, indent ? "\"d\": {" : "\"d\":{");
      for (int i = 0; i < 16; i++) {
        if (i) jb(b, ",");
        jpad(b, indent, depth + 2);
        char k[16];
        snprintf(k, sizeof k, indent ? "\"%d\": " : "\"%d\":", i);
        jb(b, k);
        jnum(b, m->d[i]);
      }
      jpad(b, indent, depth + 1);
      jb(b, "}");
      jpad(b, indent, depth);
      jb(b, "}");
      return;
    }
    case AX_RANGE: {
      AxRange *r = (AxRange *)v.o;
      KV kv[3] = { { "lo", ax_num(r->lo), false }, { "hi", ax_num(r->hi), false }, { "step", ax_num(r->step), false } };
      json_object(b, kv, 3, indent, depth, sim);
      return;
    }
    case AX_FN: jb(b, sim ? "\"<fn>\"" : "null"); return;
    case AX_BIG: {
      // The state dump writes a big as its digits; JSON.stringify cannot write one at all,
      // which the caller learns from the flag.
      b->bigint = true;
      char *digits = ax_big_to_cstr((AxBig *)v.o);
      jstr(b, digits, strlen(digits));
      free(digits);
      return;
    }
    case AX_HOST: {
      extern void ax_host_json(AxHost *h, void *jbp, int indent, int depth, bool sim);
      ax_host_json((AxHost *)v.o, b, indent, depth, sim);
      return;
    }
    default: jb(b, "null"); return;
  }
}

bool ax_json_write_checked(AxValue v, int indent, char **out) {
  init_keys();
  JB b = { 0 };
  jb(&b, "");
  json_value(&b, v, indent, 0, false);
  *out = b.buf;
  return !b.bigint;
}

void ax_json_write(AxValue v, int indent, char **out) { ax_json_write_checked(v, indent, out); }

// ---------------------------------------------------------------------------------------------
// Operators on engine values (interpreter.js binaryOp, first four blocks)
// ---------------------------------------------------------------------------------------------

static AxValue raycast_world(AxWorld *w, V3 origin, V3 dir, double max_dist);

static const char *op_text(int op) {
  switch (op) {
    case OP_ADD: return "+"; case OP_SUB: return "-"; case OP_MUL: return "*"; case OP_DIV: return "/";
    case OP_MOD: return "%"; case OP_POW: return "**"; case OP_GT: return ">"; case OP_LT: return "<";
    case OP_GE: return ">="; case OP_LE: return "<="; case OP_EQ: return "=="; case OP_NE: return "!=";
    case OP_IN: return "in"; case OP_DOT: return "\xC2\xB7"; case OP_CROSS: return "\xC3\x97";
    default: return "?";
  }
}

// JavaScript's String(v) for an engine value — what `"hp " + v` produces.
static void js_tostring(AxValue v, char **buf, size_t *len, size_t *cap) {
  char tmp[64];
  switch (v.t) {
    case AX_VEC2: case AX_VEC3: case AX_QUAT: {
      AxVec *p = ax_vecp(v);
      app(buf, len, cap, v.t == AX_VEC2 ? "v2(" : v.t == AX_VEC3 ? "v3(" : "q(");
      ax_fmt_num(p->x, tmp, sizeof tmp); app(buf, len, cap, tmp); app(buf, len, cap, ", ");
      ax_fmt_num(p->y, tmp, sizeof tmp); app(buf, len, cap, tmp);
      if (v.t != AX_VEC2) { app(buf, len, cap, ", "); ax_fmt_num(p->z, tmp, sizeof tmp); app(buf, len, cap, tmp); }
      if (v.t == AX_QUAT) { app(buf, len, cap, ", "); ax_fmt_num(p->w, tmp, sizeof tmp); app(buf, len, cap, tmp); }
      app(buf, len, cap, ")");
      return;
    }
    case AX_SHAPE: {
      AxShape *s = (AxShape *)v.o;
      app(buf, len, cap, s->kind->data);
      app(buf, len, cap, "(");
      for (int i = 0; i < s->nparams; i++) {
        if (i) app(buf, len, cap, ", ");
        if (s->params[i].t == AX_NULL) continue;
        if (s->params[i].t >= AX_VEC2) js_tostring(s->params[i], buf, len, cap);
        else { AxStr *t = ax_to_str(s->params[i]); app(buf, len, cap, t->data); ax_release(ax_strv(t)); }
      }
      app(buf, len, cap, ")");
      return;
    }
    case AX_TIMER: ax_fmt_num(((AxTimer *)v.o)->remaining, tmp, sizeof tmp); app(buf, len, cap, tmp); return;
    default:
      if (v.t >= AX_VEC2) { app(buf, len, cap, "[object Object]"); return; }
      if (v.t == AX_DICT) { app(buf, len, cap, "[object Object]"); return; }
      { AxStr *t = ax_to_str(v); app(buf, len, cap, t->data); ax_release(ax_strv(t)); }
  }
}

bool ax_engine_binary(AxVM *vm, int op, AxValue l, AxValue r, AxValue *out) {
  init_keys();
  if (op == OP_RAY) {
    // `origin ?> dir * maxDist` — the nearest collider hit, or null.
    if (l.t != AX_VEC3 || r.t != AX_VEC3) { *out = ax_null(); return true; }
    V3 rv = v3of(r);
    *out = raycast_world(W(vm), v3of(l), v3norm(rv), v3mag(rv));
    return true;
  }
  // Equality, membership and string concatenation mean the same thing for every type; the
  // interpreter's generic rules handle them (a string `+` renders the other side like f"{v}").
  if (op == OP_EQ || op == OP_NE || op == OP_IN) return false;
  if (op == OP_ADD && (l.t == AX_STR || r.t == AX_STR)) return false;
  bool fall = false;   // a `*` that no vector rule claimed falls through to the next block
  if (l.t == AX_VEC3 || r.t == AX_VEC3) {
    bool lv = l.t == AX_VEC3 || l.t == AX_VEC2, rv = r.t == AX_VEC3 || r.t == AX_VEC2;
    V3 a = v3of(l), b = v3of(r);
    if (l.t == AX_VEC2) a = v3(ax_vecp(l)->x, 0, ax_vecp(l)->y);   // Vec2 promotes to the XZ plane
    if (r.t == AX_VEC2) b = v3(ax_vecp(r)->x, 0, ax_vecp(r)->y);
    if (r.t == AX_QUAT) b = v3of(r);
    switch (op) {
      case OP_ADD: case OP_SUB: case OP_DOT: case OP_CROSS:
        if (!lv) ax_throw(vm, "AX-RUNTIME-000", "l.%s is not a function", op == OP_ADD ? "add" : op == OP_SUB ? "sub" : op == OP_DOT ? "dot" : "cross");
        if (!rv && r.t != AX_QUAT) b = v3(NAN, NAN, NAN);
        if (op == OP_ADD) *out = mk3(v3add(a, b));
        else if (op == OP_SUB) *out = mk3(v3sub(a, b));
        else if (op == OP_DOT) *out = ax_num(v3dot(a, b));
        else *out = mk3(v3cross(a, b));
        return true;
      case OP_MUL:
        if (lv && rv) { *out = ax_vec3(a.x * b.x, a.y * b.y, a.z * b.z); return true; }
        if (lv && r.t == AX_NUM) { *out = mk3(v3mul(a, r.num)); return true; }
        if (rv && l.t == AX_NUM) { *out = mk3(v3mul(b, l.num)); return true; }
        if (r.t == AX_QUAT) { *out = mk3(qrot(qof(r), lv ? a : v3(NAN, NAN, NAN))); return true; }
        fall = true;
        break;
      case OP_DIV:
        if (lv && r.t == AX_NUM) { *out = mk3(v3div(a, r.num)); return true; }
        ax_throw(vm, "AX-RUNTIME-000", "'/' requires a vector divided by a scalar");
        return true;
      default:
        ax_throw(vm, "AX-RUNTIME-000", "operator '%s' not defined for vectors", op_text(op));
        return true;
    }
  }
  if (!fall && (l.t == AX_VEC2 || r.t == AX_VEC2)) {
    bool lv = l.t == AX_VEC2, rv = r.t == AX_VEC2;
    double ax_ = lv ? ax_vecp(l)->x : NAN, ay = lv ? ax_vecp(l)->y : NAN;
    double bx = NAN, by = NAN;
    if (r.t == AX_VEC2 || r.t == AX_QUAT) { bx = ax_vecp(r)->x; by = ax_vecp(r)->y; }
    switch (op) {
      case OP_ADD: case OP_SUB: case OP_DOT: case OP_CROSS:
        if (!lv) ax_throw(vm, "AX-RUNTIME-000", "l.%s is not a function", op == OP_ADD ? "add" : op == OP_SUB ? "sub" : op == OP_DOT ? "dot" : "cross");
        if (op == OP_ADD) *out = ax_vec2(ax_ + bx, ay + by);
        else if (op == OP_SUB) *out = ax_vec2(ax_ - bx, ay - by);
        else if (op == OP_DOT) *out = ax_num(ax_ * bx + ay * by);
        else *out = ax_num(ax_ * by - ay * bx);
        return true;
      case OP_MUL:
        if (lv && rv) { *out = ax_vec2(ax_ * bx, ay * by); return true; }
        if (lv && r.t == AX_NUM) { *out = ax_vec2(ax_ * r.num, ay * r.num); return true; }
        if (rv && l.t == AX_NUM) { *out = ax_vec2(bx * l.num, by * l.num); return true; }
        ax_throw(vm, "AX-RUNTIME-000", "'*' between a Vec2 and non-vector/non-number is undefined");
        return true;
      case OP_DIV:
        if (lv && r.t == AX_NUM) { *out = ax_vec2(ax_ / r.num, ay / r.num); return true; }
        ax_throw(vm, "AX-RUNTIME-000", "'/' requires a vector divided by a scalar");
        return true;
      default:
        ax_throw(vm, "AX-RUNTIME-000", "operator '%s' not defined for vectors", op_text(op));
        return true;
    }
  }
  if (l.t == AX_QUAT || r.t == AX_QUAT) {
    if (op == OP_MUL) {
      if (l.t == AX_QUAT && r.t == AX_QUAT) { *out = mkq(qmul(qof(l), qof(r))); return true; }
      if (l.t == AX_QUAT && r.t == AX_VEC3) { *out = mk3(qrot(qof(l), v3of(r))); return true; }
      fall = true;
    } else {
      ax_throw(vm, "AX-RUNTIME-000", "operator '%s' not defined for quaternions", op_text(op));
    }
  }
  if (l.t == AX_MAT4 || r.t == AX_MAT4) {
    if (op == OP_MUL) {
      if (l.t == AX_MAT4 && r.t == AX_MAT4) {
        AxValue m = ax_mat4_new(NULL);
        m4_mul(((AxMat4 *)l.o)->d, ((AxMat4 *)r.o)->d, ((AxMat4 *)m.o)->d);
        *out = m;
        return true;
      }
      if (l.t == AX_MAT4 && r.t == AX_VEC3) { *out = mk3(m4_mul_v3(((AxMat4 *)l.o)->d, v3of(r))); return true; }
      fall = true;
    } else {
      ax_throw(vm, "AX-RUNTIME-000", "operator '%s' not defined for matrices", op_text(op));
    }
  }
  if (fall) { *out = ax_num(NAN); return true; }
  // Everything else follows JavaScript on objects: `+` with a string concatenates the value's
  // String() form, other arithmetic is NaN, ordering is false, equality is identity.
  switch (op) {
    case OP_ADD: {
      if (l.t == AX_NUM && r.t == AX_NUM) return false;
      char *buf = NULL; size_t len = 0, cap = 0;
      app(&buf, &len, &cap, "");
      js_tostring(l, &buf, &len, &cap);
      js_tostring(r, &buf, &len, &cap);
      *out = ax_strv(ax_str_new(buf, len));
      free(buf);
      return true;
    }
    case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: case OP_POW: *out = ax_num(NAN); return true;
    case OP_GT: case OP_LT: case OP_GE: case OP_LE: *out = ax_bool(false); return true;
    case OP_DOT: case OP_CROSS: *out = ax_num(NAN); return true;
    case OP_IN: *out = ax_bool(r.t == AX_HOST ? ax_host_contains(vm, r, l) : false); return true;
    default: return false;
  }
}

// ---------------------------------------------------------------------------------------------
// Members and methods of engine values (interpreter.js memberOf / callMethod)
// ---------------------------------------------------------------------------------------------

bool ax_engine_member(AxVM *vm, AxValue obj, AxStr *prop, AxValue *out) {
  init_keys();
  *out = ax_null();
  if (!prop->interned) {
    // Property keys compare by pointer; a name built at run time (a field-name selector,
    // `v[name]`) is interned first so it finds the same members as `v.name`.
    AxStr *k = ax_intern(prop->data, prop->len);
    bool r = ax_engine_member(vm, obj, k, out);
    ax_release(ax_strv(k));
    return r;
  }
  switch (obj.t) {
    case AX_VEC2: {
      AxVec *p = ax_vecp(obj);
      if (prop == K_mag) *out = ax_num(hyp2(p->x, p->y));
      else if (prop == K_norm) { double m = hyp2(p->x, p->y); *out = m < 1e-9 ? ax_vec2(0, 0) : ax_vec2(p->x / m, p->y / m); }
      else if (prop == K_angle) *out = ax_num(atan2(p->y, p->x));
      else if (prop == K_x) *out = ax_num(p->x);
      else if (prop == K_y) *out = ax_num(p->y);
      return true;
    }
    case AX_VEC3: {
      AxVec *p = ax_vecp(obj);
      V3 a = v3of(obj);
      if (prop == K_mag) *out = ax_num(v3mag(a));
      else if (prop == K_norm) *out = mk3(v3norm(a));
      else if (prop == K_xy) *out = ax_vec2(p->x, p->y);
      else if (prop == K_xz) *out = ax_vec2(p->x, p->z);
      else if (prop == K_yz) *out = ax_vec2(p->y, p->z);
      else if (prop == K_x) *out = ax_num(p->x);
      else if (prop == K_y) *out = ax_num(p->y);
      else if (prop == K_z) *out = ax_num(p->z);
      return true;
    }
    case AX_QUAT: {
      AxVec *p = ax_vecp(obj);
      Q q = qof(obj);
      if (prop == K_euler) *out = mk3(q_to_euler(q));
      else if (prop == K_conj) *out = mkq(qconj(q));
      else if (prop == K_normalized) *out = mkq(qnormalized(q));
      else if (prop == K_x) *out = ax_num(p->x);
      else if (prop == K_y) *out = ax_num(p->y);
      else if (prop == K_z) *out = ax_num(p->z);
      else if (prop == K_w) *out = ax_num(p->w);
      return true;
    }
    case AX_MAT4: {
      AxMat4 *m = (AxMat4 *)obj.o;
      if (prop == K_inv) { AxValue r = ax_mat4_new(NULL); m4_inv(m->d, ((AxMat4 *)r.o)->d); *out = r; }
      else if (prop == K_T) {
        AxValue r = ax_mat4_new(NULL);
        for (int c = 0; c < 4; c++) for (int row = 0; row < 4; row++) ((AxMat4 *)r.o)->d[row * 4 + c] = m->d[c * 4 + row];
        *out = r;
      }
      return true;
    }
    case AX_XFORM: {
      AxXform *x = (AxXform *)obj.o;
      if (prop == K_pos) *out = ax_copy(x->pos);
      else if (prop == K_rot) *out = ax_copy(x->rot);
      else if (prop == K_scl) *out = ax_copy(x->scl);
      else if (prop == K_vel) *out = ax_copy(x->vel);
      return true;
    }
    case AX_ENTITY: {
      AxEntity *e = (AxEntity *)obj.o;
      if (ax_dict_get(e->fields, prop, out)) return true;
      if (ax_dict_get(e->locals, prop, out)) return true;
      *out = ax_null();
      AxXform *p = ent_pose(e);
      if (p) {
        if (prop == K_pos) *out = ax_copy(p->pos);
        else if (prop == K_vel) *out = ax_copy(p->vel);
        else if (prop == K_rot) *out = ax_copy(p->rot);
        else if (prop == K_scl) *out = ax_copy(p->scl);
        else if (prop == K_wpose) *out = world_pose(e->world, p, 0);
      }
      if (prop == K_tagName || prop == K_name) *out = str_val(e->name);
      return true;
    }
    case AX_TIMER: {
      AxTimer *t = (AxTimer *)obj.o;
      if (prop == K_remaining) *out = ax_num(t->remaining);
      else if (prop == K_total) *out = ax_num(t->total);
      else if (prop == K_unit) *out = ax_str_from(t->unit);
      return true;
    }
    case AX_SHAPE: {
      AxShape *s = (AxShape *)obj.o;
      if (prop == K_kind) *out = str_val(s->kind);
      else if (prop == K_params) {
        AxArr *a = ax_arr_new(2);
        for (int i = 0; i < s->nparams; i++) ax_arr_push(a, ax_copy(s->params[i]));
        *out = ax_arrv(a);
      }
      return true;
    }
    case AX_HOST: {
      extern bool ax_host_member(AxVM *vm, AxHost *h, AxStr *prop, AxValue *out);
      return ax_host_member(vm, (AxHost *)obj.o, prop, out);
    }
    default: return false;
  }
}

bool ax_engine_set_member(AxVM *vm, AxValue obj, AxStr *prop, AxValue v) {
  init_keys();
  switch (obj.t) {
    case AX_VEC2: case AX_VEC3: case AX_QUAT: {
      AxVec *p = ax_vecp(obj);
      double d = ax_to_num(v);
      if (prop == K_x) p->x = d;
      else if (prop == K_y) p->y = d;
      else if (prop == K_z && obj.t != AX_VEC2) p->z = d;
      else if (prop == K_w && obj.t == AX_QUAT) p->w = d;
      ax_release(v);
      return true;
    }
    case AX_XFORM: {
      if (prop == K_pos || prop == K_rot || prop == K_scl || prop == K_vel) { xform_set((AxXform *)obj.o, prop, v); return true; }
      ax_release(v);
      return true;
    }
    case AX_TIMER: {
      AxTimer *t = (AxTimer *)obj.o;
      if (prop == K_remaining) t->remaining = ax_to_num(v);
      else if (prop == K_total) t->total = ax_to_num(v);
      ax_release(v);
      return true;
    }
    default:
      ax_release(v);
      return false;
  }
}

static double num_arg(AxValue *args, int argc, int i) { return i < argc ? ax_to_num(args[i]) : NAN; }
static AxValue arg_or_null(AxValue *args, int argc, int i) { return i < argc ? args[i] : ax_null(); }

bool ax_engine_method(AxVM *vm, AxValue obj, AxStr *name, AxValue *args, int argc, AxValue *out) {
  init_keys();
  if (obj.t == AX_NUM) {
    if (name == K_clamp) { *out = ax_num(js_clamp(obj.num, num_arg(args, argc, 0), num_arg(args, argc, 1))); return true; }
    return false;
  }
  if (obj.t == AX_VEC2) {
    AxVec *p = ax_vecp(obj);
    AxValue a0 = arg_or_null(args, argc, 0);
    double tx = (a0.t >= AX_VEC2 && a0.t <= AX_QUAT) ? ax_vecp(a0)->x : NAN, ty = (a0.t >= AX_VEC2 && a0.t <= AX_QUAT) ? ax_vecp(a0)->y : NAN;
    if (name == K_to) {
      double dx = tx - p->x, dy = ty - p->y, d = hyp2(dx, dy), md = num_arg(args, argc, 1);
      if (d <= md || d < 1e-9) *out = ax_vec2(tx, ty);
      else *out = ax_vec2(p->x + dx / d * md, p->y + dy / d * md);
      return true;
    }
    if (name == K_lerp) { double t = num_arg(args, argc, 1); *out = ax_vec2(p->x + (tx - p->x) * t, p->y + (ty - p->y) * t); return true; }
    if (name == K_mag || name == K_norm) return ax_engine_member(vm, obj, name, out);
    if (name == K_clamp) { double lo = num_arg(args, argc, 0), hi = num_arg(args, argc, 1); *out = ax_vec2(js_clamp(p->x, lo, hi), js_clamp(p->y, lo, hi)); return true; }
    if (name == K_reflect) { double s = 2 * (p->x * tx + p->y * ty); *out = ax_vec2(p->x - tx * s, p->y - ty * s); return true; }
    if (name == K_rotate) { double a = num_arg(args, argc, 0), c = cos(a), s = sin(a); *out = ax_vec2(p->x * c - p->y * s, p->x * s + p->y * c); return true; }
    return false;
  }
  if (obj.t == AX_VEC3) {
    V3 a = v3of(obj);
    AxValue a0 = arg_or_null(args, argc, 0);
    V3 t = v3of(a0);
    if (name == K_to) {
      V3 diff = v3sub(t, a);
      double d = v3mag(diff), md = num_arg(args, argc, 1);
      if (d <= md || d < 1e-9) *out = mk3(t);
      else *out = mk3(v3add(a, v3mul(v3div(diff, d), md)));
      return true;
    }
    if (name == K_lerp) { double k = num_arg(args, argc, 1); *out = ax_vec3(a.x + (t.x - a.x) * k, a.y + (t.y - a.y) * k, a.z + (t.z - a.z) * k); return true; }
    if (name == K_reflect) { double d = 2 * v3dot(a, t); *out = ax_vec3(a.x - d * t.x, a.y - d * t.y, a.z - d * t.z); return true; }
    if (name == K_rotate) {
      if (a0.t != AX_QUAT) ax_throw(vm, "AX-RUNTIME-000", "q.rotateVec is not a function");
      *out = mk3(qrot(qof(a0), a));
      return true;
    }
    if (name == K_mag || name == K_norm) return ax_engine_member(vm, obj, name, out);
    if (name == K_clamp) { double lo = num_arg(args, argc, 0), hi = num_arg(args, argc, 1); *out = ax_vec3(js_clamp(a.x, lo, hi), js_clamp(a.y, lo, hi), js_clamp(a.z, lo, hi)); return true; }
    return false;
  }
  if (obj.t == AX_QUAT) {
    if (name == K_slerp) { *out = mkq(qslerp(qof(obj), qof(arg_or_null(args, argc, 0)), num_arg(args, argc, 1))); return true; }
    if (name == K_normalized) { *out = mkq(qnormalized(qof(obj))); return true; }
    return false;
  }
  if (obj.t == AX_XFORM) {
    if (name == K_toMat4) { AxValue m = ax_mat4_new(NULL); xform_to_m4((AxXform *)obj.o, ((AxMat4 *)m.o)->d); *out = m; return true; }
    return false;
  }
  if (obj.t == AX_HOST) {
    extern bool ax_host_method(AxVM *vm, AxHost *h, AxStr *name, AxValue *args, int argc, AxValue *out);
    return ax_host_method(vm, (AxHost *)obj.o, name, args, argc, out);
  }
  if (obj.t == AX_ENTITY) {
    // A field holding a function is a method.
    AxValue m;
    if (ax_entity_get((AxEntity *)obj.o, name, &m)) {
      if (m.t == AX_FN) { *out = ax_call(vm, m, args, argc); ax_release(m); return true; }
      ax_release(m);
    }
    return false;
  }
  return false;
}

// ---------------------------------------------------------------------------------------------
// Engine intrinsics (interpreter.js defaultIntrinsics — the ones the library does not cover)
// ---------------------------------------------------------------------------------------------

#define NATIVE(name) static AxValue name(AxVM *vm, AxFn *self, AxValue *args, int argc)
#define A(i) ((i) < argc ? args[i] : ax_null())
#define N(i) ((i) < argc ? ax_to_num(args[i]) : NAN)

NATIVE(e_v2) { return argc < 2 ? ax_vec2(N(0), N(0)) : ax_vec2(N(0), N(1)); }
NATIVE(e_v3) {
  if (argc < 3) {
    if (argc < 2) return ax_vec3(N(0), N(0), N(0));   // uniform
    return ax_vec3(N(0), 0, N(1));                       // the horizontal plane
  }
  return ax_vec3(N(0), N(1), N(2));
}
NATIVE(e_v3x) { return ax_vec3(N(0), 0, 0); }
NATIVE(e_v3y) { return ax_vec3(0, N(0), 0); }
NATIVE(e_v3z) { return ax_vec3(0, 0, N(0)); }
NATIVE(e_v3xz) { return ax_vec3(N(0), 0, N(1)); }
// A big where the reference does arithmetic on the argument, or hands it to Math.*.
static void big_arg(AxVM *vm, AxValue *args, int argc, bool mix) {
  for (int i = 0; i < argc; i++)
    if (args[i].t == AX_BIG)
      ax_throw(vm, "AX-RUNTIME-000", mix ? "Cannot mix BigInt and other types, use explicit conversions" : "Cannot convert a BigInt value to a number");
}

NATIVE(e_v2dir) { big_arg(vm, args, argc < 1 ? argc : 1, false); return ax_vec2(cos(N(0)), sin(N(0))); }
// An argument that must be a vector: the same error the reference raises.
static void need_vec3(AxVM *vm, const char *fn, const char *what, AxValue v) {
  if (v.t != AX_VEC3) ax_throw(vm, "AX-RUNTIME-000", "%s() needs %s, got %s", fn, what, ax_type_name(v));
}

NATIVE(e_q) { need_vec3(vm, "q", "a vec3 axis", A(0)); return mkq(q_axis_angle(v3of(A(0)), N(1))); }
NATIVE(e_euler) { big_arg(vm, args, argc, true); return mkq(q_euler(N(0), N(1), N(2))); }
NATIVE(e_m4) { AxValue m = ax_mat4_new(NULL); m4_identity(((AxMat4 *)m.o)->d); return m; }
NATIVE(e_persp) {
  big_arg(vm, args, argc, true);
  AxValue m = ax_mat4_new(NULL);
  double *d = ((AxMat4 *)m.o)->d;
  double f = 1 / tan(N(0) * M_PI / 360), nf = 1 / (N(2) - N(3));
  d[0] = f / N(1); d[5] = f; d[10] = (N(3) + N(2)) * nf; d[11] = -1; d[14] = 2 * N(3) * N(2) * nf;
  return m;
}
NATIVE(e_ortho) {
  big_arg(vm, args, argc, true);
  AxValue m = ax_mat4_new(NULL);
  double *d = ((AxMat4 *)m.o)->d;
  double l = N(0), r = N(1), b = N(2), t = N(3), n = N(4), f = N(5);
  d[0] = 2 / (r - l); d[5] = 2 / (t - b); d[10] = -2 / (f - n);
  d[12] = -(r + l) / (r - l); d[13] = -(t + b) / (t - b); d[14] = -(f + n) / (f - n); d[15] = 1;
  return m;
}
NATIVE(e_lookat) {
  need_vec3(vm, "lookat", "vec3 points", A(0));
  need_vec3(vm, "lookat", "vec3 points", A(1));
  if (argc > 2 && args[2].t != AX_NULL) need_vec3(vm, "lookat", "vec3 points", args[2]);
  AxValue m = ax_mat4_new(NULL);
  V3 up = (argc > 2 && ax_truthy(args[2])) ? v3of(args[2]) : v3(0, 1, 0);
  m4_look_at(v3of(A(0)), v3of(A(1)), up, ((AxMat4 *)m.o)->d);
  return m;
}
NATIVE(e_aabb) {
  AxDict *d = ax_dict_new();
  AxStr *k = ax_internz("__aabb"); ax_dict_set(d, k, ax_bool(true)); ax_release(ax_strv(k));
  k = ax_internz("min"); ax_dict_set(d, k, ax_copy(A(0))); ax_release(ax_strv(k));
  k = ax_internz("max"); ax_dict_set(d, k, ax_copy(A(1))); ax_release(ax_strv(k));
  return ax_dictv(d);
}
NATIVE(e_dist) {
  AxValue a = A(0), b = A(1);
  if (a.t == AX_NUM && b.t == AX_NUM) return ax_num(fabs(a.num - b.num));
  bool av = a.t == AX_VEC2 || a.t == AX_VEC3, bv = b.t == AX_VEC2 || b.t == AX_VEC3;
  if (!av || !bv) ax_throw(vm, "AX-RUNTIME-000", "dist() needs two numbers or two vectors, got %s and %s", ax_type_name(a), ax_type_name(b));
  AxValue diff = ax_binary_op(vm, OP_SUB, a, b);
  AxValue m;
  if (!ax_engine_member(vm, diff, K_mag, &m)) m = ax_num(NAN);
  ax_release(diff);
  return m;
}
static AxValue shape(const char *kind, AxValue *args, int argc, int n) {
  AxShape *s = calloc(1, sizeof(AxShape));
  s->hdr.rc = 1;
  s->hdr.type = AX_SHAPE;
  s->kind = ax_internz(kind);
  s->nparams = n;
  for (int i = 0; i < n; i++) s->params[i] = i < argc ? ax_copy(args[i]) : ax_null();
  AxValue v; v.t = AX_SHAPE; v.o = (AxObj *)s;
  return v;
}
NATIVE(e_sphere) { return shape("sphere", args, argc, 1); }
NATIVE(e_box) { return shape("box", args, argc, 1); }
NATIVE(e_capsule) { return shape("capsule", args, argc, 2); }
NATIVE(e_cell_to_world) {
  // cell.x and cell.y of a dict or vector; a missing one is NaN (undefined), not 0.
  double xy[2] = { NAN, NAN };
  AxStr *keys[2] = { K_x, K_y };
  for (int i = 0; i < 2; i++) {
    AxValue v;
    bool have = A(0).t == AX_DICT ? ax_dict_get((AxDict *)A(0).o, keys[i], &v) : (A(0).t >= AX_VEC2 && ax_engine_member(vm, A(0), keys[i], &v));
    if (have) { xy[i] = ax_to_num(v); ax_release(v); }
  }
  return ax_vec3(xy[0] + 0.5, 0, xy[1] + 0.5);
}
NATIVE(e_bar) {
  AxDict *d = ax_dict_new();
  const char *keys[] = { "__hud", "kind", "pos", "w", "h", "fg" };
  for (int i = 0; i < 6; i++) {
    AxStr *k = ax_internz(keys[i]);
    if (i >= 2 && i - 2 >= argc) { ax_release(ax_strv(k)); continue; }   // an absent part is left out
    AxValue v = i == 0 ? ax_bool(true) : i == 1 ? ax_str_from("bar") : ax_copy(A(i - 2));
    ax_dict_set(d, k, v);
    ax_release(ax_strv(k));
  }
  return ax_dictv(d);
}
NATIVE(e_hypot) { big_arg(vm, args, argc, false); double v[2] = { N(0), N(1) }; return ax_num(js_hypot(2, v)); }

static void def(AxVM *vm, const char *name, AxNativeFn fn, int min_args, int max_args) {
  AxStr *key = ax_internz(name);
  ax_scope_declare(vm->builtins, key, ax_native(name, fn, min_args, max_args));
  ax_release(ax_strv(key));
}

void ax_engine_install(AxVM *vm) {
  init_keys();
  def(vm, "v2", e_v2, 1, 2);       def(vm, "v3", e_v3, 1, 3);
  def(vm, "v3x", e_v3x, 1, 1);     def(vm, "v3y", e_v3y, 1, 1);
  def(vm, "v3z", e_v3z, 1, 1);     def(vm, "v3xz", e_v3xz, 2, 2);
  def(vm, "v2dir", e_v2dir, 1, 1); def(vm, "q", e_q, 2, 2);
  def(vm, "euler", e_euler, 3, 3); def(vm, "m4", e_m4, 0, 0);
  def(vm, "persp", e_persp, 4, 4); def(vm, "ortho", e_ortho, 6, 6);
  def(vm, "lookat", e_lookat, 2, 3); def(vm, "aabb", e_aabb, 2, 2);
  def(vm, "dist", e_dist, 2, 2);
  def(vm, "sphere", e_sphere, 1, 1); def(vm, "box", e_box, 1, 1); def(vm, "capsule", e_capsule, 2, 2);
  def(vm, "cell_to_world", e_cell_to_world, 1, 1);
  def(vm, "bar", e_bar, 1, 4);
  def(vm, "hypot", e_hypot, 2, 2);   // V8's rounding, so both runtimes agree to the bit
  extern void ax_engine_install_more(AxVM *vm);
  ax_engine_install_more(vm);
}

// ---------------------------------------------------------------------------------------------
// Diagnostics — a fault in a frame block is recorded and ends that block, never the program
// (interpreter.js runBlock / makeRuntimeFault)
// ---------------------------------------------------------------------------------------------

static void push_diag(AxVM *vm, AxEntity *e, AxStr *block, AxNode *stmt) {
  AxWorld *w = W(vm);
  if (w->ndiags == w->capdiags) {
    w->capdiags = w->capdiags ? w->capdiags * 2 : 8;
    w->diags = realloc(w->diags, sizeof(Diag) * w->capdiags);
  }
  Diag *d = &w->diags[w->ndiags++];
  memset(d, 0, sizeof *d);
  snprintf(d->code, sizeof d->code, "%s", vm->error_code[0] ? vm->error_code : "AX-RUNTIME-000");
  snprintf(d->severity, sizeof d->severity, "fatal");
  snprintf(d->title, sizeof d->title, "Runtime Fault");
  AxValue msg;
  if (vm->error.t == AX_DICT && ax_dict_get((AxDict *)vm->error.o, ax_internz("msg"), &msg)) {
    AxStr *s = ax_to_str(msg);
    snprintf(d->human, sizeof d->human, "%s", s->data);
    ax_release(ax_strv(s));
    ax_release(msg);
  } else {
    snprintf(d->human, sizeof d->human, "%s", vm->error_msg);
  }
  d->entity = e ? e->name : NULL;
  d->block = block;
  d->line = stmt ? stmt->line : 0;
  d->col = stmt ? stmt->col : 0;
  if (w->source && d->line > 0) {
    const char *p = w->source;
    for (int ln = 1; ln < d->line && p; ln++) { p = strchr(p, '\n'); if (p) p++; }
    if (p) {
      while (*p == ' ' || *p == '\t') p++;
      const char *end = strchr(p, '\n');
      size_t n = end ? (size_t)(end - p) : strlen(p);
      while (n && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r')) n--;
      if (n >= sizeof d->snippet) n = sizeof d->snippet - 1;
      memcpy(d->snippet, p, n);
      d->snippet[n] = '\0';
      d->has_snippet = true;
    }
  }
}

// A fault outside any entity: a global initializer ("global") or ^main ("main").
void ax_engine_fault(AxVM *vm, const char *block, AxNode *stmt) {
  if (!vm->world) ax_engine_init(vm, NULL);
  push_diag(vm, NULL, ax_internz(block), stmt);
  vm->error_code[0] = '\0';
}

bool ax_engine_diag_at(AxVM *vm, int i, const char **code, const char **msg) {
  AxWorld *w = W(vm);
  if (!w || i < 0 || i >= w->ndiags) return false;
  *code = w->diags[i].code;
  *msg = w->diags[i].human;
  return true;
}

int ax_engine_last_fault(AxVM *vm, char *code, size_t coden, char *msg, size_t msgn) {
  AxWorld *w = W(vm);
  if (!w || !w->ndiags) { snprintf(code, coden, "AX-RUNTIME-000"); snprintf(msg, msgn, "?"); return 0; }
  snprintf(code, coden, "%s", w->diags[w->ndiags - 1].code);
  snprintf(msg, msgn, "%s", w->diags[w->ndiags - 1].human);
  return w->diags[w->ndiags - 1].line;
}

static bool hot_block(AxStr *name) { return name == K_physics || name == K_render || name == K_tick || name == K_on; }

// Run one frame block. Each statement runs in turn; a fault is recorded and ends the block.
static void run_block(AxVM *vm, AxNode *block, AxEntity *e, double dt, AxDict *payload) {
  AxCtx saved = vm->ctx;
  vm->ctx.entity = e;
  vm->ctx.dt = dt;
  vm->ctx.payload = payload;
  vm->ctx.block = block->str;
  vm->ctx.in_fn = false;
  vm->ctx.hot = hot_block(block->str);
  if (vm->nhandlers >= AX_MAX_HANDLERS) ax_throw(vm, "AX-TRY", "frame blocks nested too deeply");
  AxScope *scope = ax_scope_enter(vm, vm->globals, false);
  int hidx = ax_handler_push(vm);
  int saved_depth = vm->call_depth;
  volatile int i = 0;
  if (setjmp(vm->handlers[hidx]) == 0) {
    for (; i < block->nlist; i++) {
      AxValue out = ax_null();
      ax_exec(vm, block->list[i], scope, &out);   // a stray ^return/~break just ends that statement
      ax_release(out);
    }
    vm->nhandlers--;
  } else {
    vm->nhandlers = hidx;
    vm->call_depth = saved_depth;
    vm->ctx = saved;
    push_diag(vm, e, block->str, block->list[i]);
    vm->error_code[0] = '\0';
  }
  ax_scope_exit(vm, scope);
  vm->ctx = saved;
}

static void run_blocks_named(AxVM *vm, AxEntity *e, AxStr *name, double dt, AxDict *payload, AxStr *event) {
  for (int m = 0; m < e->nmembers; m++) {
    AxNode *b = e->members[m];
    if (b->kind != N_EBLOCK || b->str != name) continue;
    if (event && b->str2 != event) continue;
    if (name == K_on && !event) continue;
    run_block(vm, b, e, dt, payload);
  }
}

// ---------------------------------------------------------------------------------------------
// Entities
// ---------------------------------------------------------------------------------------------

static void world_add(AxWorld *w, AxEntity *e) {
  if (w->nents == w->capents) {
    w->capents = w->capents ? w->capents * 2 : 16;
    w->ents = realloc(w->ents, sizeof(AxEntity *) * w->capents);
  }
  w->ents[w->nents++] = e;   // the world owns this reference
  AxValue ev = ent_val(e);
  ax_dict_set(w->tags, e->name, ev);
}

extern AxValue ax_host_from_field(AxVM *vm, AxNode *field, AxEntity *e);

// new EntityInstance(decl): the legacy 2D locals, the pose, then every field in order.
static AxEntity *entity_new(AxVM *vm, AxStr *name, AxStr *prefab, AxStr *base, AxNode **members, int nmembers) {
  AxWorld *w = W(vm);
  AxEntity *e = calloc(1, sizeof(AxEntity));
  e->hdr.rc = 1;
  e->hdr.type = AX_ENTITY;
  e->name = name;
  ax_retain(ax_strv(name));
  e->prefab = prefab;
  e->base = base;
  e->members = members;
  e->nmembers = nmembers;
  e->fields = ax_dict_new();
  e->locals = ax_dict_new();
  e->world = w;
  ax_dict_set(e->locals, K_position, ax_vec2(0, 0));
  ax_dict_set(e->locals, K_velocity, ax_vec2(0, 0));
  ax_dict_set(e->locals, K_facing, ax_vec2(0, -1));
  AxValue pose = ax_xform_new();
  ((AxXform *)pose.o)->entity = e;
  ax_dict_set(e->locals, K_pose, pose);
  world_add(w, e);

  AxCtx saved = vm->ctx;
  vm->ctx.entity = e;
  vm->ctx.dt = 0;
  vm->ctx.payload = NULL;
  vm->ctx.block = NULL;
  vm->ctx.in_fn = false;
  vm->ctx.hot = false;
  for (int i = 0; i < nmembers; i++) {
    AxNode *m = members[i];
    if (m->kind != N_FIELD) continue;
    AxScope *scope = ax_scope_enter(vm, vm->globals, false);
    AxValue v;
    if (m->op == 1 || (m->a && m->a->kind == N_POOLTYPE)) {
      v = ax_host_from_field(vm, m, e);
    } else if (m->a && m->a->kind == N_CALL && (m->a->str == K_sphere || m->a->str == K_box || m->a->str == K_capsule || m->a->str == K_mesh)) {
      // A collider: the shape's arguments are evaluated, and kept whatever their number.
      AxShape *s = calloc(1, sizeof(AxShape));
      s->hdr.rc = 1;
      s->hdr.type = AX_SHAPE;
      s->kind = m->a->str;
      ax_retain(ax_strv(s->kind));
      for (int k = 0; k < m->a->nlist && k < 2; k++) s->params[s->nparams++] = ax_eval(vm, m->a->list[k]->b, scope);
      v.t = AX_SHAPE;
      v.o = (AxObj *)s;
    } else {
      v = m->a ? ax_eval(vm, m->a, scope) : ax_null();
      if (m->a && m->a->kind == N_NUM && m->a->str && (strcmp(m->a->str->data, "s") == 0 || strcmp(m->a->str->data, "ms") == 0)) {
        AxValue t = make_timer_from(v, m->a->str->data);
        ax_release(v);
        v = t;
      }
    }
    ax_scope_exit(vm, scope);
    ax_dict_set(e->fields, m->str, v);
  }
  for (int i = 0; i < nmembers; i++) {
    AxNode *m = members[i];
    if (m->kind == N_FIELD && m->op == 0 && m->str == K_nosave && !m->a) e->nosave = true;
  }
  vm->ctx = saved;
  return e;
}

static int find_decl(AxWorld *w, AxStr *name) {
  for (int i = 0; i < w->ndecls; i++) if (w->decls[i].name == name) return i;
  return -1;
}

static AxNode *find_named(AxNode **list, int n, AxStr *name) {
  for (int i = 0; i < n; i++) if (list[i]->str == name) return list[i];
  return NULL;
}

// Mixin members come first; an entity's own field of the same name shadows the mixin's.
static void compose_mixins(AxWorld *w, AxNode *decl, AxNode ***out, int *nout) {
  int n = decl->nlist;
  AxNode **members = malloc(sizeof(AxNode *) * (n ? n : 1));
  memcpy(members, decl->list, sizeof(AxNode *) * n);
  for (int mi = 0; mi < decl->nnames; mi++) {
    AxNode *mix = find_named(w->mixins, w->nmixins, decl->names[mi]);
    if (!mix) continue;
    AxNode **merged = malloc(sizeof(AxNode *) * (n + mix->nlist + 1));
    int k = 0;
    for (int j = 0; j < mix->nlist; j++) {
      AxNode *m = mix->list[j];
      bool shadowed = false;
      if (m->kind == N_FIELD)
        for (int q = 0; q < n && !shadowed; q++) shadowed = members[q]->kind == N_FIELD && members[q]->str == m->str;
      if (!shadowed) merged[k++] = m;
    }
    memcpy(merged + k, members, sizeof(AxNode *) * n);
    free(members);
    members = merged;
    n += k;
  }
  *out = members;
  *nout = n;
}

// ---------------------------------------------------------------------------------------------
// Loading a program (World.loadProgram)
// ---------------------------------------------------------------------------------------------

static AxWorld *world_new(void) {
  AxWorld *w = calloc(1, sizeof(AxWorld));
  w->tags = ax_dict_new();
  w->resources = ax_dict_new();
  w->log = ax_arr_new(16);
  w->tick_acc = ax_dict_new();
  w->channels = ax_dict_new();
  w->draw_list = ax_arr_new(8);
  w->save_slots = ax_dict_new();
  w->input = ax_dict_new();
  ax_dict_set(w->input, K_move, ax_vec2(0, 0));
  ax_dict_set(w->input, K_jump, ax_bool(false));
  ax_dict_set(w->input, K_fire, ax_bool(false));
  ax_dict_set(w->input, K_aim, ax_vec2(0, 0));
  return w;
}

#define PUSH(arr, n, item) do { arr = realloc(arr, sizeof(*(arr)) * ((n) + 1)); (arr)[(n)++] = (item); } while (0)

// ---- .glb meshes (World.loadProgram) ---------------------------------------------------------
//
// A #Mesh3D whose data loads as a .glb — inline (`base64("…")` or a `glb:` heredoc) or a file
// path, read relative to the working directory — is registered as X (the first primitive) and
// as X_0, X_1, … (every primitive), with `glb: true`, `inline: true` for embedded data, and the
// primitive's material index, and its `vertices` / `indices`. A mesh that does not load stays a
// plain descriptor, which the renderer draws by the shape its path names.

static void mesh_register(AxWorld *w, AxStr *name, const AxGlbPrim *prim) {
  w->meshes = realloc(w->meshes, sizeof(*w->meshes) * (w->nmeshes + 1));
  ax_retain(ax_strv(name));
  w->meshes[w->nmeshes].name = name;
  w->meshes[w->nmeshes].prim = prim;
  w->nmeshes++;
}

const AxGlbPrim *ax_world_mesh(AxVM *vm, AxStr *name) {
  AxWorld *w = W(vm);
  if (!w || !name) return NULL;
  for (int i = w->nmeshes - 1; i >= 0; i--) if (ax_str_eq(w->meshes[i].name, name)) return w->meshes[i].prim;
  return NULL;
}

// The descriptor's `vertices` (8 float32 per vertex) and `indices`, as the reference exposes
// them — plain arrays here, typed arrays there.
static AxValue prim_vertices(const AxGlbPrim *p) {
  AxArr *a = ax_arr_new((uint32_t)p->nverts * 8);
  for (int i = 0; i < p->nverts * 8; i++) ax_arr_push(a, ax_num(p->verts[i]));
  return ax_arrv(a);
}
static AxValue prim_indices(const AxGlbPrim *p) {
  AxArr *a = ax_arr_new((uint32_t)p->nidx);
  for (int i = 0; i < p->nidx; i++) ax_arr_push(a, ax_num(p->idx[i]));
  return ax_arrv(a);
}

static void load_glb_resource(AxWorld *w, AxNode *d, AxDict *res) {
  uint8_t *bytes = NULL;
  size_t nbytes = 0;
  bool inl = d->b != NULL;
  if (inl) ax_base64_decode(d->b->str->data, d->b->str->len, &bytes, &nbytes);
  else if (d->a) {
    FILE *f = fopen(d->a->str->data, "rb");
    if (f) {
      size_t cap = 1 << 16;
      bytes = malloc(cap);
      size_t got;
      while ((got = fread(bytes + nbytes, 1, cap - nbytes, f)) > 0) {
        nbytes += got;
        if (nbytes == cap) { cap *= 2; bytes = realloc(bytes, cap); }
      }
      fclose(f);
    }
  }
  int np = 0;
  AxGlbPrim *prims = bytes ? ax_glb_parse(bytes, nbytes, &np) : NULL;
  free(bytes);
  if (!prims) return;
  AxStr *kglb = ax_internz("glb"), *kinl = ax_internz("inline"), *kmat = ax_internz("material");
  AxStr *kv = ax_internz("vertices"), *ki = ax_internz("indices");
  ax_dict_set(res, kv, prim_vertices(&prims[0]));
  ax_dict_set(res, ki, prim_indices(&prims[0]));
  ax_dict_set(res, kglb, ax_bool(true));
  if (inl) ax_dict_set(res, kinl, ax_bool(true));
  if (prims[0].material.t != AX_NULL) ax_dict_set(res, kmat, ax_copy(prims[0].material));
  mesh_register(w, d->str2, &prims[0]);
  for (int i = 0; i < np; i++) {
    char nm[512];
    snprintf(nm, sizeof nm, "%s_%d", d->str2->data, i);
    AxStr *pname = ax_internz(nm);
    AxDict *pr = ax_dict_new();
    ax_dict_set(pr, K_kind, str_val(d->str));
    ax_dict_set(pr, K_path, (!inl && d->a) ? str_val(d->a->str) : ax_null());
    ax_dict_set(pr, K_name, str_val(pname));
    ax_dict_set(pr, kv, prim_vertices(&prims[i]));
    ax_dict_set(pr, ki, prim_indices(&prims[i]));
    ax_dict_set(pr, kglb, ax_bool(true));
    if (inl) ax_dict_set(pr, kinl, ax_bool(true));
    if (prims[i].material.t != AX_NULL) ax_dict_set(pr, kmat, ax_copy(prims[i].material));
    ax_dict_set(w->resources, pname, ax_dictv(pr));
    mesh_register(w, pname, &prims[i]);
    ax_release(ax_strv(pname));
  }
  ax_release(ax_strv(kglb)); ax_release(ax_strv(kinl)); ax_release(ax_strv(kmat));
  ax_release(ax_strv(kv)); ax_release(ax_strv(ki));
  // The primitives are kept for the life of the world (render.c draws from them).
}

void ax_engine_init(AxVM *vm, const char *source) {
  init_keys();
  if (!vm->world) vm->world = world_new();
  vm->world->source = source;
}

// The embedding API frees a world with its VM. Entity reference cycles (an entity field holding
// another entity that points back) are not collected, as everywhere else in the runtime.
void ax_engine_free(AxVM *vm) {
  AxWorld *w = vm->world;
  if (!w) return;
  vm->world = NULL;
  for (int i = 0; i < w->nents; i++) ax_release((AxValue){ .t = AX_ENTITY, .o = (AxObj *)w->ents[i] });
  free(w->ents);
  ax_release(ax_dictv(w->tags));
  ax_release(ax_dictv(w->resources));
  ax_release(ax_arrv(w->log));
  ax_release(ax_dictv(w->tick_acc));
  ax_release(ax_dictv(w->channels));
  ax_release(ax_arrv(w->draw_list));
  ax_release(ax_dictv(w->save_slots));
  ax_release(ax_dictv(w->input));
  for (int i = 0; i < w->npending; i++) {
    if (w->pending[i].payload) ax_release(ax_dictv(w->pending[i].payload));
  }
  free(w->pending);
  for (int i = 0; i < w->ntweens; i++) {
    ax_release(w->tweens[i].target);
    ax_release(w->tweens[i].start);
    ax_release(w->tweens[i].end);
  }
  free(w->tweens);
  free(w->diags);
  for (int i = 0; i < w->nmeshes; i++) ax_release(ax_strv(w->meshes[i].name));
  free(w->meshes);
  free(w->events);
  free(w->mixins);
  free(w->materials);
  for (int i = 0; i < w->ndecls; i++) free(w->decls[i].members);   // shared by its entities
  free(w->decls);
  free(w);
}

bool ax_engine_load(AxVM *vm, AxNode *program, const char *source) {
  ax_engine_init(vm, source);
  AxWorld *w = vm->world;
  bool any = false;
  for (int i = 0; i < program->nlist; i++) {
    AxNode *d = program->list[i];
    if (d->kind == N_RESOURCE) {
      AxDict *res = ax_dict_new();
      ax_dict_set(res, K_kind, str_val(d->str));
      ax_dict_set(res, K_path, d->a ? str_val(d->a->str) : ax_null());
      ax_dict_set(res, K_name, str_val(d->str2));
      if (!strcmp(d->str->data, "Mesh3D")) load_glb_resource(w, d, res);
      ax_dict_set(w->resources, d->str2, ax_dictv(res));
    } else if (d->kind == N_EVENT) PUSH(w->events, w->nevents, d);
    else if (d->kind == N_MIXIN) PUSH(w->mixins, w->nmixins, d);
    else if (d->kind == N_MATERIAL) PUSH(w->materials, w->nmaterials, d);
  }
  for (int i = 0; i < program->nlist; i++) {
    AxNode *d = program->list[i];
    if (d->kind != N_ENTITY) continue;
    any = true;
    AxNode **members;
    int n;
    compose_mixins(w, d, &members, &n);
    w->decls = realloc(w->decls, sizeof(*w->decls) * (w->ndecls + 1));
    w->decls[w->ndecls].name = d->str;
    w->decls[w->ndecls].base = d->str2;
    w->decls[w->ndecls].members = members;
    w->decls[w->ndecls].nmembers = n;
    w->ndecls++;
    entity_new(vm, d->str, d->str, d->str2, members, n);
  }
  // `@E at v3(...)` — initial poses, applied once every entity exists.
  for (int i = 0; i < program->nlist; i++) {
    AxNode *d = program->list[i];
    if (d->kind != N_ENTITY || !d->a) continue;
    AxValue ev;
    if (!ax_dict_get(w->tags, d->str, &ev)) continue;
    AxEntity *e = (AxEntity *)ev.o;
    ax_release(ev);
    AxXform *pose = ent_pose(e);
    AxCtx saved = vm->ctx;
    vm->ctx.entity = e; vm->ctx.in_fn = false; vm->ctx.dt = 0; vm->ctx.payload = NULL;
    AxScope *scope = ax_scope_enter(vm, vm->globals, false);
    AxValue val = ax_eval(vm, d->a, scope);
    ax_scope_exit(vm, scope);
    vm->ctx = saved;
    if (val.t == AX_VEC3 && pose) {
      xform_set(pose, K_pos, ax_copy(val));
      ax_dict_set(e->locals, K_position, ax_vec2(ax_vecp(val)->x, ax_vecp(val)->y));
    } else if (val.t == AX_XFORM && pose) {
      AxXform *x = (AxXform *)val.o;
      xform_set(pose, K_pos, ax_copy(x->pos));
      xform_set(pose, K_rot, ax_copy(x->rot));
      xform_set(pose, K_scl, ax_copy(x->scl));
    }
    ax_release(val);
  }
  return any;
}

int ax_engine_entity_count(AxVM *vm) { return W(vm) ? W(vm)->nents : 0; }

void ax_engine_summary(AxVM *vm, FILE *out) {
  AxWorld *w = W(vm);
  fprintf(out, "%d entities (", w->nents);
  for (int i = 0; i < w->nents; i++) fprintf(out, "%s%s", i ? ", " : "", w->ents[i]->name->data);
  fprintf(out, ")");
}

// print(), !log and !print all land in the world log; the console copy is suppressed in --json
// mode. Returns true when the caller should NOT also print.
bool ax_engine_log_msg(AxVM *vm, AxStr *msg) {
  AxWorld *w = W(vm);
  if (!w) return false;
  ax_arr_push(w->log, str_val(msg));
  return w->suppress_console;
}
void ax_engine_log(AxVM *vm, const char *msg) { AxStr *s = ax_str_newz(msg); ax_engine_log_msg(vm, s); ax_release(ax_strv(s)); }
void ax_engine_quiet(AxVM *vm, bool quiet) { if (W(vm)) W(vm)->suppress_console = quiet; }

void ax_engine_set_input(AxVM *vm, double mx, double my, bool jump, bool fire) {
  AxWorld *w = W(vm);
  if (!w) return;
  AxValue mv;
  if (ax_dict_get(w->input, K_move, &mv)) {
    if (mv.t == AX_VEC2) { ax_vecp(mv)->x = mx; ax_vecp(mv)->y = my; }
    ax_release(mv);
  }
  ax_dict_set(w->input, K_jump, ax_bool(jump));
  ax_dict_set(w->input, K_fire, ax_bool(fire));
}

// ---------------------------------------------------------------------------------------------
// Broadcasts (World.deliverBroadcast)
// ---------------------------------------------------------------------------------------------

static void deliver(AxVM *vm, PendingEvent *ev) {
  AxWorld *w = W(vm);
  for (int i = 0; i < w->nents; i++) {
    AxEntity *e = w->ents[i];
    if (e->pending_remove) continue;
    if (ev->mode == 1) {
      if (e != resolve_tag(w, ev->target)) continue;
    } else if (ev->mode == 2) {
      AxXform *p = ent_pose(e);
      V3 pos = p ? v3of(p->pos) : v3(0, 0, 0);
      double d = hyp3(pos.x - ev->origin.x, pos.y - ev->origin.y, pos.z - ev->origin.z);
      if (d > ev->radius) continue;
    }
    run_blocks_named(vm, e, K_on, 1.0 / 60, ev->payload, ev->name);
  }
}

// ---------------------------------------------------------------------------------------------
// Physics: gravity, ground plane, collisions (stepPhysics / stepGroundPlane / stepCollisions)
// ---------------------------------------------------------------------------------------------

typedef struct { int kind; V3 center, half, a, b; double r, h; } Collider;   // 1 sphere, 2 box, 3 capsule

static bool collider_of(AxEntity *e, Collider *c) {
  AxValue hit;
  if (!field_get(e, K_hit, &hit)) return false;
  bool ok = false;
  AxXform *pose = ent_pose(e);
  if (hit.t == AX_SHAPE && pose) {
    AxShape *s = (AxShape *)hit.o;
    V3 pos = v3of(pose->pos);
    c->center = pos;
    if (s->kind == K_sphere) {
      c->kind = 1;
      c->r = (s->nparams > 0 && s->params[0].t == AX_NUM) ? s->params[0].num : 0.5;
      ok = true;
    } else if (s->kind == K_box) {
      if (s->nparams > 0 && (s->params[0].t == AX_VEC3 || s->params[0].t == AX_VEC2 || s->params[0].t == AX_QUAT)) {
        V3 v = v3of(s->params[0]);
        c->kind = 2;
        c->half = v3(v.x / 2, v.y / 2, v.z / 2);
        ok = true;
      }
    } else if (s->kind == K_capsule) {
      c->kind = 3;
      c->r = (s->nparams > 0 && s->params[0].t == AX_NUM) ? s->params[0].num : 0.5;
      c->h = (s->nparams > 1 && s->params[1].t == AX_NUM) ? s->params[1].num : 1;
      ok = true;
    }
  }
  ax_release(hit);
  return ok;
}

static void refresh_capsule(Collider *c, AxXform *pose) {
  V3 axis = qrot(qof(pose->rot), v3(0, 1, 0));
  double half = c->h / 2;
  V3 p = v3of(pose->pos);
  c->center = p;
  c->a = v3(p.x + axis.x * half, p.y + axis.y * half, p.z + axis.z * half);
  c->b = v3(p.x - axis.x * half, p.y - axis.y * half, p.z - axis.z * half);
}

typedef struct { bool hit; V3 normal; double depth; } MTV;

static MTV mtv_from(double dx, double dy, double dz, double overlap) {
  MTV m = { false, { 0, 0, 0 }, 0 };
  if (overlap <= 0) return m;
  double dist = hyp3(dx, dy, dz);
  m.hit = true;
  m.depth = overlap;
  m.normal = dist < 1e-9 ? v3(1, 0, 0) : v3(dx / dist, dy / dist, dz / dist);
  return m;
}

static MTV sphere_sphere(Collider *a, Collider *b) {
  double dx = a->center.x - b->center.x, dy = a->center.y - b->center.y, dz = a->center.z - b->center.z;
  double dist = hyp3(dx, dy, dz);
  return mtv_from(dx, dy, dz, a->r + b->r - dist);
}

static MTV sphere_box(V3 sc, double sr, Collider *b) {
  double cx = js_max(b->center.x - b->half.x, js_min(sc.x, b->center.x + b->half.x));
  double cy = js_max(b->center.y - b->half.y, js_min(sc.y, b->center.y + b->half.y));
  double cz = js_max(b->center.z - b->half.z, js_min(sc.z, b->center.z + b->half.z));
  double dx = sc.x - cx, dy = sc.y - cy, dz = sc.z - cz;
  double dist = hyp3(dx, dy, dz);
  double overlap = sr - dist;
  MTV m = { false, { 0, 0, 0 }, 0 };
  if (overlap <= 0) return m;
  m.hit = true;
  if (dist < 1e-9) {
    double ex = b->half.x - fabs(sc.x - b->center.x);
    double ey = b->half.y - fabs(sc.y - b->center.y);
    double ez = b->half.z - fabs(sc.z - b->center.z);
    double minE = js_min(ex, js_min(ey, ez));
    if (minE == ex) { m.normal = v3(sc.x > b->center.x ? 1 : -1, 0, 0); m.depth = ex + sr; return m; }
    if (minE == ey) { m.normal = v3(0, sc.y > b->center.y ? 1 : -1, 0); m.depth = ey + sr; return m; }
    m.normal = v3(0, 0, sc.z > b->center.z ? 1 : -1); m.depth = ez + sr; return m;
  }
  m.normal = v3(dx / dist, dy / dist, dz / dist);
  m.depth = overlap;
  return m;
}

static MTV box_box(Collider *a, Collider *b) {
  double ax = a->center.x - b->center.x, ay = a->center.y - b->center.y, az = a->center.z - b->center.z;
  double ox = (a->half.x + b->half.x) - fabs(ax);
  double oy = (a->half.y + b->half.y) - fabs(ay);
  double oz = (a->half.z + b->half.z) - fabs(az);
  MTV m = { false, { 0, 0, 0 }, 0 };
  if (ox <= 0 || oy <= 0 || oz <= 0) return m;
  m.hit = true;
  if (ox <= oy && ox <= oz) { m.normal = v3(ax >= 0 ? 1 : -1, 0, 0); m.depth = ox; return m; }
  if (oy <= ox && oy <= oz) { m.normal = v3(0, ay >= 0 ? 1 : -1, 0); m.depth = oy; return m; }
  m.normal = v3(0, 0, az >= 0 ? 1 : -1); m.depth = oz;
  return m;
}

static V3 closest_on_segment(V3 p, V3 a, V3 b) {
  double abx = b.x - a.x, aby = b.y - a.y, abz = b.z - a.z;
  double apx = p.x - a.x, apy = p.y - a.y, apz = p.z - a.z;
  double len2 = abx*abx + aby*aby + abz*abz;
  if (len2 < 1e-12) return a;
  double t = (apx*abx + apy*aby + apz*abz) / len2;
  double tc = t < 0 ? 0 : t > 1 ? 1 : t;
  return v3(a.x + abx * tc, a.y + aby * tc, a.z + abz * tc);
}

static MTV capsule_sphere(Collider *cap, Collider *sph) {
  V3 c = closest_on_segment(sph->center, cap->a, cap->b);
  double dx = c.x - sph->center.x, dy = c.y - sph->center.y, dz = c.z - sph->center.z;
  double dist = hyp3(dx, dy, dz);
  return mtv_from(dx, dy, dz, cap->r + sph->r - dist);
}

static MTV capsule_box(Collider *cap, Collider *bx) {
  MTV best = { false, { 0, 0, 0 }, 0 };
  for (int i = 0; i <= 10; i++) {
    double t = i / 10.0;
    V3 p = v3(cap->a.x + (cap->b.x - cap->a.x) * t, cap->a.y + (cap->b.y - cap->a.y) * t, cap->a.z + (cap->b.z - cap->a.z) * t);
    MTV r = sphere_box(p, cap->r, bx);
    if (r.hit && (!best.hit || r.depth > best.depth)) best = r;
  }
  return best;
}

static void closest_seg_seg(V3 a1, V3 a2, V3 b1, V3 b2, V3 *pa, V3 *pb) {
  double d1x = a2.x - a1.x, d1y = a2.y - a1.y, d1z = a2.z - a1.z;
  double d2x = b2.x - b1.x, d2y = b2.y - b1.y, d2z = b2.z - b1.z;
  double rx = a1.x - b1.x, ry = a1.y - b1.y, rz = a1.z - b1.z;
  double a = d1x*d1x + d1y*d1y + d1z*d1z;
  double e = d2x*d2x + d2y*d2y + d2z*d2z;
  double f = d2x*rx + d2y*ry + d2z*rz;
  double s, t;
  if (a <= 1e-12 && e <= 1e-12) { s = 0; t = 0; }
  else if (a <= 1e-12) { s = 0; t = js_max(0, js_min(1, f / e)); }
  else {
    double c = d1x*rx + d1y*ry + d1z*rz;
    if (e <= 1e-12) { t = 0; s = js_max(0, js_min(1, -c / a)); }
    else {
      double b = d1x*d2x + d1y*d2y + d1z*d2z;
      double denom = a * e - b * b;
      s = denom > 1e-12 ? js_max(0, js_min(1, (b * f - c * e) / denom)) : 0;
      t = (b * s + f) / e;
      if (t < 0) { t = 0; s = js_max(0, js_min(1, -c / a)); }
      else if (t > 1) { t = 1; s = js_max(0, js_min(1, (b - c) / a)); }
    }
  }
  *pa = v3(a1.x + d1x * s, a1.y + d1y * s, a1.z + d1z * s);
  *pb = v3(b1.x + d2x * t, b1.y + d2y * t, b1.z + d2z * t);
}

static MTV capsule_capsule(Collider *a, Collider *b) {
  V3 pa, pb;
  closest_seg_seg(a->a, a->b, b->a, b->b, &pa, &pb);
  double dx = pa.x - pb.x, dy = pa.y - pb.y, dz = pa.z - pb.z;
  double dist = hyp3(dx, dy, dz);
  return mtv_from(dx, dy, dz, a->r + b->r - dist);
}

static MTV flip(MTV m) { if (m.hit) m.normal = v3(-m.normal.x, -m.normal.y, -m.normal.z); return m; }

static MTV test_colliders(Collider *a, Collider *b) {
  MTV none = { false, { 0, 0, 0 }, 0 };
  if (a->kind == 1 && b->kind == 1) return sphere_sphere(a, b);
  if (a->kind == 1 && b->kind == 2) return sphere_box(a->center, a->r, b);
  if (a->kind == 2 && b->kind == 1) return flip(sphere_box(b->center, b->r, a));
  if (a->kind == 2 && b->kind == 2) return box_box(a, b);
  if (a->kind == 3 && b->kind == 1) return capsule_sphere(a, b);
  if (a->kind == 1 && b->kind == 3) return flip(capsule_sphere(b, a));
  if (a->kind == 3 && b->kind == 2) return capsule_box(a, b);
  if (a->kind == 2 && b->kind == 3) return flip(capsule_box(b, a));
  if (a->kind == 3 && b->kind == 3) return capsule_capsule(a, b);
  return none;
}

static int32_t to_int32(double d) {
  if (!isfinite(d)) return 0;
  double t = trunc(d);
  double m = fmod(t, 4294967296.0);
  if (m < 0) m += 4294967296.0;
  uint32_t u = (uint32_t)m;
  return (int32_t)u;
}

static double field_raw_num(AxEntity *e, AxStr *name, bool *present) {
  AxValue v;
  *present = field_get(e, name, &v);
  if (!*present) return 0;
  double d = ax_to_num(v);
  ax_release(v);
  return d;
}

static AxDict *collide_payload(AxEntity *other, V3 normal, double depth) {
  AxDict *p = ax_dict_new();
  ax_dict_set(p, K_other, str_val(other->name));
  ax_dict_set(p, K_other_entity, ent_val(other));
  ax_dict_set(p, K_normal, mk3(normal));
  ax_dict_set(p, K_depth, ax_num(depth));
  return p;
}

// ---- collisions ------------------------------------------------------------------------------
//
// The reference tests every pair (i < j) in order, and a hit moves bodies before the next pair is
// tested, so later pairs see earlier pushes. The broadphase below reproduces that pass exactly
// — the same pairs hit, in the same order, with the same arithmetic — while testing only pairs
// whose bounding boxes touch:
//
//   * every narrowphase test (sphere, box, capsule, all combinations) reports "no hit" when the
//     two bounding boxes (centre ± |extent|, capsules as their segment ± |r|) are apart, so
//     skipping such a pair changes nothing. Boxes are widened by a relative 1e-9 so rounding in
//     the narrowphase cannot hit a pair the box test skipped;
//   * NaN compares false, so a body with a non-finite coordinate or extent "hits" everything —
//     those bodies (and very large ones) are tested against every other body;
//   * row i's candidates are gathered from the grid as it stands; a hit that moves body i
//     re-gathers the rest of the row from i's new box, and every body a hit moves is re-filed.
//     Nothing else moves during the pass (no program code runs in it).
//
// Below GRID_MIN bodies the plain double loop is used. AXIOM_BROADPHASE=grid|pairs forces one.

typedef struct {
  AxEntity *e;
  AxXform *pose;
  Collider c;
  double layer, mask;
  bool dyn;
} Body;

typedef struct { AxEntity *to; AxDict *payload; } Contact;
typedef struct { Contact *items; int n, cap; } Contacts;

static void contact_push(Contacts *cs, AxEntity *to, AxDict *payload) {
  if (cs->n == cs->cap) { cs->cap = cs->cap ? cs->cap * 2 : 16; cs->items = realloc(cs->items, sizeof(Contact) * cs->cap); }
  cs->items[cs->n].to = to;
  cs->items[cs->n].payload = payload;
  cs->n++;
}

// Test and resolve one pair (interpreter.js stepCollisions, loop body). Returns which bodies
// moved: 1 = A, 2 = B.
static int collide_pair(Body *A, Body *B, Contacts *cs) {
  if (A->layer != 0 && B->layer != 0) {
    if ((to_int32(A->mask) & to_int32(B->layer)) == 0 || (to_int32(B->mask) & to_int32(A->layer)) == 0) return 0;
  }
  AxXform *poseA = A->pose, *poseB = B->pose;
  A->c.center = v3of(poseA->pos);
  B->c.center = v3of(poseB->pos);
  if (A->c.kind == 3) refresh_capsule(&A->c, poseA);
  if (B->c.kind == 3) refresh_capsule(&B->c, poseB);
  MTV m = test_colliders(&A->c, &B->c);
  if (!m.hit) return 0;
  V3 push = v3mul(m.normal, m.depth / 2);
  bool dynA = A->dyn, dynB = B->dyn;
  if (dynA && dynB) {
    xform_set(poseA, K_pos, mk3(v3add(v3of(poseA->pos), push)));
    xform_set(poseB, K_pos, mk3(v3sub(v3of(poseB->pos), push)));
  } else if (dynA) {
    xform_set(poseA, K_pos, mk3(v3add(v3of(poseA->pos), v3mul(push, 2))));
  } else if (dynB) {
    xform_set(poseB, K_pos, mk3(v3sub(v3of(poseB->pos), v3mul(push, 2))));
  }
  if (dynA && dynB) {
    V3 rel = v3sub(v3of(poseA->vel), v3of(poseB->vel));
    double vn = rel.x * m.normal.x + rel.y * m.normal.y + rel.z * m.normal.z;
    if (vn < 0) {
      double mA = field_num_or(A->e, K_mass, 1), mB = field_num_or(B->e, K_mass, 1);
      double impulse = (2 * vn) / (1 / mA + 1 / mB);
      xform_set(poseA, K_vel, mk3(v3sub(v3of(poseA->vel), v3mul(m.normal, impulse / mA))));
      xform_set(poseB, K_vel, mk3(v3add(v3of(poseB->vel), v3mul(m.normal, impulse / mB))));
    }
  } else if (dynA) {
    V3 va = v3of(poseA->vel);
    double vn = va.x * m.normal.x + va.y * m.normal.y + va.z * m.normal.z;
    if (vn < 0) xform_set(poseA, K_vel, mk3(v3sub(va, v3mul(m.normal, 2 * vn))));
  } else if (dynB) {
    V3 vb = v3of(poseB->vel);
    double vn = vb.x * (-m.normal.x) + vb.y * (-m.normal.y) + vb.z * (-m.normal.z);
    if (vn < 0) xform_set(poseB, K_vel, mk3(v3sub(vb, v3mul(m.normal, -2 * vn))));
  }
  contact_push(cs, A->e, collide_payload(B->e, m.normal, m.depth));
  contact_push(cs, B->e, collide_payload(A->e, v3(-m.normal.x, -m.normal.y, -m.normal.z), m.depth));
  return (dynA ? 1 : 0) | (dynB ? 2 : 0);
}

// The body's current bounding box, widened; false when any bound is not finite.
static bool body_box(Body *b, double lo[3], double hi[3]) {
  V3 p = v3of(b->pose->pos);
  double ext[3];
  if (b->c.kind == 3) {
    Collider c = b->c;
    refresh_capsule(&c, b->pose);
    double r = fabs(c.r);
    lo[0] = fmin(c.a.x, c.b.x) - r; hi[0] = fmax(c.a.x, c.b.x) + r;
    lo[1] = fmin(c.a.y, c.b.y) - r; hi[1] = fmax(c.a.y, c.b.y) + r;
    lo[2] = fmin(c.a.z, c.b.z) - r; hi[2] = fmax(c.a.z, c.b.z) + r;
    if (isnan(c.a.x) || isnan(c.a.y) || isnan(c.a.z) || isnan(c.b.x) || isnan(c.b.y) || isnan(c.b.z) || isnan(r)) return false;
  } else {
    if (b->c.kind == 1) ext[0] = ext[1] = ext[2] = fabs(b->c.r);
    else { ext[0] = fabs(b->c.half.x); ext[1] = fabs(b->c.half.y); ext[2] = fabs(b->c.half.z); }
    double pc[3] = { p.x, p.y, p.z };
    for (int k = 0; k < 3; k++) { lo[k] = pc[k] - ext[k]; hi[k] = pc[k] + ext[k]; }
  }
  for (int k = 0; k < 3; k++) {
    if (!isfinite(lo[k]) || !isfinite(hi[k])) return false;
    double m = (fabs(lo[k]) + fabs(hi[k])) * 1e-9 + 1e-9;
    lo[k] -= m;
    hi[k] += m;
  }
  return true;
}

#define GRID_MIN 24
#define GRID_SPAN 16          // a box wider than this many cells on an axis is tested against all
#define GRID_LIMIT 1000000.0  // cell coordinates are kept well inside the packed key

typedef struct { int64_t key; int *ids; int n, cap; bool used; } Cell;

typedef struct {
  double inv;                 // 1 / cell size
  Cell *cells;
  uint32_t mask;              // table size - 1 (a power of two)
  int (*range)[6];            // per body: the cells it is filed under (lo xyz, hi xyz)
  uint8_t *state;             // 0 not filed, 1 in cells, 2 in the everyone list
  int *all, nall;             // bodies tested against everyone (non-finite or huge)
  int *stamp, qid;
} Grid;

static int64_t cell_key(int x, int y, int z) {
  return ((int64_t)(x + (1 << 20)) << 42) | ((int64_t)(y + (1 << 20)) << 21) | (int64_t)(z + (1 << 20));
}

static Cell *grid_cell(Grid *g, int64_t key, bool create) {
  uint64_t h = (uint64_t)key * 0x9E3779B97F4A7C15ull;
  for (uint32_t i = (uint32_t)(h >> 32) & g->mask;; i = (i + 1) & g->mask) {
    Cell *c = &g->cells[i];
    if (!c->used) {
      if (!create) return NULL;
      c->used = true;
      c->key = key;
      return c;
    }
    if (c->key == key) return c;
  }
}

static void grid_remove(Grid *g, int id) {
  if (g->state[id] == 1) {
    int *r = g->range[id];
    for (int x = r[0]; x <= r[3]; x++) for (int y = r[1]; y <= r[4]; y++) for (int z = r[2]; z <= r[5]; z++) {
      Cell *c = grid_cell(g, cell_key(x, y, z), false);
      if (!c) continue;
      for (int k = 0; k < c->n; k++) if (c->ids[k] == id) { c->ids[k] = c->ids[--c->n]; break; }
    }
  } else if (g->state[id] == 2) {
    for (int k = 0; k < g->nall; k++) if (g->all[k] == id) { g->all[k] = g->all[--g->nall]; break; }
  }
  g->state[id] = 0;
}

static void grid_file(Grid *g, Body *bodies, int id) {
  double lo[3], hi[3];
  int r[6];
  bool everyone = !body_box(&bodies[id], lo, hi);
  if (!everyone) {
    for (int k = 0; k < 3; k++) {
      double a = floor(lo[k] * g->inv), b = floor(hi[k] * g->inv);
      if (a < -GRID_LIMIT || b > GRID_LIMIT || b - a >= GRID_SPAN) { everyone = true; break; }
      r[k] = (int)a;
      r[k + 3] = (int)b;
    }
  }
  if (everyone) {
    g->all[g->nall++] = id;
    g->state[id] = 2;
    return;
  }
  memcpy(g->range[id], r, sizeof r);
  for (int x = r[0]; x <= r[3]; x++) for (int y = r[1]; y <= r[4]; y++) for (int z = r[2]; z <= r[5]; z++) {
    Cell *c = grid_cell(g, cell_key(x, y, z), true);
    if (c->n == c->cap) { c->cap = c->cap ? c->cap * 2 : 4; c->ids = realloc(c->ids, sizeof(int) * c->cap); }
    c->ids[c->n++] = id;
  }
  g->state[id] = 1;
}

static int cmp_int(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }

// The bodies j > after that row i must test, ascending.
static int grid_gather(Grid *g, Body *bodies, int n, int i, int after, int *out) {
  int k = 0;
  g->qid++;
  double lo[3], hi[3];
  bool everyone = !body_box(&bodies[i], lo, hi);
  int r[6];
  if (!everyone) {
    for (int a = 0; a < 3; a++) {
      double p = floor(lo[a] * g->inv), q = floor(hi[a] * g->inv);
      if (p < -GRID_LIMIT || q > GRID_LIMIT || q - p >= GRID_SPAN) { everyone = true; break; }
      r[a] = (int)p;
      r[a + 3] = (int)q;
    }
  }
  if (everyone) {
    for (int j = after + 1; j < n; j++) out[k++] = j;
    return k;
  }
  for (int x = r[0]; x <= r[3]; x++) for (int y = r[1]; y <= r[4]; y++) for (int z = r[2]; z <= r[5]; z++) {
    Cell *c = grid_cell(g, cell_key(x, y, z), false);
    if (!c) continue;
    for (int m = 0; m < c->n; m++) {
      int j = c->ids[m];
      if (j > after && g->stamp[j] != g->qid) { g->stamp[j] = g->qid; out[k++] = j; }
    }
  }
  for (int m = 0; m < g->nall; m++) {
    int j = g->all[m];
    if (j > after && g->stamp[j] != g->qid) { g->stamp[j] = g->qid; out[k++] = j; }
  }
  qsort(out, (size_t)k, sizeof(int), cmp_int);
  return k;
}

static int cmp_dbl(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

static void collide_grid(Body *bodies, int n, Contacts *cs) {
  Grid g = { 0 };
  // Cell size: 1.5 × the median box size, so a typical body is filed under a few cells.
  double *sizes = malloc(sizeof(double) * n);
  int ns = 0;
  for (int i = 0; i < n; i++) {
    double lo[3], hi[3];
    if (!body_box(&bodies[i], lo, hi)) continue;
    sizes[ns++] = fmax(hi[0] - lo[0], fmax(hi[1] - lo[1], hi[2] - lo[2]));
  }
  double cell = 1;
  if (ns) { qsort(sizes, (size_t)ns, sizeof(double), cmp_dbl); cell = sizes[ns / 2] * 1.5; }
  free(sizes);
  if (!(cell > 1e-6) || !isfinite(cell)) cell = 1;
  g.inv = 1 / cell;
  uint32_t cap = 64;
  while (cap < (uint32_t)n * 32) cap <<= 1;
  g.cells = calloc(cap, sizeof(Cell));
  g.mask = cap - 1;
  g.range = malloc(sizeof(*g.range) * n);
  g.state = calloc((size_t)n, 1);
  g.all = malloc(sizeof(int) * n);
  g.stamp = calloc((size_t)n, sizeof(int));
  int *cand = malloc(sizeof(int) * n);
  for (int i = 0; i < n; i++) grid_file(&g, bodies, i);
  for (int i = 0; i < n; i++) {
    grid_remove(&g, i);                        // later rows only look at bodies after them
    int after = i;
    for (;;) {
      int k = grid_gather(&g, bodies, n, i, after, cand);
      bool again = false;
      for (int m = 0; m < k; m++) {
        int j = cand[m];
        int moved = collide_pair(&bodies[i], &bodies[j], cs);
        if (moved & 2) { grid_remove(&g, j); grid_file(&g, bodies, j); }
        if (moved & 1) { after = j; again = m + 1 < k || j + 1 < n; break; }
      }
      if (!again) break;
    }
  }
  for (uint32_t c = 0; c < cap; c++) free(g.cells[c].ids);
  free(g.cells); free(g.range); free(g.state); free(g.all); free(g.stamp); free(cand);
}

static int broadphase_mode = -1;   // 0 pairs, 1 grid, 2 by count

static void step_collisions(AxVM *vm) {
  AxWorld *w = W(vm);
  if (broadphase_mode < 0) {
    const char *m = getenv("AXIOM_BROADPHASE");
    broadphase_mode = m && !strcmp(m, "pairs") ? 0 : m && !strcmp(m, "grid") ? 1 : 2;
  }
  int n = 0;
  Body *bodies = malloc(sizeof(Body) * (w->nents + 1));
  for (int i = 0; i < w->nents; i++) {
    AxEntity *e = w->ents[i];
    if (e->pending_remove) continue;
    Body *b = &bodies[n];
    if (!collider_of(e, &b->c)) continue;
    // Layer, mask and kind cannot change during the pass: read them once per body, not per pair.
    bool pm;
    b->e = e;
    b->pose = ent_pose(e);
    b->layer = field_num_or(e, K_collision_layer, 0);
    b->mask = field_raw_num(e, K_collision_mask, &pm);
    if (!pm) b->mask = 4294967295.0;
    b->dyn = e->base == K_Body3D;
    n++;
  }
  Contacts cs = { 0 };
  if (broadphase_mode == 1 || (broadphase_mode == 2 && n >= GRID_MIN)) collide_grid(bodies, n, &cs);
  else {
    for (int i = 0; i < n; i++)
      for (int j = i + 1; j < n; j++) collide_pair(&bodies[i], &bodies[j], &cs);
  }
  // Contacts are delivered after the whole pass, so a handler sees resolved positions.
  for (int i = 0; i < cs.n; i++) {
    if (!cs.items[i].to->pending_remove) run_blocks_named(vm, cs.items[i].to, K_on, 1.0 / 60, cs.items[i].payload, K_Collide);
    ax_release(ax_dictv(cs.items[i].payload));
  }
  free(cs.items);
  free(bodies);
}

static void step_ground_plane(AxWorld *w) {
  for (int i = 0; i < w->nents; i++) {
    AxEntity *e = w->ents[i];
    if (e->pending_remove || e->base != K_Body3D) continue;
    AxXform *p = ent_pose(e);
    if (!p || p->pos.t < AX_VEC2 || p->pos.t > AX_VEC3) continue;
    AxVec *pos = ax_vecp(p->pos);
    if (pos->y < 0) {
      pos->y = 0;   // in place, as the reference mutates the shared vector
      if (p->vel.t == AX_VEC3 || p->vel.t == AX_VEC2) { AxVec *vel = ax_vecp(p->vel); if (vel->y < 0) vel->y = 0; }
    }
  }
}

static double raycast_collider(Collider *c, V3 o, V3 d) {
  if (c->kind == 1) {
    double ox = o.x - c->center.x, oy = o.y - c->center.y, oz = o.z - c->center.z;
    double a = d.x*d.x + d.y*d.y + d.z*d.z;
    double b = 2 * (ox*d.x + oy*d.y + oz*d.z);
    double cc = ox*ox + oy*oy + oz*oz - c->r*c->r;
    double disc = b*b - 4*a*cc;
    if (disc < 0) return NAN;
    double sq = sqrt(disc);
    double t1 = (-b - sq) / (2 * a), t2 = (-b + sq) / (2 * a);
    if (t1 >= 0) return t1;
    if (t2 >= 0) return t2;
    return NAN;
  }
  if (c->kind == 2) {
    double tmin = -INFINITY, tmax = INFINITY;
    double oo[3] = { o.x, o.y, o.z }, dd[3] = { d.x, d.y, d.z };
    double cc[3] = { c->center.x, c->center.y, c->center.z }, hh[3] = { c->half.x, c->half.y, c->half.z };
    for (int k = 0; k < 3; k++) {
      if (fabs(dd[k]) < 1e-9) {
        if (oo[k] < cc[k] - hh[k] || oo[k] > cc[k] + hh[k]) return NAN;
      } else {
        double lo = (cc[k] - hh[k] - oo[k]) / dd[k], hi = (cc[k] + hh[k] - oo[k]) / dd[k];
        double tLo = js_min(lo, hi), tHi = js_max(lo, hi);
        if (tLo > tmin) tmin = tLo;
        if (tHi < tmax) tmax = tHi;
        if (tmin > tmax) return NAN;
      }
    }
    if (tmin >= 0) return tmin;
    if (tmax >= 0) return tmax;
    return NAN;
  }
  if (c->kind == 3) {
    double best = NAN;
    for (int i = 0; i <= 16; i++) {
      double t = i / 16.0;
      Collider s = *c;
      s.kind = 1;
      s.center = v3(c->a.x + (c->b.x - c->a.x) * t, c->a.y + (c->b.y - c->a.y) * t, c->a.z + (c->b.z - c->a.z) * t);
      double tt = raycast_collider(&s, o, d);
      if (!isnan(tt) && tt >= 0 && (isnan(best) || tt < best)) best = tt;
    }
    return best;
  }
  return NAN;
}

static AxValue raycast_world(AxWorld *w, V3 origin, V3 dir, double max_dist) {
  if (!w) return ax_null();
  V3 d = v3mag(dir) > 1e-9 ? v3norm(dir) : dir;
  double best_t = max_dist;
  bool found = false;
  V3 hit = { 0, 0, 0 };
  for (int i = 0; i < w->nents; i++) {
    AxEntity *e = w->ents[i];
    if (e->pending_remove) continue;
    Collider c;
    if (!collider_of(e, &c)) continue;
    if (c.kind == 3) { AxXform *p = ent_pose(e); refresh_capsule(&c, p); }
    double t = raycast_collider(&c, origin, d);
    if (!isnan(t) && t >= 0 && t < best_t) {
      best_t = t;
      found = true;
      hit = v3(origin.x + d.x * t, origin.y + d.y * t, origin.z + d.z * t);
    }
  }
  return found ? mk3(hit) : ax_null();
}

// ---------------------------------------------------------------------------------------------
// Tweens (class Tween)
// ---------------------------------------------------------------------------------------------

static double ease(const char *name, double t) {
  if (strcmp(name, "in") == 0) return t * t;
  if (strcmp(name, "out") == 0) return t * (2 - t);
  if (strcmp(name, "inout") == 0) return t < 0.5 ? 2 * t * t : -1 + (4 - 2 * t) * t;
  if (strcmp(name, "bounce") == 0) { if (t < 0.5) return 4 * t * t * t; double f = (2 * t) - 2; return 0.5 * f * f * f + 1; }
  if (strcmp(name, "elastic") == 0) { if (t == 0 || t == 1) return t; return -pow(2, 10 * (t - 1)) * sin((t - 1.1) * 5 * M_PI); }
  return t;
}

// obj[prop], falling back to obj.get(prop) — which is how the reference walks `pose.pos` on an
// entity (an EntityInstance has no own `pose` property; its get() finds it).
static AxValue tween_get(AxVM *vm, AxValue obj, AxStr *prop) {
  AxValue out = ax_null();
  if (obj.t == AX_ENTITY) { if (!ax_entity_get((AxEntity *)obj.o, prop, &out)) out = ax_null(); return out; }
  if (obj.t == AX_DICT) { if (!ax_dict_get((AxDict *)obj.o, prop, &out)) out = ax_null(); return out; }
  if (obj.t >= AX_VEC2) { ax_engine_member(vm, obj, prop, &out); return out; }
  return ax_null();
}

static AxValue tween_walk(AxVM *vm, Tween *tw) {
  AxValue obj = ax_copy(tw->target);
  for (int i = 0; i < tw->npath - 1; i++) {
    AxValue v = tween_get(vm, obj, tw->path[i]);
    ax_release(obj);
    if (v.t == AX_NULL) { tw->done = true; return ax_null(); }
    obj = v;
  }
  return obj;
}

static void tween_tick(AxVM *vm, Tween *tw, double dt) {
  if (tw->done) return;
  if (tw->target.t == AX_ENTITY && ((AxEntity *)tw->target.o)->pending_remove) { tw->done = true; return; }
  AxStr *last = tw->path[tw->npath - 1];
  if (!tw->started) {
    AxValue obj = tween_walk(vm, tw);
    if (obj.t == AX_NULL) return;
    AxValue sv = tween_get(vm, obj, last);
    tw->start = clone_value(sv);
    ax_release(sv);
    ax_release(obj);
    tw->started = true;
  }
  tw->elapsed += dt;
  double t = js_clamp(tw->elapsed / tw->duration, 0, 1);
  double et = ease(tw->ease, t);
  AxValue obj = tween_walk(vm, tw);
  if (obj.t == AX_NULL) return;
  AxValue val;
  AxValue s = tw->start;
  if (s.t == AX_VEC2 || s.t == AX_VEC3) {
    AxValue a[2] = { tw->end, ax_num(et) };
    ax_engine_method(vm, s, K_lerp, a, 2, &val);
  } else if (s.t == AX_NUM) {
    val = ax_num(s.num + (ax_to_num(tw->end) - s.num) * et);
  } else {
    val = ax_copy(tw->end);
  }
  if (obj.t == AX_ENTITY) ax_entity_set((AxEntity *)obj.o, last, val);
  else if (obj.t == AX_DICT) ax_dict_set((AxDict *)obj.o, last, val);
  else if (obj.t >= AX_VEC2) ax_engine_set_member(vm, obj, last, val);
  else ax_release(val);
  ax_release(obj);
  if (t >= 1) tw->done = true;
}

// ---------------------------------------------------------------------------------------------
// The frame loop (World.update / stepPhysics / stepCognition / stepRender)
// ---------------------------------------------------------------------------------------------

static void sweep_removed(AxWorld *w) {
  int k = 0;
  for (int i = 0; i < w->nents; i++) {
    AxEntity *e = w->ents[i];
    if (!e->pending_remove) { w->ents[k++] = e; continue; }
    AxValue cur;
    if (ax_dict_get(w->tags, e->name, &cur)) {
      if (cur.o == (AxObj *)e) ax_dict_del(w->tags, e->name);
      ax_release(cur);
    }
    AxValue ev; ev.t = AX_ENTITY; ev.o = (AxObj *)e;
    ax_release(ev);              // the world's own reference
  }
  w->nents = k;
}

static bool any_removed(AxWorld *w) {
  for (int i = 0; i < w->nents; i++) if (w->ents[i]->pending_remove) return true;
  return false;
}

static void step_physics(AxVM *vm, double dt) {
  AxWorld *w = W(vm);
  int np = w->npending;
  PendingEvent *pending = w->pending;
  w->pending = NULL;
  w->npending = w->cappending = 0;
  for (int i = 0; i < np; i++) {
    deliver(vm, &pending[i]);
    ax_release(ax_dictv(pending[i].payload));
  }
  free(pending);
  for (int i = 0; i < w->nents; i++) {
    AxEntity *e = w->ents[i];
    if (e->pending_remove) continue;
    // Timers count down.
    for (uint32_t k = 0; k < e->fields->len; k++) {
      if (e->fields->entries[k].dead) continue;
      AxValue v = e->fields->entries[k].val;
      if (v.t != AX_TIMER) continue;
      AxTimer *t = (AxTimer *)v.o;
      double step = strcmp(t->unit, "ms") == 0 ? dt * 1000 : dt;
      t->remaining = js_max(0, t->remaining - step);
    }
    // &Body3D: gravity and drag.
    if (e->base == K_Body3D) {
      AxXform *pose = ent_pose(e);
      if (pose) {
        if (pose->vel.t == AX_VEC2) xform_set(pose, K_vel, ax_vec3(ax_vecp(pose->vel)->x, 0, ax_vecp(pose->vel)->y));
        double mass = field_num_or(e, K_mass, 1);
        double drag = field_num_or(e, K_drag, 0);
        V3 grav = v3mul(v3(0, -9.8, 0), mass);
        V3 vel = v3of(pose->vel);
        V3 dragf = v3mul(vel, -drag);
        V3 nv = v3add(vel, v3mul(v3div(v3add(grav, dragf), mass), dt));
        xform_set(pose, K_vel, mk3(nv));
        xform_set(pose, K_pos, mk3(v3add(v3of(pose->pos), v3mul(nv, dt))));
      }
    }
    run_blocks_named(vm, e, K_physics, dt, NULL, NULL);
  }
  step_ground_plane(w);
  step_collisions(vm);
  if (any_removed(w)) sweep_removed(w);
  int k = 0;
  for (int i = 0; i < w->ntweens; i++) {
    tween_tick(vm, &w->tweens[i], dt);
    if (!w->tweens[i].done) w->tweens[k++] = w->tweens[i];
    else { ax_release(w->tweens[i].target); ax_release(w->tweens[i].start); ax_release(w->tweens[i].end); }
  }
  w->ntweens = k;
  w->sim_time += dt;
}

static void step_cognition(AxVM *vm, double real_dt) {
  AxWorld *w = W(vm);
  for (int i = 0; i < w->nents; i++) {
    AxEntity *e = w->ents[i];
    if (e->pending_remove) continue;
    for (int m = 0; m < e->nmembers; m++) {
      AxNode *b = e->members[m];
      if (b->kind != N_EBLOCK || b->str != K_tick) continue;
      double hz = b->num ? b->num : 10;
      // The accumulator key is the reference's `decl.name + '::' + block.name + block.freq`.
      char keybuf[256];
      char fq[64];
      if (b->num) ax_fmt_num(b->num, fq, sizeof fq); else snprintf(fq, sizeof fq, "null");
      snprintf(keybuf, sizeof keybuf, "%s::%s%s", e->name->data, b->str->data, fq);
      AxStr *key = ax_internz(keybuf);
      AxValue accv;
      double acc = 0;
      if (ax_dict_get(w->tick_acc, key, &accv)) { acc = accv.num; ax_release(accv); }
      double a = (acc ? acc : 0) + real_dt;
      double period = 1 / hz;
      int ran = 0;
      while (a >= period && ran < 8) { a -= period; run_block(vm, b, e, period, NULL); ran++; }
      ax_dict_set(w->tick_acc, key, ax_num(a));
      ax_release(ax_strv(key));
    }
  }
}

static void step_render(AxVM *vm, double dt) {
  AxWorld *w = W(vm);
  ax_release(ax_arrv(w->draw_list));
  w->draw_list = ax_arr_new(8);
  for (int i = 0; i < w->nents; i++) {
    AxEntity *e = w->ents[i];
    if (e->pending_remove) continue;
    run_blocks_named(vm, e, K_render, dt, NULL, NULL);
  }
}

static void flush_debug_ring(AxWorld *w) {
  if (!w->ring_count) return;
  int start = (w->ring_head - w->ring_count + DEBUG_RING) % DEBUG_RING;
  for (int i = 0; i < w->ring_count; i++) {
    DebugEntry *d = &w->ring[(start + i) % DEBUG_RING];
    char *buf = NULL; size_t len = 0, cap = 0;
    app(&buf, &len, &cap, "");
    for (uint32_t k = 0; k < d->args->len; k++) {
      if (k) app(&buf, &len, &cap, " ");
      AxStr *s = ax_to_str(d->args->items[k]);
      ax_str_append(&buf, &len, &cap, s->data, s->len);
      ax_release(ax_strv(s));
    }
    ax_arr_push(w->log, ax_strv(ax_str_new(buf, len)));
    free(buf);
    ax_release(ax_arrv(d->args));
    d->args = NULL;
  }
  w->ring_head = 0;
  w->ring_count = 0;
}

void ax_engine_update(AxVM *vm, double dt) {
  init_keys();
  AxWorld *w = W(vm);
  if (!w) return;
  w->physics_acc += dt;
  double period = 1.0 / 60;
  int ran = 0;
  while (w->physics_acc >= period && ran < 8) { w->physics_acc -= period; step_physics(vm, period); ran++; }
  step_cognition(vm, dt);
  step_render(vm, dt);
  flush_debug_ring(w);
  if (any_removed(w)) sweep_removed(w);
}

// ---------------------------------------------------------------------------------------------
// Engine expressions: #Tag, ?query(...), dist ~> op
// ---------------------------------------------------------------------------------------------

static AxValue arg_eval(AxVM *vm, AxNode *call, int i, AxScope *scope) {
  return i < call->nlist ? ax_eval(vm, call->list[i]->b, scope) : ax_null();
}

static V3 entity_pos(AxEntity *e) {
  AxXform *p = e ? ent_pose(e) : NULL;
  return p ? v3of(p->pos) : v3(0, 0, 0);
}

extern AxValue ax_nav_query(AxVM *vm, AxEntity *nav, AxStr *name, AxValue *args, int argc);
extern AxValue ax_dist_infer(AxVM *vm, AxValue dist, AxStr *op);
extern void ax_dist_observe(AxVM *vm, AxNode *n, AxScope *scope);

static AxValue global_query(AxVM *vm, AxNode *n, AxScope *scope) {
  AxWorld *w = W(vm);
  AxEntity *self = vm->ctx.entity;
  AxNode *a0 = n->nlist ? n->list[0]->b : NULL;
  const char *name = n->str->data;
  if (strcmp(name, "nearest") == 0) {
    if (!a0 || a0->kind != N_TAGREF) ax_throw(vm, "AX-RUNTIME-000", "'?nearest(...)' requires a #Prefab tag as the first argument");
    AxValue rv = n->nlist > 1 ? ax_eval(vm, n->list[1]->b, scope) : ax_num(INFINITY);
    double radius = rv.t == AX_NUM ? rv.num : INFINITY;
    ax_release(rv);
    if (!self) ax_throw(vm, "AX-RUNTIME-000", "Cannot read properties of null (reading 'locals')");
    V3 me = entity_pos(self);
    AxEntity *best = NULL;
    double best_d = INFINITY;
    for (int i = 0; w && i < w->nents; i++) {
      AxEntity *e = w->ents[i];
      if (e == self || e->prefab != a0->names[0] || e->pending_remove) continue;
      AxXform *p = ent_pose(e);
      if (!p) continue;
      V3 q = v3of(p->pos);
      double d = hyp3(q.x - me.x, q.y - me.y, q.z - me.z);
      if (d < best_d && d <= radius) { best_d = d; best = e; }
    }
    return best ? ent_val(best) : ax_null();
  }
  if (strcmp(name, "exists") == 0) {
    if (!a0 || a0->kind != N_TAGREF) {
      // `exists("path")` is the standard library's file check — the two never overlap.
      AxValue v = a0 ? ax_eval(vm, a0, scope) : ax_null();
      if (v.t == AX_STR) {
        AxValue fn;
        AxStr *k = ax_internz("file_exists");
        bool found = ax_scope_lookup(vm->builtins, k, &fn);
        ax_release(ax_strv(k));
        AxValue r = found ? ax_call(vm, fn, &v, 1) : ax_bool(false);
        if (found) ax_release(fn);
        ax_release(v);
        return r;
      }
      ax_release(v);
      ax_throw(vm, "AX-QUERY-001", "'?exists(...)' takes a #Tag (entity check) or a string (file check)");
    }
    return ax_bool(resolve_tag(w, a0->names[0]) != NULL);
  }
  // dist
  if (!a0 || a0->kind != N_TAGREF) ax_throw(vm, "AX-RUNTIME-000", "'?dist(...)' requires a #Tag argument");
  AxEntity *t = resolve_tag(w, a0->names[0]);
  if (!t || !ent_pose(t)) return ax_num(INFINITY);
  if (!self || !ent_pose(self)) return ax_num(INFINITY);
  V3 a = entity_pos(t), b = entity_pos(self);
  return ax_num(hyp3(a.x - b.x, a.y - b.y, a.z - b.z));
}

static bool is_nav_query(const char *n) {
  return !strcmp(n, "path") || !strcmp(n, "raycast") || !strcmp(n, "block_cell") || !strcmp(n, "unblock_cell") || !strcmp(n, "is_blocked");
}

static AxValue call_query(AxVM *vm, AxNode *n, AxScope *scope) {
  const char *name = n->str->data;
  if (!strcmp(name, "nearest") || !strcmp(name, "exists") || !strcmp(name, "dist")) return global_query(vm, n, scope);
  AxWorld *w = W(vm);
  if (is_nav_query(name) && n->nlist && n->list[0]->b->kind == N_TAGREF) {
    AxEntity *nav = resolve_tag(w, n->list[0]->b->names[0]);
    if (nav && nav->base == K_NavMesh3D) {
      int argc = n->nlist - 1;
      AxValue *args = calloc(argc + 1, sizeof(AxValue));
      for (int i = 0; i < argc; i++) args[i] = ax_eval(vm, n->list[i + 1]->b, scope);
      AxValue r = ax_nav_query(vm, nav, n->str, args, argc);
      for (int i = 0; i < argc; i++) ax_release(args[i]);
      free(args);
      return r;
    }
  }
  AxEntity *self = vm->ctx.entity;
  if (!self) ax_throw(vm, "AX-RUNTIME-000", "Cannot read properties of null (reading 'decl')");
  if (self->base == K_NavMesh3D && is_nav_query(name)) {
    int argc = n->nlist;
    AxValue *args = calloc(argc + 1, sizeof(AxValue));
    for (int i = 0; i < argc; i++) args[i] = ax_eval(vm, n->list[i]->b, scope);
    AxValue r = ax_nav_query(vm, self, n->str, args, argc);
    for (int i = 0; i < argc; i++) ax_release(args[i]);
    free(args);
    return r;
  }
  ax_throw(vm, "AX-RUNTIME-QUERY", "'?%s(...)' is not a declared query on '&%s'", name, self->base ? self->base->data : "undefined");
  return ax_null();
}

AxValue ax_engine_eval(AxVM *vm, AxNode *n, AxScope *scope) {
  init_keys();
  AxWorld *w = W(vm);
  switch (n->kind) {
    case N_TAGREF: {
      AxEntity *e = resolve_tag(w, n->names[0]);
      if (e) {
        AxValue v = ent_val(e);
        for (int i = 1; i < n->nnames; i++) {
          AxValue next = ax_member_get(vm, v, n->names[i]);
          ax_release(v);
          v = next;
        }
        return v;
      }
      AxValue res;
      if (w && ax_dict_get(w->resources, n->names[0], &res)) return res;
      return ax_null();   // a despawned or unknown tag reads as null
    }
    case N_QUERY: {
      if (n->a) { AxValue recv = ax_eval(vm, n->a, scope); ax_release(recv); }
      return call_query(vm, n, scope);
    }
    case N_INFER: {
      AxValue d = ax_eval(vm, n->a, scope);
      AxValue r = ax_dist_infer(vm, d, n->str);
      ax_release(d);
      return r;
    }
    default: return ax_null();
  }
}

// ---------------------------------------------------------------------------------------------
// Actions (interpreter.js execAction)
// ---------------------------------------------------------------------------------------------

static AxEntity *need_entity(AxVM *vm) {
  if (!vm->ctx.entity) ax_throw(vm, "AX-RUNTIME-000", "Cannot read properties of null (reading 'locals')");
  return vm->ctx.entity;
}

// "a b c" from the evaluated arguments, strings verbatim and everything else as an f-string
// would render it.
static AxStr *join_args(AxVM *vm, AxNode *n, AxScope *scope) {
  char *buf = NULL; size_t len = 0, cap = 0;
  app(&buf, &len, &cap, "");
  AxValue *vals = calloc(n->nlist + 1, sizeof(AxValue));
  for (int i = 0; i < n->nlist; i++) vals[i] = ax_eval(vm, n->list[i]->b, scope);
  for (int i = 0; i < n->nlist; i++) {
    if (i) app(&buf, &len, &cap, " ");
    AxStr *s = ax_to_str(vals[i]);
    ax_str_append(&buf, &len, &cap, s->data, s->len);
    ax_release(ax_strv(s));
    ax_release(vals[i]);
  }
  free(vals);
  AxStr *out = ax_str_new(buf, len);
  free(buf);
  return out;
}

static AxEntity *spawn(AxVM *vm, AxNode *n, AxScope *scope) {
  AxWorld *w = W(vm);
  AxStr *decl_name = n->list[0]->b->names[0];
  int di = find_decl(w, decl_name);
  if (di < 0) ax_throw(vm, "AX-RUNTIME-000", "!spawn(#%s): no entity declaration '%s' found", decl_name->data, decl_name->data);
  w->spawn_counter++;
  char nm[256];
  snprintf(nm, sizeof nm, "%s_%d", decl_name->data, w->spawn_counter);
  AxStr *name = ax_internz(nm);
  AxEntity *inst = entity_new(vm, name, decl_name, w->decls[di].base, w->decls[di].members, w->decls[di].nmembers);
  ax_release(ax_strv(name));
  for (int i = 1; i < n->nlist; i++) {
    AxNode *a = n->list[i];
    if (!a->str) continue;
    AxValue v = ax_eval(vm, a->b, scope);
    if (a->str == K_at) {
      if (v.t == AX_VEC3) {
        AxXform *p = ent_pose(inst);
        if (p) xform_set(p, K_pos, ax_copy(v));
        ax_dict_set(inst->locals, K_position, ax_vec2(ax_vecp(v)->x, ax_vecp(v)->y));
      }
      ax_release(v);
    } else {
      ax_entity_set(inst, a->str, v);
    }
  }
  return inst;
}

extern AxValue ax_pool_spawn(AxVM *vm, AxNode *n, AxScope *scope);
extern void ax_pool_release_action(AxVM *vm, AxNode *n, AxScope *scope);
extern void ax_save_action(AxVM *vm, AxNode *n, AxScope *scope, bool load);

static void action(AxVM *vm, AxNode *n, AxScope *scope) {
  AxWorld *w = W(vm);
  const char *name = n->str->data;
  AxEntity *self = vm->ctx.entity;
  if (!strcmp(name, "print") || !strcmp(name, "log")) {
    AxStr *msg = join_args(vm, n, scope);
    bool quiet = w ? ax_engine_log_msg(vm, msg) : false;
    if (!quiet) ax_write_line(vm, 1, msg->data, msg->len);
    ax_release(ax_strv(msg));
    return;
  }
  if (w) {
    if (!strcmp(name, "move")) {
      AxEntity *e = need_entity(vm);
      AxValue d = arg_eval(vm, n, 0, scope);
      V3 d3 = d.t == AX_VEC3 ? v3of(d) : d.t == AX_VEC2 ? v3(ax_vecp(d)->x, 0, ax_vecp(d)->y) : d.t == AX_NUM ? v3(d.num, 0, 0) : v3(0, 0, 0);
      ax_release(d);
      AxXform *p = ent_pose(e);
      if (p) xform_set(p, K_pos, mk3(v3add(v3of(p->pos), d3)));
      AxValue pos2;
      if (ax_dict_get(e->locals, K_position, &pos2) && pos2.t == AX_VEC2) {
        ax_dict_set(e->locals, K_position, ax_vec2(ax_vecp(pos2)->x + d3.x, ax_vecp(pos2)->y + d3.z));
      } else {
        ax_dict_set(e->locals, K_position, ax_vec2(d3.x, d3.z));
      }
      ax_release(pos2);
      return;
    }
    if (!strcmp(name, "spawn")) {
      if (n->nlist && n->list[0]->b->kind == N_TAGREF) { spawn(vm, n, scope); return; }
      AxValue r = ax_pool_spawn(vm, n, scope);
      ax_release(r);
      return;
    }
    if (!strcmp(name, "release")) { ax_pool_release_action(vm, n, scope); return; }
    if (!strcmp(name, "despawn")) {
      if (!n->nlist) ax_throw(vm, "AX-RUNTIME-000", "'!despawn(...)' requires an argument: self or #Tag");
      AxNode *a = n->list[0]->b;
      AxEntity *target = NULL;
      if (a->kind == N_TAGREF) {
        target = resolve_tag(w, a->names[0]);
        if (!target) ax_throw(vm, "AX-RUNTIME-000", "'!despawn(#%s)': entity not found", a->names[0]->data);
      } else if (a->kind == N_IDENT && a->str == K_self) {
        target = self;
      } else {
        AxValue v = ax_eval(vm, a, scope);
        if (v.t == AX_ENTITY) target = (AxEntity *)v.o;
        ax_release(v);
        if (!target) ax_throw(vm, "AX-RUNTIME-000", "'!despawn(...)': argument must be 'self' or #Tag");
      }
      if (!target) ax_throw(vm, "AX-RUNTIME-000", "Cannot set properties of null (setting '_pendingRemove')");
      target->pending_remove = true;
      return;
    }
    if (!strcmp(name, "tween")) {
      if (n->nlist < 3) ax_throw(vm, "AX-RUNTIME-000", "!tween requires at least 3 args: target, endValue, duration");
      AxNode *te = n->list[0]->b;
      AxStr *path[9];
      int np = 0;
      // flattenPath: a name or a chain of member reads.
      AxNode *cur = te;
      AxStr *rev[9];
      while (cur && cur->kind == N_MEMBER && np < 8) { rev[np++] = cur->str; cur = cur->a; }
      if (!cur || cur->kind != N_IDENT) ax_throw(vm, "AX-RUNTIME-000", "!tween target must be a dotted property path");
      int total = np + 1;
      path[0] = cur->str;
      for (int i = 0; i < np; i++) path[i + 1] = rev[np - 1 - i];
      Tween tw;
      memset(&tw, 0, sizeof tw);
      if (total == 1) { tw.target = self ? ent_val(self) : ax_null(); tw.path[0] = path[0]; tw.npath = 1; }
      else {
        AxValue root = ax_eval(vm, cur, scope);
        tw.target = root;
        for (int i = 1; i < total; i++) tw.path[i - 1] = path[i];
        tw.npath = total - 1;
      }
      tw.end = ax_eval(vm, n->list[1]->b, scope);
      AxNode *de = n->list[2]->b;
      AxValue dv = ax_eval(vm, de, scope);
      tw.duration = ax_to_num(dv);
      ax_release(dv);
      if (de->kind == N_NUM && de->str && !strcmp(de->str->data, "ms")) tw.duration /= 1000;
      snprintf(tw.ease, sizeof tw.ease, "out");
      if (n->nlist > 3) {
        AxValue ev = ax_eval(vm, n->list[3]->b, scope);
        AxStr *es = ax_to_str(ev);
        snprintf(tw.ease, sizeof tw.ease, "%s", es->data);
        ax_release(ax_strv(es));
        ax_release(ev);
      }
      tw.start = ax_null();
      if (w->ntweens == w->captweens) { w->captweens = w->captweens ? w->captweens * 2 : 8; w->tweens = realloc(w->tweens, sizeof(Tween) * w->captweens); }
      w->tweens[w->ntweens++] = tw;
      return;
    }
    if (!strcmp(name, "save") || !strcmp(name, "load")) { need_entity(vm); ax_save_action(vm, n, scope, name[0] == 'l'); return; }
    if (!strcmp(name, "play") || !strcmp(name, "music") || !strcmp(name, "play_anim") || !strcmp(name, "dbg_line")) {
      // Recorded in the log without a message, as in the reference; the native build does
      // not open an audio device.
      need_entity(vm);
      for (int i = 0; i < n->nlist; i++) { AxValue v = ax_eval(vm, n->list[i]->b, scope); ax_release(v); }
      return;
    }
    if (!strcmp(name, "stop_anim")) { need_entity(vm); return; }
    if (!strcmp(name, "mesh")) {
      AxEntity *e = need_entity(vm);
      AxDict *cmd = ax_dict_new();
      AxStr *k = ax_internz("entity"); ax_dict_set(cmd, k, str_val(e->name)); ax_release(ax_strv(k));
      k = ax_internz("mesh"); ax_dict_set(cmd, k, arg_eval(vm, n, 0, scope)); ax_release(ax_strv(k));
      for (int i = 1; i < n->nlist; i++) {
        if (!n->list[i]->str) continue;
        ax_dict_set(cmd, n->list[i]->str, ax_eval(vm, n->list[i]->b, scope));
      }
      ax_arr_push(w->draw_list, ax_dictv(cmd));
      return;
    }
    if (!strcmp(name, "d")) {
      AxEntity *e = need_entity(vm);
      AxArr *args = ax_arr_new(n->nlist);
      for (int i = 0; i < n->nlist; i++) ax_arr_push(args, ax_eval(vm, n->list[i]->b, scope));
      DebugEntry *d = &w->ring[w->ring_head];
      if (d->args) ax_release(ax_arrv(d->args));
      d->args = args;
      d->entity = e->name;
      d->block = vm->ctx.block;
      d->t = w->sim_time;
      w->ring_head = (w->ring_head + 1) % DEBUG_RING;
      if (w->ring_count < DEBUG_RING) w->ring_count++;
      return;
    }
    // Subsystem actions.
    if (self && self->base == K_Body3D && (!strcmp(name, "force") || !strcmp(name, "impulse") || !strcmp(name, "torque"))) {
      AxValue f = arg_eval(vm, n, 0, scope);
      for (int i = 1; i < n->nlist; i++) { AxValue v = ax_eval(vm, n->list[i]->b, scope); ax_release(v); }
      AxXform *p = ent_pose(self);
      if (p && f.t == AX_VEC3 && name[0] != 't') {
        V3 fv = v3of(f);
        if (name[0] == 'f') fv = v3div(fv, field_num_or(self, K_mass, 1));
        xform_set(p, K_vel, mk3(v3add(v3of(p->vel), fv)));
      }
      ax_release(f);
      return;
    }
    if (self && self->base && ((!strcmp(self->base->data, "Net") && !strcmp(name, "rpc")) || (!strcmp(self->base->data, "Fluid3D") && !strcmp(name, "fluid_step")))) {
      for (int i = 0; i < n->nlist; i++) { AxValue v = ax_eval(vm, n->list[i]->b, scope); ax_release(v); }
      return;
    }
  }
  // Otherwise `!name(...)` calls a ^proc/^fn, a callable local, or a library function whose
  // result is not wanted.
  AxValue fn = ax_null();
  bool found = false;
  for (AxScope *p = scope; p && !found; p = p->parent) {
    AxValue v;
    if (!ax_scope_lookup_local(p, n->str, &v)) continue;
    if (v.t == AX_FN) { fn = v; found = true; } else ax_release(v);
  }
  if (!found) ax_throw(vm, "AX-RUNTIME-ACTION", "unknown action '!%s(...)'", name);
  AxValue *args = calloc(n->nlist + 1, sizeof(AxValue));
  for (int i = 0; i < n->nlist; i++) args[i] = ax_eval(vm, n->list[i]->b, scope);
  AxValue r = ax_call(vm, fn, args, n->nlist);
  ax_release(r);
  for (int i = 0; i < n->nlist; i++) ax_release(args[i]);
  free(args);
  ax_release(fn);
}

// ---------------------------------------------------------------------------------------------
// Statements: actions, broadcasts, ^emit, transitions, ~=
// ---------------------------------------------------------------------------------------------

int ax_engine_exec(AxVM *vm, AxNode *n, AxScope *scope, AxValue *out) {
  init_keys();
  AxWorld *w = W(vm);
  switch (n->kind) {
    case N_ACTION: action(vm, n, scope); return AX_FLOW_NORMAL;
    case N_ASSIGN: ax_dist_observe(vm, n, scope); return AX_FLOW_NORMAL;
    case N_EMIT: {
      char *buf = NULL; size_t len = 0, cap = 0;
      app(&buf, &len, &cap, "");
      for (int i = 0; i < n->nnames; i++) { if (i) app(&buf, &len, &cap, "."); app(&buf, &len, &cap, n->names[i]->data); }
      AxArr *vals = ax_arr_new(n->nlist);
      for (int i = 0; i < n->nlist; i++) ax_arr_push(vals, ax_eval(vm, n->list[i]->b, scope));
      AxValue v = vals->len == 1 ? ax_copy(vals->items[0]) : ax_arrv(vals);
      if (vals->len == 1) ax_release(ax_arrv(vals));
      if (w) {
        AxStr *key = ax_intern(buf, len);
        ax_dict_set(w->channels, key, v);
        ax_release(ax_strv(key));
      } else ax_release(v);
      free(buf);
      return AX_FLOW_NORMAL;
    }
    case N_TRANSITION: {
      for (int i = 0; i < n->nlist; i++) {
        AxNode *c = n->list[i];
        bool take = true;
        if (c->a) { AxValue g = ax_eval(vm, c->a, scope); take = ax_truthy(g); ax_release(g); }
        if (!take) continue;
        if (c->str == n->str || c->str == K_self) return AX_FLOW_NORMAL;
        AxEntity *e = need_entity(vm);
        if (c->nlist) {
          AxValue first = ax_null();
          for (int k = 0; k < c->nlist; k++) {
            AxValue v = ax_eval(vm, c->list[k]->b, scope);
            if (k == 0) first = v; else ax_release(v);
          }
          char nm[256];
          snprintf(nm, sizeof nm, "_arg_%s", n->str->data);
          AxStr *key = ax_internz(nm);
          ax_dict_set(e->locals, key, first);
          ax_release(ax_strv(key));
        }
        ax_retain(ax_strv(c->str));
        ax_entity_set(e, n->str, ax_atom(c->str));
        return AX_FLOW_NORMAL;
      }
      return AX_FLOW_NORMAL;
    }
    case N_BROADCAST: {
      AxDict *payload = ax_dict_new();
      for (int i = 0; i < n->nlist; i++) {
        if (!n->list[i]->str) continue;
        ax_dict_set(payload, n->list[i]->str, ax_eval(vm, n->list[i]->b, scope));
      }
      AxNode *schema = w ? find_named(w->events, w->nevents, n->str) : NULL;
      if (schema) {
        for (int i = 0; i < schema->nlist; i++) {
          AxNode *f = schema->list[i];
          if (ax_dict_has(payload, f->str)) continue;
          // An omitted `source` defaults to the sender, unless the schema marks it optional.
          if (!f->flag && f->str == K_source) ax_dict_set(payload, f->str, vm->ctx.entity ? ent_val(vm->ctx.entity) : ax_null());
          else ax_dict_set(payload, f->str, ax_null());
        }
      }
      PendingEvent ev;
      memset(&ev, 0, sizeof ev);
      ev.name = n->str;
      ev.payload = payload;
      ev.mode = n->op;
      ev.target = n->str2;
      if (n->op == 2) {
        AxValue r = ax_eval(vm, n->a, scope);
        ev.radius = ax_to_num(r);
        ax_release(r);
        if (n->b) { AxValue o = ax_eval(vm, n->b, scope); ev.origin = v3of(o); ax_release(o); }
        else ev.origin = entity_pos(need_entity(vm));
      }
      if (!w) { ax_release(ax_dictv(payload)); return AX_FLOW_NORMAL; }
      // Sent from a &tick block, an event waits for the next physics step; from anywhere else
      // it is delivered now.
      if (vm->ctx.block == K_tick) {
        if (w->npending == w->cappending) { w->cappending = w->cappending ? w->cappending * 2 : 8; w->pending = realloc(w->pending, sizeof(PendingEvent) * w->cappending); }
        w->pending[w->npending++] = ev;
      } else {
        deliver(vm, &ev);
        ax_release(ax_dictv(payload));
      }
      return AX_FLOW_NORMAL;
    }
    default: return AX_FLOW_NORMAL;
  }
}

// ---------------------------------------------------------------------------------------------
// --sim --json output (main.js)
// ---------------------------------------------------------------------------------------------

int ax_engine_diag_count(AxVM *vm) { return W(vm) ? W(vm)->ndiags : 0; }

int ax_engine_fatal_count(AxVM *vm) {
  int n = 0;
  if (W(vm)) for (int i = 0; i < W(vm)->ndiags; i++) if (!strcmp(W(vm)->diags[i].severity, "fatal")) n++;
  return n;
}

static const char *classify_human(const char *code, const char *msg) {
  if (!strcmp(code, "AX-RUNTIME-000")) return "An unclassified runtime fault occurred.";
  if (!strcmp(code, "AX-RUNTIME-ACTION")) return "Called an undefined action.";
  if (!strcmp(code, "AX-RUNTIME-QUERY")) return "Called an undeclared query on a subsystem entity.";
  if (!strcmp(code, "AX-RUNTIME-KERNEL")) return "Used a stochastic operator on a non-distribution value.";
  if (!strcmp(code, "AX-DEPTH-001")) return "Recursion went too deep.";
  if (!strcmp(code, "AX-LOOP-002")) return "A loop in a hot block ran past its iteration budget.";
  if (!strcmp(code, "AX-CALL-001")) return "Tried to call something that is not a function.";
  if (!strcmp(code, "AX-SANDBOX-001")) return "The sandbox denied this operation.";
  return msg;
}

// The advice interpreter.js classifyRuntimeError appends to message_for_agent, by code.
static const char *fault_hint(const char *code) {
  static const struct { const char *code, *hint; } H[] = {
    { "AX-DEPTH-001", "Add a base case, or convert the recursion to a loop. Use memo(fn) if the recursion is re-computing the same arguments." },
    { "AX-LOOP-002", "Frame blocks (&physics/&render/&tick/&on) are capped so one frame cannot hang the program. Move the long computation into a ^fn called from ^main, or bound the loop." },
    { "AX-CALL-001", "Check the value: type(v) reports \"fn\" for callables. A ^fn name used without () is a function value; a field holding a number is not." },
    { "AX-SANDBOX-001", "Pass --allow-read/--allow-write for the path, or --allow-exec for commands. Sandbox mode denies all three by default." },
    { "AX-RUNTIME-INDEX", "Guard the value first: `?is_null(v):` or `v ?? []`, and use len(v) to check the range. Reading past the end of an array gives null rather than an error." },
    { "AX-RUNTIME-METHOD", "Check the receiver type with type(v). Array, dict, and string methods are listed in STDLIB.md; a dict field holding a function is callable as a method." },
    { "AX-RUNTIME-FUNC", "Declare it with ^fn/^proc, import it with ^use, or check the spelling against STDLIB.md." },
    { "AX-CHECK", "A check()/check_eq() assertion failed. Catch it with ^try:/^catch e: or fix the condition." },
    { "AX-RUNTIME-ACTION", "Only intrinsic actions and native subsystem declared actions exist." },
    { "AX-RUNTIME-QUERY", "Queries are declared on the subsystem. Check the entity has the right base type." },
    { "AX-RUNTIME-KERNEL", "Check that the '~='/'~>' target is actually a '$'-declared field." },
    { "AX-RUNTIME-TAG", "Check the tag is spelled correctly and that entity exists." },
    { "AX-RUNTIME-MEMBER", "The object does not have the requested property." },
    { "AX-RUNTIME-000", "See the raw message for details." },
  };
  for (size_t i = 0; i < sizeof H / sizeof H[0]; i++) if (!strcmp(code, H[i].code)) return H[i].hint;
  return "Raised by ^throw or by the standard library. Wrap the call in ^try:/^catch e: to handle it.";
}

// interpreter.js attaches a suggested fix to the faults it classifies from a message; these
// codes arise only that way.
static const char *fault_fix(const char *code) {
  if (!strcmp(code, "AX-RUNTIME-ACTION")) return "Available actions: !play(snd,vol), !music(snd), !play_anim(#Mesh,clip,loop?), !mesh(#Ref,mat/color:), !tween(target,dest,dur,ease), !save(slot), !load(slot), !dbg_line(a,b), !log(msg,level), !d(msg,...), !spawn(#Entity,at:), !despawn(self|#Tag), !move(delta). Body3D: !force(v), !impulse(v).";
  if (!strcmp(code, "AX-RUNTIME-KERNEL")) return "Distributions are declared with $field: shape ~infer: strategy. The ~= and ~> operators only work on $-fields.";
  if (!strcmp(code, "AX-RUNTIME-TAG")) return "Tag references like #Player.pos resolve to entity tags. Ensure the entity @Player is declared. For resources, use #Mesh3D Name: \"path\" and reference as #Name.";
  if (!strcmp(code, "AX-RUNTIME-QUERY")) return "NavMesh3D queries: ?path(from,to), ?raycast(origin,dir,maxDist). No other subsystems have queries. Ensure the entity has &NavMesh3D as its base type.";
  if (!strcmp(code, "AX-RUNTIME-MEMBER")) return "Vec3: .x .y .z .mag .norm. Quat: .euler .conj .normalized. Mat4: .inv .T. Transform: .pos .rot .scl .vel. Entity: .hp .speed etc (your ~fields). Use pose.pos not self.pos (pose is implicit).";
  if (!strcmp(code, "AX-RUNTIME-000")) return "Check the line for syntax errors, undefined references, or type mismatches.";
  return NULL;
}

void ax_engine_print_diags(AxVM *vm, FILE *out) {
  AxWorld *w = W(vm);
  if (!w) return;
  AxStr *main_block = ax_internz("main");
  for (int i = 0; i < w->ndiags; i++) {
    if (w->diags[i].block == main_block && !w->diags[i].entity) continue;   // reported with its line
    fprintf(out, "  [%s] %s\n", w->diags[i].code, classify_human(w->diags[i].code, w->diags[i].human));
  }
}

static void json_diag(JB *b, Diag *d, int depth) {
  const int I = 2;
  jb(b, "{");
  jpad(b, I, depth + 1); jb(b, "\"error_code\": "); jstr(b, d->code, strlen(d->code)); jb(b, ",");
  jpad(b, I, depth + 1); jb(b, "\"severity\": "); jstr(b, d->severity, strlen(d->severity)); jb(b, ",");
  jpad(b, I, depth + 1); jb(b, "\"location\": {");
  jpad(b, I, depth + 2); jb(b, "\"entity\": "); if (d->entity) jstr(b, d->entity->data, d->entity->len); else jb(b, "null"); jb(b, ",");
  jpad(b, I, depth + 2); jb(b, "\"block\": "); if (d->block) jstr(b, d->block->data, d->block->len); else jb(b, "null"); jb(b, ",");
  jpad(b, I, depth + 2); jb(b, "\"line\": "); if (d->line) jnum(b, d->line); else jb(b, "null"); jb(b, ",");
  jpad(b, I, depth + 2); jb(b, "\"col\": "); if (d->col) jnum(b, d->col); else jb(b, "null");
  jpad(b, I, depth + 1); jb(b, "},");
  jpad(b, I, depth + 1); jb(b, "\"violated_rule\": {");
  jpad(b, I, depth + 2); jb(b, "\"section\": \"runtime\",");
  jpad(b, I, depth + 2); jb(b, "\"title\": "); jstr(b, d->title, strlen(d->title));
  jpad(b, I, depth + 1); jb(b, "},");
  jpad(b, I, depth + 1); jb(b, "\"context_snippet\": "); if (d->has_snippet) jstr(b, d->snippet, strlen(d->snippet)); else jb(b, "null"); jb(b, ",");
  const char *human = d->title[0] == 'S' ? d->human : classify_human(d->code, d->human);
  jpad(b, I, depth + 1); jb(b, "\"message_for_human\": "); jstr(b, human, strlen(human)); jb(b, ",");
  char agent[1600];
  const char *hint = d->title[0] == 'S' ? NULL : fault_hint(d->code);
  if (hint) snprintf(agent, sizeof agent, "%s Raw error: \"%s\". %s", human, d->human, hint);
  else snprintf(agent, sizeof agent, "%s Raw error: \"%s\".", human, d->human);
  jpad(b, I, depth + 1); jb(b, "\"message_for_agent\": "); jstr(b, agent, strlen(agent)); jb(b, ",");
  const char *fix = d->title[0] == 'S' ? NULL : fault_fix(d->code);
  jpad(b, I, depth + 1); jb(b, "\"suggested_fix\": ");
  if (fix) jstr(b, fix, strlen(fix)); else jb(b, "null");
  jb(b, ",");
  jpad(b, I, depth + 1); jb(b, "\"auto_fixable\": false,");
  jpad(b, I, depth + 1); jb(b, "\"__axiomRuntimeFault\": true");
  jpad(b, I, depth); jb(b, "}");
}

void ax_engine_print_json(AxVM *vm, int frames, FILE *out) {
  init_keys();
  AxWorld *w = W(vm);
  JB b = { 0 };
  const int I = 2;
  jb(&b, "{");
  jpad(&b, I, 1); jb(&b, "\"frames_run\": "); jnum(&b, frames); jb(&b, ",");
  jpad(&b, I, 1); jb(&b, "\"sim_time\": "); jnum(&b, w ? w->sim_time : 0); jb(&b, ",");
  jpad(&b, I, 1); jb(&b, "\"entities\": ");
  if (!w || !w->nents) jb(&b, "[]");
  else {
    jb(&b, "[");
    for (int i = 0; i < w->nents; i++) {
      AxEntity *e = w->ents[i];
      if (i) jb(&b, ",");
      jpad(&b, I, 2); jb(&b, "{");
      jpad(&b, I, 3); jb(&b, "\"tag\": "); jstr(&b, e->name->data, e->name->len); jb(&b, ",");
      jpad(&b, I, 3); jb(&b, "\"fields\": ");
      AxValue fv; fv.t = AX_DICT; fv.o = (AxObj *)e->fields;
      json_value(&b, fv, I, 3, true);
      jb(&b, ",");
      jpad(&b, I, 3); jb(&b, "\"pos\": ");
      AxXform *p = ent_pose(e);
      if (p) json_value(&b, p->pos, I, 3, true); else jb(&b, "null");
      jpad(&b, I, 2); jb(&b, "}");
    }
    jpad(&b, I, 1);
    jb(&b, "]");
  }
  jb(&b, ",");
  jpad(&b, I, 1); jb(&b, "\"log\": ");
  int nlog = 0;
  if (w) for (uint32_t i = 0; i < w->log->len; i++) if (((AxStr *)w->log->items[i].o)->len) nlog++;
  if (!nlog) jb(&b, "[]");
  else {
    jb(&b, "[");
    int k = 0;
    for (uint32_t i = 0; i < w->log->len; i++) {
      AxStr *s = (AxStr *)w->log->items[i].o;
      if (!s->len) continue;   // the reference keeps only truthy messages
      if (k++) jb(&b, ",");
      jpad(&b, I, 2);
      jstr(&b, s->data, s->len);
    }
    jpad(&b, I, 1);
    jb(&b, "]");
  }
  jb(&b, ",");
  jpad(&b, I, 1); jb(&b, "\"diagnostics\": ");
  if (!w || !w->ndiags) jb(&b, "[]");
  else {
    jb(&b, "[");
    for (int i = 0; i < w->ndiags; i++) {
      if (i) jb(&b, ",");
      jpad(&b, I, 2);
      json_diag(&b, &w->diags[i], 2);
    }
    jpad(&b, I, 1);
    jb(&b, "]");
  }
  jpad(&b, I, 0);
  jb(&b, "}");
  fprintf(out, "%s\n", b.buf);
  free(b.buf);
}

// Script mode's --json (main.js): {main_result, log, diagnostics, exit_code}.
void ax_engine_print_script_json(AxVM *vm, AxValue result, int code, FILE *out) {
  init_keys();
  AxWorld *w = W(vm);
  JB b = { 0 };
  const int I = 2;
  jb(&b, "{");
  jpad(&b, I, 1); jb(&b, "\"main_result\": "); json_value(&b, result, I, 1, true); jb(&b, ",");
  jpad(&b, I, 1); jb(&b, "\"log\": ");
  int nlog = 0;
  if (w) for (uint32_t i = 0; i < w->log->len; i++) if (((AxStr *)w->log->items[i].o)->len) nlog++;
  if (!nlog) jb(&b, "[]");
  else {
    jb(&b, "[");
    int k = 0;
    for (uint32_t i = 0; i < w->log->len; i++) {
      AxStr *s = (AxStr *)w->log->items[i].o;
      if (!s->len) continue;
      if (k++) jb(&b, ",");
      jpad(&b, I, 2);
      jstr(&b, s->data, s->len);
    }
    jpad(&b, I, 1);
    jb(&b, "]");
  }
  jb(&b, ",");
  jpad(&b, I, 1); jb(&b, "\"diagnostics\": ");
  if (!w || !w->ndiags) jb(&b, "[]");
  else {
    jb(&b, "[");
    for (int i = 0; i < w->ndiags; i++) {
      if (i) jb(&b, ",");
      jpad(&b, I, 2);
      json_diag(&b, &w->diags[i], 2);
    }
    jpad(&b, I, 1);
    jb(&b, "]");
  }
  jb(&b, ",");
  jpad(&b, I, 1); jb(&b, "\"exit_code\": "); jnum(&b, code);
  jpad(&b, I, 0);
  jb(&b, "}");
  fprintf(out, "%s\n", b.buf);
  free(b.buf);
}

void ax_json_value_into(void *jbp, AxValue v, int indent, int depth, bool sim) { json_value((JB *)jbp, v, indent, depth, sim); }

void ax_engine_print_log_json(AxVM *vm, FILE *out, int depth) {
  AxWorld *w = W(vm);
  JB b = { 0 };
  jb(&b, "");
  int n = 0;
  if (w) for (uint32_t i = 0; i < w->log->len; i++) if (((AxStr *)w->log->items[i].o)->len) n++;
  if (!n) jb(&b, "[]");
  else {
    jb(&b, "[");
    int k = 0;
    for (uint32_t i = 0; i < w->log->len; i++) {
      AxStr *s = (AxStr *)w->log->items[i].o;
      if (!s->len) continue;
      if (k++) jb(&b, ",");
      jpad(&b, 2, depth + 1);
      jstr(&b, s->data, s->len);
    }
    jpad(&b, 2, depth);
    jb(&b, "]");
  }
  fputs(b.buf, out);
  free(b.buf);
}


// ---------------------------------------------------------------------------------------------
// The small surface infer.c (distributions, navmesh, save/load) needs from the world
// ---------------------------------------------------------------------------------------------

AxValue ax_entity_pos(AxEntity *e) {
  init_keys();
  AxXform *p = e ? ent_pose(e) : NULL;
  return p ? ax_copy(p->pos) : ax_null();
}
int ax_world_entities(AxVM *vm, AxEntity ***out) { *out = W(vm) ? W(vm)->ents : NULL; return W(vm) ? W(vm)->nents : 0; }
AxDict *ax_world_channels(AxVM *vm) { return W(vm) ? W(vm)->channels : NULL; }
AxDict *ax_world_saves(AxVM *vm) { return W(vm) ? W(vm)->save_slots : NULL; }
const char *ax_world_version(AxVM *vm) { return W(vm) && W(vm)->version ? W(vm)->version : NULL; }
void ax_engine_set_version(AxVM *vm, const char *v) { if (W(vm)) W(vm)->version = v; }
AxXform *ax_entity_pose(AxEntity *e) { init_keys(); return ent_pose(e); }

void ax_world_diag(AxVM *vm, AxEntity *e, AxStr *block, const char *code, const char *severity, const char *title, const char *human) {
  AxWorld *w = W(vm);
  if (!w) return;
  if (w->ndiags == w->capdiags) {
    w->capdiags = w->capdiags ? w->capdiags * 2 : 8;
    w->diags = realloc(w->diags, sizeof(Diag) * w->capdiags);
  }
  Diag *d = &w->diags[w->ndiags++];
  memset(d, 0, sizeof *d);
  snprintf(d->code, sizeof d->code, "%s", code);
  snprintf(d->severity, sizeof d->severity, "%s", severity);
  snprintf(d->title, sizeof d->title, "%s", title);
  snprintf(d->human, sizeof d->human, "%s", human);
  d->entity = e ? e->name : NULL;
  d->block = block;
}

// `vision_cells(origin, facing, player_pos, range, half_angle_deg)` — which cells of a 10×10
// grid are in view, and whether the player's cell is one of them (the observation a `$belief`
// grid is updated with).
static AxValue cell_dict(int x, int y) {
  AxDict *c = ax_dict_new();
  ax_dict_set(c, K_x, ax_num(x));
  ax_dict_set(c, K_y, ax_num(y));
  return ax_dictv(c);
}

NATIVE(e_vision_cells) {
  const int W_ = 10, H_ = 10;
  V3 origin = v3of(A(0));
  AxValue facing = A(1), player = A(2);
  double range = N(3), half = N(4);
  AxArr *visible = ax_arr_new(16);
  for (int y = 0; y < H_; y++) for (int x = 0; x < W_; x++) {
    V3 c = v3(x + 0.5, 0, y + 0.5);
    V3 to = v3sub(c, origin);
    double d = v3mag(to);
    if (d > range) continue;
    if (d > 1e-6) {
      V3 f = A(0).t == AX_VEC3 ? v3of(facing) : v3norm(v3(ax_vecp(facing)->x, 0, ax_vecp(facing)->y));
      double cosA = js_clamp(v3dot(f, v3norm(to)), -1, 1);
      double ang = acos(cosA) * 180 / M_PI;
      if (ang > half) continue;
    }
    ax_arr_push(visible, cell_dict(x, y));
  }
  V3 pp = v3of(player);
  double pz = player.t == AX_VEC3 ? pp.z : pp.y;
  if (isnan(pz)) pz = 0;
  int px = (int)js_clamp(floor(pp.x), 0, W_ - 1), py = (int)js_clamp(floor(pz), 0, H_ - 1);
  AxValue seen = ax_null();
  for (uint32_t i = 0; i < visible->len; i++) {
    AxValue cx, cy;
    ax_dict_get((AxDict *)visible->items[i].o, K_x, &cx);
    ax_dict_get((AxDict *)visible->items[i].o, K_y, &cy);
    if (cx.num == px && cy.num == py) { seen = cell_dict(px, py); break; }
  }
  AxDict *out = ax_dict_new();
  AxStr *k = ax_internz("visibleCells"); ax_dict_set(out, k, ax_arrv(visible)); ax_release(ax_strv(k));
  k = ax_internz("seenCell"); ax_dict_set(out, k, seen); ax_release(ax_strv(k));
  return ax_dictv(out);
}

// patrol_point() — a random point in the 1..9 square, held for 3 seconds of simulation time,
// drawn from the seeded generator.
NATIVE(e_patrol_point) {
  AxEntity *e = vm->ctx.entity;
  if (!e) ax_throw(vm, "AX-RUNTIME-000", "Cannot read properties of null (reading 'world')");
  double now = W(vm) ? W(vm)->sim_time : 0;
  if (e->patrol.t == AX_NULL || now - e->patrol_t0 > 3.0) {
    double x = 1 + ax_rng_next(vm) * 8;
    double z = 1 + ax_rng_next(vm) * 8;
    ax_release(e->patrol);
    e->patrol = ax_vec3(x, 0, z);
    e->patrol_t0 = now;
  }
  return ax_copy(e->patrol);
}

void ax_engine_install_more(AxVM *vm) {
  def(vm, "vision_cells", e_vision_cells, 5, 5);
  def(vm, "patrol_point", e_patrol_point, 0, 0);
}

AxArr *ax_world_draw_list(AxVM *vm) { return W(vm) ? W(vm)->draw_list : NULL; }
