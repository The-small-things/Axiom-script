// AxiomScript reference interpreter (runtime) — v0.4
//
// SCOPE NOTE (read this before assuming parity with §2 of the spec):
// The spec describes an AOT compiler with two backend kernels: a D-Kernel that transpiles
// "&"-marked code to a flat SoA/ECS representation in a systems language (Rust/C++ -> native
// or WASM/SIMD), and an S-Kernel that lowers "$"-typed fields to a chosen inference backend.
// This file is a tree-walking INTERPRETER, not that compiler. It exists to give the spec a
// runnable semantics: every sigil, operator, and construct below behaves the way §1-§4 describe,
// but "&physics" blocks run as ordinary JS function calls on a fixed-timestep loop rather than
// being statically compiled to allocation-free native code, and symbol references are resolved
// dynamically against each entity's own field/local maps rather than compiled ahead of time into
// fixed memory offsets (§1.3's "zero runtime lookup cost" claim is therefore a design target of
// the spec, not a property this interpreter itself provides).
//
// v0.4 additions: Vec3/Quat/Mat4/Transform, user functions/procs, control flow (if/while/for),
// timers, tweens, BVec/BMap collections, collision stubs, draw list, HUD, save/load,
// mixins, audio/network stubs, camera, entity spawning, assert, debug draw/log.

// ==========================================================================================
// SECTION 1: Math Types — Vec2, Vec3, Quat, Mat4, Transform
// ==========================================================================================

// v0.9.0: the general-purpose standard library (see stdlib.js). Kept in its own module so
// the language core stays readable and so the library can be audited/extended on its own.
const { stdlibIntrinsics } = require('./stdlib');

class Vec2 {
  constructor(x, y) { this.x = x; this.y = y; }
  add(o) { return new Vec2(this.x + o.x, this.y + o.y); }
  sub(o) { return new Vec2(this.x - o.x, this.y - o.y); }
  mulScalar(s) { return new Vec2(this.x * s, this.y * s); }
  divScalar(s) { return new Vec2(this.x / s, this.y / s); }
  dot(o) { return this.x * o.x + this.y * o.y; }
  cross(o) { return this.x * o.y - this.y * o.x; }
  get mag() { return Math.hypot(this.x, this.y); }
  get norm() { const m = this.mag; return m < 1e-9 ? new Vec2(0, 0) : new Vec2(this.x / m, this.y / m); }
  to(target, maxDelta) {
    const diff = target.sub(this);
    const d = diff.mag;
    if (d <= maxDelta || d < 1e-9) return new Vec2(target.x, target.y);
    const step = diff.divScalar(d).mulScalar(maxDelta);
    return this.add(step);
  }
  lerp(v, t) { return new Vec2(this.x + (v.x - this.x) * t, this.y + (v.y - this.y) * t); }
  // v0.8.8: reflect — mirror this vector across a surface normal. Formula: v - 2*(v·n)*n.
  // The normal MUST be normalized (unit length) for correct results. Used in 2D physics for
  // bouncing (e.g. a 2D ball reflecting off a wall). callMethod already dispatches .reflect(n)
  // on Vec2 instances; this method just wasn't implemented on the class.
  reflect(n) { return this.sub(n.mulScalar(2 * this.dot(n))); }
  // v0.8.10: angle and rotate for 2D top-down games.
  get angle() { return Math.atan2(this.y, this.x); }
  rotate(a) { const c = Math.cos(a), s = Math.sin(a); return new Vec2(this.x * c - this.y * s, this.x * s + this.y * c); }
  clone() { return new Vec2(this.x, this.y); }
  toString() { return `v2(${this.x}, ${this.y})`; }
}

class Vec3 {
  constructor(x = 0, y = 0, z = 0) { this.x = x; this.y = y; this.z = z; }
  add(o) { return new Vec3(this.x + o.x, this.y + o.y, this.z + o.z); }
  sub(o) { return new Vec3(this.x - o.x, this.y - o.y, this.z - o.z); }
  mulScalar(s) { return new Vec3(this.x * s, this.y * s, this.z * s); }
  divScalar(s) { return new Vec3(this.x / s, this.y / s, this.z / s); }
  dot(o) { return this.x * o.x + this.y * o.y + this.z * o.z; }
  cross(o) {
    return new Vec3(this.y * o.z - this.z * o.y, this.z * o.x - this.x * o.z, this.x * o.y - this.y * o.x);
  }
  get mag() { return Math.hypot(this.x, this.y, this.z); }
  get norm() {
    const m = this.mag;
    return m < 1e-9 ? new Vec3(0, 0, 0) : new Vec3(this.x / m, this.y / m, this.z / m);
  }
  to(target, maxDelta) {
    const diff = target.sub(this);
    const d = diff.mag;
    if (d <= maxDelta || d < 1e-9) return new Vec3(target.x, target.y, target.z);
    const step = diff.divScalar(d).mulScalar(maxDelta);
    return this.add(step);
  }
  lerp(v, t) { return new Vec3(this.x + (v.x - this.x) * t, this.y + (v.y - this.y) * t, this.z + (v.z - this.z) * t); }
  reflect(n) {
    const d = 2 * this.dot(n);
    return new Vec3(this.x - d * n.x, this.y - d * n.y, this.z - d * n.z);
  }
  rotate(q) { return q.rotateVec(this); }
  clone() { return new Vec3(this.x, this.y, this.z); }
  toString() { return `v3(${this.x}, ${this.y}, ${this.z})`; }
  get xy() { return new Vec2(this.x, this.y); }
  get xz() { return new Vec2(this.x, this.z); } // v0.8.10: XZ plane projection
  get yz() { return new Vec2(this.y, this.z); } // v0.8.10: YZ plane projection
  static fromVec2(v, z = 0) { return new Vec3(v.x, v.y, z); }
}

class Quat {
  constructor(x, y, z, w) { this.x = x; this.y = y; this.z = z; this.w = w; }
  static identity() { return new Quat(0, 0, 0, 1); }
  static fromAxisAngle(axis, angle) {
    const ha = angle / 2;
    const s = Math.sin(ha);
    const a = axis.norm;
    return new Quat(a.x * s, a.y * s, a.z * s, Math.cos(ha));
  }
  static fromEuler(pitch, yaw, roll) {
    // v0.5 fix: correct Tait-Bryan ZYX (intrinsic) Euler-to-Quaternion
    const cp = Math.cos(pitch / 2), sp = Math.sin(pitch / 2);
    const cy = Math.cos(yaw / 2), sy = Math.sin(yaw / 2);
    const cr = Math.cos(roll / 2), sr = Math.sin(roll / 2);
    return new Quat(
      sp * cy * cr - cp * sy * sr,
      cp * sy * cr + sp * cy * sr,
      cp * cy * sr - sp * sy * cr,
      cp * cy * cr + sp * sy * sr
    );
  }
  get euler() {
    const { x, y, z, w } = this;
    const sinp = 2 * (w * x - y * z);
    let pitch = Math.abs(sinp) >= 1 ? Math.sign(sinp) * Math.PI / 2 : Math.asin(sinp);
    let yaw = Math.atan2(2 * (w * y + z * x), 1 - 2 * (y * y + x * x));
    let roll = Math.atan2(2 * (w * z + x * y), 1 - 2 * (z * z + x * x));
    return new Vec3(pitch, yaw, roll);
  }
  mulQuat(q) {
    return new Quat(
      this.w * q.x + this.x * q.w + this.y * q.z - this.z * q.y,
      this.w * q.y - this.x * q.z + this.y * q.w + this.z * q.x,
      this.w * q.z + this.x * q.y - this.y * q.x + this.z * q.w,
      this.w * q.w - this.x * q.x - this.y * q.y - this.z * q.z
    );
  }
  rotateVec(v) {
    const qv = new Quat(v.x, v.y, v.z, 0);
    const r = this.mulQuat(qv).mulQuat(this.conj);
    return new Vec3(r.x, r.y, r.z);
  }
  get conj() { return new Quat(-this.x, -this.y, -this.z, this.w); }
  get normalized() {
    const m = Math.hypot(this.x, this.y, this.z, this.w);
    return m < 1e-9 ? Quat.identity() : new Quat(this.x / m, this.y / m, this.z / m, this.w / m);
  }
  slerp(q, t) {
    let dot = this.x * q.x + this.y * q.y + this.z * q.z + this.w * q.w;
    let q2 = q;
    if (dot < 0) { q2 = new Quat(-q.x, -q.y, -q.z, -q.w); dot = -dot; }
    if (dot > 0.9995) {
      return new Quat(this.x + t * (q2.x - this.x), this.y + t * (q2.y - this.y), this.z + t * (q2.z - this.z), this.w + t * (q2.w - this.w)).normalized;
    }
    const theta = Math.acos(clampNum(dot, -1, 1));
    const sinT = Math.sin(theta);
    const a = Math.sin((1 - t) * theta) / sinT;
    const b = Math.sin(t * theta) / sinT;
    return new Quat(a * this.x + b * q2.x, a * this.y + b * q2.y, a * this.z + b * q2.z, a * this.w + b * q2.w);
  }
  clone() { return new Quat(this.x, this.y, this.z, this.w); }
  toString() { return `q(${this.x}, ${this.y}, ${this.z}, ${this.w})`; }
}

class Mat4 {
  constructor(d) { this.d = d || new Float64Array(16); }
  static identity() {
    const m = new Mat4(); m.d[0] = 1; m.d[5] = 1; m.d[10] = 1; m.d[15] = 1; return m;
  }
  static perspective(fovDeg, aspect, near, far) {
    const f = 1 / Math.tan(fovDeg * Math.PI / 360);
    const nf = 1 / (near - far);
    const m = new Mat4();
    m.d[0] = f / aspect; m.d[5] = f; m.d[10] = (far + near) * nf; m.d[11] = -1; m.d[14] = 2 * far * near * nf;
    return m;
  }
  static ortho(l, r, b, t, n, f) {
    const m = new Mat4();
    m.d[0] = 2 / (r - l); m.d[5] = 2 / (t - b); m.d[10] = -2 / (f - n);
    m.d[12] = -(r + l) / (r - l); m.d[13] = -(t + b) / (t - b); m.d[14] = -(f + n) / (f - n); m.d[15] = 1;
    return m;
  }
  static lookAt(eye, target, up) {
    // v0.8.3: handle degenerate case where view direction is parallel to `up` (e.g. top-down
    // camera with eye=(0,N,0), target=(0,0,0), up=(0,1,0)). The cross product `up × z`
    // collapses to (0,0,0), producing a degenerate view matrix with zero right/up columns —
    // nothing rasterizes. Fix: when |up · z| ≈ 1, fall back to world-forward (0,0,1) as the
    // up vector. This makes the canonical minimap/overhead camera work without requiring the
    // user to set a custom ~up: field.
    const z = eye.sub(target).norm;
    let upVec = up || new Vec3(0, 1, 0);
    // Check for near-parallel: |dot(up, z)| close to 1 means the cross product will be ~0.
    const dot = Math.abs(upVec.x * z.x + upVec.y * z.y + upVec.z * z.z);
    if (dot > 0.9999) {
      // Fall back to world-forward (0,0,1). If that's also parallel (camera looking along Z),
      // fall back to world-right (1,0,0) — one of the three cardinal axes is always non-parallel.
      upVec = new Vec3(0, 0, 1);
      const dot2 = Math.abs(upVec.x * z.x + upVec.y * z.y + upVec.z * z.z);
      if (dot2 > 0.9999) upVec = new Vec3(1, 0, 0);
    }
    const x = upVec.cross(z).norm;
    const y = z.cross(x);
    const m = new Mat4();
    m.d[0] = x.x; m.d[4] = x.y; m.d[8] = x.z;  m.d[12] = -x.dot(eye);
    m.d[1] = y.x; m.d[5] = y.y; m.d[9] = y.z;  m.d[13] = -y.dot(eye);
    m.d[2] = z.x; m.d[6] = z.y; m.d[10] = z.z; m.d[14] = -z.dot(eye);
    m.d[15] = 1;
    return m;
  }
  mulVec3(v) {
    const { d } = this;
    const w = d[3] * v.x + d[7] * v.y + d[11] * v.z + d[15];
    if (Math.abs(w) < 1e-9) return new Vec3(0, 0, 0);
    return new Vec3(
      (d[0] * v.x + d[4] * v.y + d[8] * v.z + d[12]) / w,
      (d[1] * v.x + d[5] * v.y + d[9] * v.z + d[13]) / w,
      (d[2] * v.x + d[6] * v.y + d[10] * v.z + d[14]) / w
    );
  }
  mulMat4(o) {
    const a = this.d, b = o.d, r = new Float64Array(16);
    for (let c = 0; c < 4; c++) for (let row = 0; row < 4; row++) {
      let s = 0; for (let k = 0; k < 4; k++) s += a[k * 4 + row] * b[c * 4 + k];
      r[c * 4 + row] = s;
    }
    return new Mat4(r);
  }
  get inv() {
    const m = this.d, inv = new Float64Array(16);
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
    let det = m[0]*inv[0]+m[1]*inv[4]+m[2]*inv[8]+m[3]*inv[12];
    if (Math.abs(det) < 1e-9) return Mat4.identity();
    det = 1 / det;
    for (let i = 0; i < 16; i++) inv[i] *= det;
    return new Mat4(inv);
  }
  get T() {
    const { d } = this, r = new Float64Array(16);
    for (let c = 0; c < 4; c++) for (let row = 0; row < 4; row++) r[row * 4 + c] = d[c * 4 + row];
    return new Mat4(r);
  }
  clone() { return new Mat4(new Float64Array(this.d)); }
}

class Transform {
  constructor() {
    this.pos = new Vec3(0, 0, 0);
    this.rot = Quat.identity();
    this.scl = new Vec3(1, 1, 1);
    this.vel = new Vec3(0, 0, 0);
  }
  toMat4() {
    let m = Mat4.identity();
    // Scale
    m.d[0] = this.scl.x; m.d[5] = this.scl.y; m.d[10] = this.scl.z;
    // Rotation (from quat)
    const { x, y, z, w } = this.rot;
    const xx = x*x, yy = y*y, zz = z*z, xy = x*y, xz = x*z, yz = y*z, wx = w*x, wy = w*y, wz = w*z;
    // v0.6 fix: the rotation block is a 3x3 embedded in a 4x4 — d[15] must stay 1 (homogeneous w),
    // not 0. The pre-fix code used `new Mat4()` (all zeros) and only set the 3x3, leaving d[15]=0,
    // which caused Transform.toMat4() to produce non-affine matrices (any point's w became 0).
    const rm = Mat4.identity();
    rm.d[0]=1-2*(yy+zz); rm.d[4]=2*(xy-wz);   rm.d[8]=2*(xz+wy);
    rm.d[1]=2*(xy+wz);   rm.d[5]=1-2*(xx+zz); rm.d[9]=2*(yz-wx);
    rm.d[2]=2*(xz-wy);   rm.d[6]=2*(yz+wx);   rm.d[10]=1-2*(xx+yy);
    m = rm.mulMat4(m);
    // Translation
    m.d[12] = this.pos.x; m.d[13] = this.pos.y; m.d[14] = this.pos.z;
    return m;
  }
  worldPose(world) {
    let parentRef = this._entity ? this._entity.fields.get('parent') : null;
    if (!parentRef) return this;
    let parentInst = null;
    if (typeof parentRef === 'string') parentInst = world.resolveTag(parentRef);
    else if (parentRef instanceof EntityInstance) parentInst = parentRef; // v0.8.10: direct EntityInstance parent
    else if (parentRef && parentRef.__cell) { /* parent is a cell ref, skip */ }
    else if (parentRef && parentRef.path) parentInst = world.resolveTag(parentRef.path[0]);
    if (!parentInst) return this;
    const parentPose = parentInst.locals.get('pose') || new Transform();
    parentPose._entity = parentInst;
    const pp = parentPose.worldPose(world);
    // Compose: child = parent * child
    const result = new Transform();
    // v0.8.8 fix: use component-wise scaling (scl.x, scl.y, scl.z) instead of just scl.x.
    // The old code `mulScalar(pp.scl.x)` applied only the X scale to all axes, breaking
    // non-uniform scale inheritance (e.g. parent scl=v3(2,1,1) would scale child Y by 2, not 1).
    const rotated = this.rot.rotateVec(this.pos);
    result.pos = pp.pos.add(new Vec3(
      rotated.x * pp.scl.x,
      rotated.y * pp.scl.y,
      rotated.z * pp.scl.z,
    ));
    result.rot = pp.rot.mulQuat(this.rot).normalized;
    result.scl = new Vec3(pp.scl.x * this.scl.x, pp.scl.y * this.scl.y, pp.scl.z * this.scl.z);
    result.vel = pp.vel.add(this.vel);
    return result;
  }
  clone() {
    const t = new Transform();
    t.pos = this.pos.clone(); t.rot = this.rot.clone(); t.scl = this.scl.clone(); t.vel = this.vel.clone();
    return t;
  }
}

// ==========================================================================================
// SECTION 2: Core Types — Atom, Distribution, Pool, BVec, BMap, Range, Tween
// ==========================================================================================

class Atom {
  constructor(name) { this.name = name; }
  toString() { return this.name; }
  // v0.9.2: an atom serializes as its name (to_json, json_stringify, save files), not {"name": …}.
  toJSON() { return this.name; }
}
function atom(name) { return new Atom(name); }

// ==========================================================================================
// v0.9.0: SCOPES, CLOSURES, AND ERRORS
//
// Before v0.9.0 every assignment in a block, a ^fn, or a loop wrote to the *entity* that was
// executing it. That made three things impossible, all of which a general-purpose language
// needs: recursion (a recursive call clobbered its caller's variables, since both wrote the
// same entity field), functions without an entity (a ^fn called from a script had nowhere to
// put a local), and shadowing (a parameter could be read but never assigned).
//
// The fix is an ordinary lexical scope chain. Each ^fn / ^proc / ^main call and each loop body
// gets a frame; a name resolves to the innermost frame that declares it, then to the entity's
// fields, then to the world. Entity field writes still work exactly as before — a bare `x = 1`
// inside an entity block still lands on the entity when `x` is one of its declared fields — so
// existing game code is unaffected.
// ==========================================================================================

// Returned by Scope.lookup when a name is bound nowhere in the chain. A sentinel (rather than
// undefined) lets a single walk distinguish "absent" from "bound to null", which matters on the
// hot path: identifier resolution runs several times per statement in a 60 Hz block.
const NOT_BOUND = Symbol('not-bound');

class Scope {
  constructor(parent = null, isFnRoot = false) {
    this.vars = new Map();
    this.parent = parent;
    this.isFnRoot = isFnRoot;   // true for a ^fn/^proc/^main frame; false for a loop/block frame
  }
  lookup(name) { for (let s = this; s; s = s.parent) { const v = s.vars.get(name); if (v !== undefined || s.vars.has(name)) return v; } return NOT_BOUND; }
  has(name) { for (let s = this; s; s = s.parent) if (s.vars.has(name)) return true; return false; }
  get(name) { for (let s = this; s; s = s.parent) if (s.vars.has(name)) return s.vars.get(name); return undefined; }
  // Write to the frame that already declares `name`. Returns false if nobody does.
  setExisting(name, value) {
    for (let s = this; s; s = s.parent) if (s.vars.has(name)) { s.vars.set(name, value); return true; }
    return false;
  }
  declare(name, value) { this.vars.set(name, value); return value; }
  // The nearest enclosing function frame — where an undeclared assignment lands, so a name
  // first assigned inside a loop body stays visible after the loop (the shape LLMs expect
  // from Python/JS `var`, and the one that makes accumulator loops work).
  fnFrame() { let s = this; while (s && !s.isFnRoot && s.parent) s = s.parent; return s; }
}

// A callable value: a lambda (`\x: x + 1`), a reference to a declared ^fn/^proc, or a bound
// native intrinsic. Closures capture the scope, entity, and world they were created in, so a
// lambda returned from a function keeps working after that function returns.
class Closure {
  constructor({ params, body, isExpr, scope, entity, name, defaults }) {
    this.params = params || [];
    this.defaults = defaults || null;   // parameter defaults, when wrapping a declared ^fn
    this.body = body;
    this.isExpr = !!isExpr;     // expression lambda vs statement body
    this.scope = scope || null;
    this.entity = entity || null;
    this.name = name || '<lambda>';
    this.__callable = true;
  }
  toString() { return `<fn ${this.name}/${this.params.length}>`; }
}

// The value `^throw` raises and `^catch` binds. Runtime faults (a bad index, an unknown
// method) are wrapped in the same shape, so one handler catches both program-raised and
// engine-raised errors.
class AxiomError extends Error {
  constructor(value, code) {
    const msg = (value && typeof value === 'object' && typeof value.msg === 'string')
      ? value.msg
      : (typeof value === 'string' ? value : stringifyFStringVal(value));
    super(msg);
    this.name = 'AxiomError';
    this.axiomValue = value;
    this.axiomCode = code || 'AX-THROW';
  }
}

// Normalize anything thrown (AxiomError, a JS TypeError from a native intrinsic, ...) into the
// dict a ^catch clause binds: {msg, code, value}. `msg` is always a string so f-strings can
// print it directly; `value` is the original payload for programs that throw structured errors.
function errorToValue(err) {
  if (err instanceof AxiomError) {
    const v = err.axiomValue;
    if (v && typeof v === 'object' && !Array.isArray(v) && !(v instanceof Atom)) {
      return Object.assign({ msg: err.message, code: err.axiomCode }, v);
    }
    return { msg: err.message, code: err.axiomCode, value: v };
  }
  return { msg: err && err.message ? err.message : String(err), code: classifyRuntimeError(err), value: null };
}

// v0.9.0: guards against the two ways a general-purpose program hangs the host instead of
// reporting an error. Both are generous enough that real computation never hits them.
const MAX_CALL_DEPTH = 6000;      // recursion — reported as an error, not a JS stack overflow
const HOT_LOOP_LIMIT = 200000;    // per-loop cap inside &physics/&render/&tick/&on (frame budget)

// Call any callable value with already-evaluated arguments. This is the single entry point used
// by `f(x)` where f is a variable, by `|>`, and by every higher-order library method — which is
// why `.map` now works with a lambda, a ^fn name, an intrinsic, or a closure stored in a dict.
function callValue(fnVal, argVals, ctx, nameHint) {
  if (fnVal == null) throw new AxiomError(`tried to call ${nameHint ? `'${nameHint}'` : 'a value'} that is null`, 'AX-CALL-001');
  // A native intrinsic (plain JS function).
  if (typeof fnVal === 'function') return invokeIntrinsic(fnVal, argVals, ctx);
  // A name: resolve it against the program's functions, then the intrinsics.
  if (typeof fnVal === 'string' || fnVal instanceof Atom) {
    const nm = typeof fnVal === 'string' ? fnVal : fnVal.name;
    const decl = ctx.world.fns.get(nm) || ctx.world.procs.get(nm);
    if (decl) return callUserFn(decl, argVals, ctx);
    const intr = ctx.world.intrinsics[nm];
    if (intr) return invokeIntrinsic(intr, argVals, ctx);
    throw new AxiomError(`'${nm}' is not a function`, 'AX-CALL-001');
  }
  if (fnVal instanceof Closure) {
    const world = ctx.world;
    if ((world._callDepth = (world._callDepth || 0) + 1) > MAX_CALL_DEPTH) {
      world._callDepth = 0;
      throw new AxiomError(`call depth exceeded ${MAX_CALL_DEPTH} — infinite recursion?`, 'AX-DEPTH-001');
    }
    try {
      const scope = new Scope(fnVal.scope, true);
      const cdefaults = fnVal.defaults || {};
      for (let i = 0; i < fnVal.params.length; i++) {
        const pname = fnVal.params[i];
        let v = i < argVals.length ? argVals[i] : undefined;
        if ((v === undefined || v === null) && cdefaults[pname]) {
          v = evalExpr(cdefaults[pname], { ...ctx, scope, fnParams: null, inFn: true, hot: false });
        }
        scope.declare(pname, v === undefined ? null : v);
      }
      // `args` is always bound to the full argument list, so variadic helpers are writable
      // without a parameter-list ceremony.
      scope.declare('args', argVals.slice());
      const callCtx = {
        ...ctx,
        entity: fnVal.entity !== null && fnVal.entity !== undefined ? fnVal.entity : ctx.entity,
        scope,
        fnParams: null,
        inFn: true,
        hot: false,   // a function body is never subject to the hot-block loop cap
      };
      if (fnVal.isExpr) return evalExpr(fnVal.body, callCtx);
      let last = null;
      for (const stmt of fnVal.body) {
        const r = execStmtInner(stmt, callCtx);
        if (r instanceof ReturnSignal) return r.value;
        if (r !== undefined && !(r instanceof BreakSignal) && !(r instanceof ContinueSignal)) last = r;
      }
      return last;
    } catch (err) {
      throw asDepthError(err);
    } finally {
      world._callDepth--;
    }
  }
  // A raw FnDecl/ProcDecl node (e.g. handed over by older code paths).
  if (fnVal && Array.isArray(fnVal.body) && Array.isArray(fnVal.params)) return callUserFn(fnVal, argVals, ctx);
  throw new AxiomError(`value of type ${typeof fnVal} is not callable`, 'AX-CALL-001');
}

// v0.9.0: some stdlib intrinsics need to call back into the language (anything taking a
// lambda). Those are tagged `__ctx` and receive the evaluation context as their final
// argument; missing optional arguments are padded so the context always lands in the right
// slot regardless of how many arguments the call site supplied.
function invokeIntrinsic(fn, args, ctx) {
  if (!fn.__ctx) return fn(...args);
  const want = Math.max(0, fn.length - 1);
  const a = args.slice();
  while (a.length < want) a.push(undefined);
  return fn(...a, ctx);
}

// True for anything callValue accepts — used by the library methods to tell "a function was
// passed" from "a value was passed" (e.g. `.sort()` vs `.sort(\a, b: b - a)`).
function isCallable(v, world) {
  if (typeof v === 'function' || v instanceof Closure) return true;
  if (v && Array.isArray(v.body) && Array.isArray(v.params)) return true;
  if (world && (typeof v === 'string' || v instanceof Atom)) {
    const nm = typeof v === 'string' ? v : v.name;
    return world.fns.has(nm) || world.procs.has(nm) || typeof world.intrinsics[nm] === 'function';
  }
  return false;
}

function isCell(v) { return v && typeof v === 'object' && v.__cell; }

// v0.5.1: timer objects auto-destructure to their `remaining` in numeric comparisons
// (`?cd <= 0`) via valueOf, so the user-facing syntax matches the spec's "cd <= 0" idiom
// without requiring users to write `cd.remaining <= 0`. The factory also unwraps
// timer-to-timer assignments (`cd = shoot_cd`) by reading the source's remaining,
// so the new timer's remaining is a number, not a nested timer object.
function makeTimer(value, unit) {
  let remaining = value;
  if (value && typeof value === 'object' && value.__timer) remaining = value.remaining;
  return {
    __timer: true,
    remaining,
    total: remaining,
    unit,
    valueOf() { return this.remaining; },
  };
}

function truthy(v) {
  if (v == null) return false;
  if (typeof v === 'boolean') return v;
  if (typeof v === 'number') return v !== 0;
  if (v instanceof Vec2) return v.mag > 0.1;
  if (v instanceof Vec3) return v.mag > 0.1;
  if (v instanceof Atom) return true;
  if (isCell(v)) return true;
  if (Array.isArray(v)) return v.length > 0;
  return !!v;
}
// v0.9.0: equality is STRUCTURAL for arrays and plain dicts/records, and identity-based for
// everything else (entities, pools, collider shapes — where identity is the meaning).
//
// Without this, `[1, 2] == [1, 2]` was false, `[0, 0]` could never be a match arm, and
// `uniq`/`in`/`count` silently under-reported on tuples. Comparing two small structures is the
// common case in the programs this language is for; the depth cap keeps a cyclic structure
// from turning a comparison into a hang.
function equalsVal(a, b, depth) {
  if (a === b) return true;
  // Fast path: two different primitives are simply unequal. This is the overwhelmingly common
  // comparison in a frame block, so it must not pay for the structural walk below.
  const ta = typeof a;
  if (ta === 'number' || ta === 'string' || ta === 'boolean' || a === null || a === undefined) {
    const tb = typeof b;
    if (tb === 'number' || tb === 'string' || tb === 'boolean' || b === null || b === undefined) return false;
  }
  if (a instanceof Atom && b instanceof Atom) return a.name === b.name;
  if (isCell(a) && isCell(b)) return a.x === b.x && a.y === b.y;
  if (a instanceof Vec3 && b instanceof Vec3) return a.x === b.x && a.y === b.y && a.z === b.z;
  if (a instanceof Vec2 && b instanceof Vec2) return a.x === b.x && a.y === b.y;
  if (a instanceof Quat && b instanceof Quat) return a.x === b.x && a.y === b.y && a.z === b.z && a.w === b.w;
  const d = depth === undefined ? 0 : depth;
  if (d > 32) return false;
  if (Array.isArray(a) && Array.isArray(b)) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (!equalsVal(a[i], b[i], d + 1)) return false;
    return true;
  }
  if (a && b && typeof a === 'object' && typeof b === 'object'
      && a.constructor === Object && b.constructor === Object) {
    const ka = Object.keys(a), kb = Object.keys(b);
    if (ka.length !== kb.length) return false;
    for (const k of ka) {
      if (!Object.prototype.hasOwnProperty.call(b, k)) return false;
      if (!equalsVal(a[k], b[k], d + 1)) return false;
    }
    return true;
  }
  return false;
}
function compareOp(op, a, b) {
  switch (op) {
    case '>': return a > b; case '<': return a < b;
    case '>=': return a >= b; case '<=': return a <= b;
    case '==': return equalsVal(a, b); case '!=': return !equalsVal(a, b);
    default: throw new Error(`unknown comparison '${op}'`);
  }
}
function clampNum(x, lo, hi) { return Math.max(lo, Math.min(hi, x)); }

const SUPPORTED_INFER_STRATEGIES = new Set(['particle', 'exact']);

class Distribution {
  constructor(shapeCall, priorCall, inferCall, rand) {
    // v0.9.2: draws come from the program's seeded generator (the one random() and seed() use),
    // so a particle filter is reproducible under seed(n). Non-enumerable: not part of the state.
    Object.defineProperty(this, '_rand', { value: rand || Math.random, enumerable: false });
    if (shapeCall.callee !== 'Grid') {
      throw new Error(`this reference interpreter only implements Grid(w,h) distributions; got '${shapeCall.callee}(...)'`);
    }
    this.w = Math.round(shapeCall.args[0]);
    this.h = Math.round(shapeCall.args[1]);
    this.priorName = priorCall ? priorCall.callee : 'Uniform';
    this.inferName = inferCall ? inferCall.callee : 'particle';
    this.mode = this.inferName === 'exact' ? 'exact' : 'particle';
    this.numParticles = (inferCall && inferCall.args && inferCall.args.length) ? Math.round(inferCall.args[0]) : 64;
    this.particles = [];
    this.weights = null;
    this._massCache = null;
    this.resetPrior();
  }
  resetPrior() {
    this._massCache = null;
    if (this.mode === 'exact') {
      const p0 = 1 / (this.w * this.h);
      this.weights = [];
      for (let y = 0; y < this.h; y++) this.weights.push(new Array(this.w).fill(p0));
      return;
    }
    this.particles = [];
    for (let i = 0; i < this.numParticles; i++) {
      this.particles.push({ x: Math.floor(this._rand() * this.w), y: Math.floor(this._rand() * this.h), w: 1 / this.numParticles });
    }
  }
  _cellLikelihood(x, y, obs, selfPos) {
    const HEAR_RANGE = 8;
    let lik = 1;
    if (typeof obs.noise === 'number' && selfPos) {
      const cx = x + 0.5, cy = y + 0.5;
      const dist = Math.hypot(cx - selfPos.x, cy - selfPos.y);
      const predicted = clampNum(1 - dist / HEAR_RANGE, 0, 1);
      const err = obs.noise - predicted;
      lik *= Math.exp(-(err * err) / (2 * 0.18 * 0.18));
    }
    if (obs.vision) {
      const seen = obs.vision.seenCell;
      const isSeenCell = seen && seen.x === x && seen.y === y;
      const inVisible = !isSeenCell && obs.vision.visibleCells.some(c => c.x === x && c.y === y);
      if (isSeenCell) lik *= 40;
      else if (inVisible) lik *= 0.03;
    }
    return lik;
  }
  update(obs, selfPos) {
    this._massCache = null;
    if (this.mode === 'exact') {
      let sum = 0;
      for (let y = 0; y < this.h; y++) {
        for (let x = 0; x < this.w; x++) {
          this.weights[y][x] *= this._cellLikelihood(x, y, obs, selfPos);
          sum += this.weights[y][x];
        }
      }
      const p0 = 1 / (this.w * this.h);
      for (let y = 0; y < this.h; y++) for (let x = 0; x < this.w; x++) {
        this.weights[y][x] = sum < 1e-12 ? p0 : this.weights[y][x] / sum;
      }
      return;
    }
    const DIFFUSE_P = 0.12;
    for (const p of this.particles) {
      if (this._rand() < DIFFUSE_P) {
        const dirs = [[1, 0], [-1, 0], [0, 1], [0, -1], [0, 0]];
        const d = dirs[Math.floor(this._rand() * dirs.length)];
        p.x = clampNum(p.x + d[0], 0, this.w - 1);
        p.y = clampNum(p.y + d[1], 0, this.h - 1);
      }
    }
    for (const p of this.particles) p.w *= this._cellLikelihood(p.x, p.y, obs, selfPos);
    let sum = 0; for (const p of this.particles) sum += p.w;
    if (sum < 1e-12) { for (const p of this.particles) p.w = 1 / this.numParticles; }
    else { for (const p of this.particles) p.w /= sum; }
    this._resample();
  }
  _resample() {
    const N = this.numParticles;
    // v0.8.13 fix: guard against 0 particles — particles[0].w would crash with TypeError
    if (N === 0 || this.particles.length === 0) return;
    const out = [];
    const step = 1 / N;
    const u0 = this._rand() * step;
    let i = 0, c = this.particles[0].w;
    for (let m = 0; m < N; m++) {
      const U = u0 + m * step;
      while (U > c && i < N - 1) { i++; c += this.particles[i].w; }
      out.push({ x: this.particles[i].x, y: this.particles[i].y, w: step });
    }
    this.particles = out;
  }
  massGrid() {
    if (this.mode === 'exact') return this.weights;
    if (this._massCache) return this._massCache;
    const g = [];
    for (let y = 0; y < this.h; y++) g.push(new Array(this.w).fill(0));
    for (const p of this.particles) g[p.y][p.x] += p.w;
    this._massCache = g;
    return g;
  }
  argmax() {
    const g = this.massGrid();
    let best = { x: 0, y: 0 }, bv = -1;
    for (let y = 0; y < this.h; y++) for (let x = 0; x < this.w; x++) if (g[y][x] > bv) { bv = g[y][x]; best = { x, y }; }
    return { x: best.x, y: best.y, __cell: true };
  }
  sample() {
    const r = this._rand();
    let acc = 0;
    if (this.mode === 'exact') {
      for (let y = 0; y < this.h; y++) for (let x = 0; x < this.w; x++) {
        acc += this.weights[y][x];
        if (r <= acc) return { x, y, __cell: true };
      }
      return { x: this.w - 1, y: this.h - 1, __cell: true };
    }
    for (const p of this.particles) { acc += p.w; if (r <= acc) return { x: p.x, y: p.y, __cell: true }; }
    const last = this.particles[this.particles.length - 1];
    return { x: last.x, y: last.y, __cell: true };
  }
  massAt(cell) {
    const g = this.massGrid();
    const x = Math.round(cell.x), y = Math.round(cell.y);
    if (y < 0 || y >= this.h || x < 0 || x >= this.w) return 0;
    return g[y][x];
  }
  any(op, value) {
    const g = this.massGrid();
    for (let y = 0; y < this.h; y++) for (let x = 0; x < this.w; x++) if (compareOp(op, g[y][x], value)) return true;
    return false;
  }
}

class Pool {
  constructor(elementType, capacity) {
    this.elementType = elementType;
    this.capacity = Math.round(capacity);
    this.slots = new Array(this.capacity).fill(null).map(() => ({ used: false, data: null }));
  }
  spawn(data) {
    const idx = this.slots.findIndex(s => !s.used);
    if (idx === -1) return null;
    this.slots[idx] = { used: true, data };
    return { __handle: true, index: idx };
  }
  release(handle) {
    if (!handle || !handle.__handle) return;
    const slot = this.slots[handle.index];
    if (slot) { slot.used = false; slot.data = null; }
  }
  get liveCount() { return this.slots.reduce((n, s) => n + (s.used ? 1 : 0), 0); }
  // v0.4: iteration support for *item in pool:
  eachUsed() {
    const out = [];
    for (let i = 0; i < this.slots.length; i++) {
      if (this.slots[i].used) out.push(this.slots[i].data);
    }
    return out;
  }
}

// v0.4: BVec — bounded growable vector (the only sanctioned growable container inside & blocks
// is Pool; BVec is for S-Kernel / non-realtime contexts like &tick and &on handlers).
class BVec {
  constructor(elementType, capacity) {
    this.elementType = elementType;
    this.capacity = Math.round(capacity);
    this.data = [];
  }
  push(v) { if (this.data.length < this.capacity) { this.data.push(v); return true; } return false; }
  pop() { return this.data.length > 0 ? this.data.pop() : undefined; }
  get len() { return this.data.length; }
  clear() { this.data.length = 0; }
  get(i) { return this.data[i]; }
  set(i, v) { if (i >= 0 && i < this.data.length) this.data[i] = v; }
  map(fn) { return this.data.map(fn); }
  filter(fn) { return this.data.filter(fn); }
  [Symbol.iterator]() { return this.data[Symbol.iterator](); }
}

// v0.4: BMap — bounded string-keyed map.
class BMap {
  constructor(keyType, valType, capacity) {
    this.keyType = keyType;
    this.valType = valType;
    this.capacity = Math.round(capacity);
    this.data = new Map();
  }
  get(k) { return this.data.get(String(k)); }
  set(k, v) {
    const sk = String(k);
    // v0.8.12 fix: allow updating existing keys even at capacity (was silently dropping updates)
    if (this.data.has(sk) || this.data.size < this.capacity) {
      this.data.set(sk, v); return true;
    }
    return false;
  }
  // v0.8.10 fix: bypassing capacity check — inc() called data.set() directly, growing beyond
  // the declared bound when a new key was added at capacity. Now checks capacity first.
  inc(k, n = 1) {
    const sk = String(k);
    // v0.8.13: return undefined (not phantom value) when at capacity and key is new
    if (!this.data.has(sk) && this.data.size >= this.capacity) return undefined;
    const cur = (this.data.get(sk) || 0) + n;
    this.data.set(sk, cur);
    return cur;
  }
  has(k) { return this.data.has(String(k)); }
  get len() { return this.data.size; }
  clear() { this.data.clear(); }
  // v0.8.12: make BMap iterable (yields keys) — enables `*k in myBMap:`
  *[Symbol.iterator]() { for (const [k] of this.data) yield k; }
}

// v0.4: Range — produced by '..' operator, consumed by ForLoop.
class Range {
  // v0.9.0: an optional step (may be negative). `0..n` still builds a step-1 range, so the
  // existing `*i in 0..10:` form is unchanged.
  constructor(lo, hi, step) { this.lo = lo; this.hi = hi; this.step = (step === undefined || step === null || step === 0) ? 1 : step; }
  get length() { const n = Math.ceil((this.hi - this.lo) / this.step); return n > 0 ? n : 0; }
  [Symbol.iterator]() {
    const step = this.step;
    let i = step > 0 ? Math.round(this.lo) : this.lo;
    const end = this.hi;
    if (Number.isInteger(step)) i = Math.round(this.lo);
    return {
      next() {
        if (step > 0 ? i < end : i > end) { const v = i; i += step; return { value: v, done: false }; }
        return { done: true };
      },
    };
  }
}

// v0.4: Tween — a timed interpolation managed by the World.
class Tween {
  constructor(targetObj, propPath, endVal, duration, easeName) {
    this.targetObj = targetObj;
    this.propPath = propPath; // e.g. ['pose', 'pos']
    this.startVal = null; // captured on first tick
    this.endVal = endVal;
    this.duration = duration;
    this.elapsed = 0;
    this.easeName = easeName || 'out';
    this.done = false;
  }
  _ease(t) {
    switch (this.easeName) {
      case 'in': return t * t;
      case 'out': return t * (2 - t);
      case 'inout': return t < 0.5 ? 2 * t * t : -1 + (4 - 2 * t) * t;
      case 'bounce': if (t < 0.5) return 4 * t * t * t; { const f = (2 * t) - 2; return 0.5 * f * f * f + 1; }
      case 'elastic': if (t === 0 || t === 1) return t; return -Math.pow(2, 10 * (t - 1)) * Math.sin((t - 1.1) * 5 * Math.PI);
      default: return t;
    }
  }
  tick(dt) {
    if (this.done) return;
    // v0.8.13: stop ticking if target entity is despawned
    if (this.targetObj && this.targetObj._pendingRemove) { this.done = true; return; }
    // v0.8.13 fix: safe prop path walk — use explicit null check instead of || (which treats 0/false as falsy)
    const walkPath = (obj, path) => {
      for (let i = 0; i < path.length - 1; i++) {
        let v = obj[path[i]];
        if (v == null && typeof obj.get === 'function') v = obj.get(path[i]);
        if (v == null) { this.done = true; return null; }
        obj = v;
      }
      return obj;
    };
    if (this.startVal === null) {
      // Capture start value by walking the prop path
      let obj = walkPath(this.targetObj, this.propPath);
      if (!obj) return;
      const last = this.propPath[this.propPath.length - 1];
      this.startVal = typeof obj.get === 'function' ? obj.get(last) : obj[last];
      if (this.startVal && typeof this.startVal.clone === 'function') this.startVal = this.startVal.clone();
    }
    this.elapsed += dt;
    const t = clampNum(this.elapsed / this.duration, 0, 1);
    const et = this._ease(t);
    // Interpolate
    let obj = walkPath(this.targetObj, this.propPath);
    if (!obj) return;
    const last = this.propPath[this.propPath.length - 1];
    let val;
    if (this.startVal && typeof this.startVal.lerp === 'function') {
      val = this.startVal.lerp(this.endVal, et);
    } else if (typeof this.startVal === 'number') {
      val = this.startVal + (this.endVal - this.startVal) * et;
    } else {
      val = this.endVal;
    }
    if (typeof obj.set === 'function') obj.set(last, val);
    else obj[last] = val;
    if (t >= 1) this.done = true;
  }
}

// v0.4: Collider shapes (stub — no real geometry, but the surface is real for the checker).
class ColliderShape {
  constructor(kind, params) { this.kind = kind; this.params = params; }
  toString() { return `${this.kind}(${this.params.join(', ')})`; }
}

// ==========================================================================================
// SECTION 2.5 — v0.6 Collision Detection
// ==========================================================================================
//
// Three collision primitives:
//   1. stepCollisions(world) — pairwise overlap test on all entities with `~hit` fields,
//      mutual push-apart (minimum translation vector) + simple impulse along collision normal.
//      Called from stepPhysics AFTER gravity integration but BEFORE &physics blocks (so user
//      code can react to post-collision state).
//   2. raycast(world, origin, dir, maxDist) — sphere/AABB intersection, returns Vec3 hit point
//      or null. Plugs into the existing `?>` operator (was a stub in v0.5).
//   3. Ground plane clamp — any Body3D with `pose.pos.y < 0` snaps to y=0 with `pose.vel.y = 0`.
//
// All three are pure JS, no native code. The collision normal + impulse formulas are the standard
// "minimum translation vector" + "1-DOF momentum exchange" — good enough for the reference; the
// production path is a native solver via the &Body3D subsystem boundary (§1.6).

function colliderWorld(entity) {
  // Resolve the entity's `~hit` field to a world-space collider: {kind, center: Vec3, half: Vec3, r: num}.
  const hit = entity.fields.get('hit');
  if (!hit || !(hit instanceof ColliderShape)) return null;
  const pose = entity.locals.get('pose');
  if (!pose) return null;
  const pos = pose.pos;
  if (hit.kind === 'sphere') {
    // Sphere: stored params are [r] (a number). For mesh kind, params[0] is a TagRef (no real geometry).
    const r = typeof hit.params[0] === 'number' ? hit.params[0] : 0.5;
    return { kind: 'sphere', center: new Vec3(pos.x, pos.y, pos.z), r };
  }
  if (hit.kind === 'box') {
    // Box: stored params are [v3] (a Vec3 = full dimensions). Half-extents = v / 2.
    const v = hit.params[0];
    if (v && typeof v.x === 'number') return { kind: 'box', center: new Vec3(pos.x, pos.y, pos.z), half: new Vec3(v.x / 2, v.y / 2, v.z / 2) };
    return null;
  }
  if (hit.kind === 'capsule') {
    // Capsule: stored params are [r, h]. The capsule is a cylinder of half-height h/2 along the
    // entity's local Y axis, capped with hemispheres of radius r at each end. We model it as a
    // line segment from (center - Y*h/2) to (center + Y*h/2) plus a radius r — the standard
    // "swept sphere" / "stadium" formulation. The entity's pose.rot rotates the segment axis.
    // (v0.8.1: was previously approximated as a sphere of radius r+h/2 — silently wrong for any
    //  pair where one body is taller than wide, e.g. character controllers. Now treated as a real
    //  capsule by testColliders via capsuleSphereMTV / capsuleBoxMTV / capsuleCapsuleMTV.)
    const r = typeof hit.params[0] === 'number' ? hit.params[0] : 0.5;
    const h = typeof hit.params[1] === 'number' ? hit.params[1] : 1;
    // Capsule axis = local +Y transformed by pose.rot. Default rot is identity, so axis = (0,1,0).
    const pose = entity.locals.get('pose');
    const axisY = pose && pose.rot ? pose.rot.rotateVec(new Vec3(0, 1, 0)) : new Vec3(0, 1, 0);
    const half = h / 2;
    const a = new Vec3(pos.x + axisY.x * half, pos.y + axisY.y * half, pos.z + axisY.z * half);
    const b = new Vec3(pos.x - axisY.x * half, pos.y - axisY.y * half, pos.z - axisY.z * half);
    return { kind: 'capsule', center: new Vec3(pos.x, pos.y, pos.z), axisA: a, axisB: b, r, h };
  }
  return null;
}

// v0.8.1: closest point on a line segment AB to a point P. Used by all three capsule MTV helpers
// (capsule-vs-sphere, capsule-vs-box, capsule-vs-capsule). Returns the closest point on AB.
function closestPointOnSegment(p, a, b) {
  const abx = b.x - a.x, aby = b.y - a.y, abz = b.z - a.z;
  const apx = p.x - a.x, apy = p.y - a.y, apz = p.z - a.z;
  const abLen2 = abx*abx + aby*aby + abz*abz;
  if (abLen2 < 1e-12) return new Vec3(a.x, a.y, a.z); // degenerate segment → treat as point
  const t = (apx*abx + apy*aby + apz*abz) / abLen2;
  const tc = t < 0 ? 0 : t > 1 ? 1 : t;
  return new Vec3(a.x + abx * tc, a.y + aby * tc, a.z + abz * tc);
}

// v0.8.1: capsule-vs-sphere MTV. Reduce to segment-vs-sphere: find the closest point on the
// capsule's axis segment to the sphere center, then test sphere-vs-sphere at that point.
function capsuleSphereMTV(cap, sph) {
  const closest = closestPointOnSegment(sph.center, cap.axisA, cap.axisB);
  const dx = closest.x - sph.center.x;
  const dy = closest.y - sph.center.y;
  const dz = closest.z - sph.center.z;
  const dist = Math.hypot(dx, dy, dz);
  const overlap = cap.r + sph.r - dist;
  if (overlap <= 0) return null;
  if (dist < 1e-9) return { normal: new Vec3(1, 0, 0), depth: overlap };
  return { normal: new Vec3(dx / dist, dy / dist, dz / dist), depth: overlap };
}

// v0.8.1: capsule-vs-box MTV. Find the closest point on the capsule's axis segment to the box
// (closest point on AABB to the closest point on segment — done by sampling the segment against
// the AABB's closest point and iterating once, which is exact for axis-aligned boxes since the
// closest-point-on-AABB function is piecewise-linear along any segment). Then test as
// sphere-vs-box at that point using the existing sphereBoxMTV.
function capsuleBoxMTV(cap, bx) {
  // Sample the segment: 8 internal points + 2 endpoints. Cheap, robust. For sub-grid precision
  // we could iterate but 10 samples is enough for game-scale capsules (h ≤ ~5 units).
  let best = null;
  const N = 10;
  for (let i = 0; i <= N; i++) {
    const t = i / N;
    const p = new Vec3(
      cap.axisA.x + (cap.axisB.x - cap.axisA.x) * t,
      cap.axisA.y + (cap.axisB.y - cap.axisA.y) * t,
      cap.axisA.z + (cap.axisB.z - cap.axisA.z) * t,
    );
    // Treat p as a sphere of radius cap.r against bx; keep the deepest overlap (largest depth).
    const r = sphereBoxMTV({ kind: 'sphere', center: p, r: cap.r }, bx);
    if (r && (!best || r.depth > best.depth)) best = r;
  }
  return best;
}

// v0.8.1: capsule-vs-capsule MTV. Reduce to segment-vs-segment: find the closest pair of points
// (one on each segment), then test sphere-vs-sphere at those points with radius = r1 + r2.
function capsuleCapsuleMTV(a, b) {
  const { pA, pB } = closestPointsSegmentSegment(a.axisA, a.axisB, b.axisA, b.axisB);
  const dx = pA.x - pB.x, dy = pA.y - pB.y, dz = pA.z - pB.z;
  const dist = Math.hypot(dx, dy, dz);
  const overlap = a.r + b.r - dist;
  if (overlap <= 0) return null;
  if (dist < 1e-9) return { normal: new Vec3(1, 0, 0), depth: overlap };
  return { normal: new Vec3(dx / dist, dy / dist, dz / dist), depth: overlap };
}

// Closest pair of points between two segments AB and CD. Standard algorithm: parametrize as
// A + s*(B-A) and C + t*(D-C), minimize |P(s) - Q(t)|², clamp s,t to [0,1]. Returns {pA, pB}.
function closestPointsSegmentSegment(a1, a2, b1, b2) {
  const d1x = a2.x - a1.x, d1y = a2.y - a1.y, d1z = a2.z - a1.z;
  const d2x = b2.x - b1.x, d2y = b2.y - b1.y, d2z = b2.z - b1.z;
  const rx = a1.x - b1.x, ry = a1.y - b1.y, rz = a1.z - b1.z;
  const a = d1x*d1x + d1y*d1y + d1z*d1z;
  const e = d2x*d2x + d2y*d2y + d2z*d2z;
  const f = d2x*rx + d2y*ry + d2z*rz;
  let s, t;
  if (a <= 1e-12 && e <= 1e-12) { s = 0; t = 0; }
  else if (a <= 1e-12) { s = 0; t = Math.max(0, Math.min(1, f / e)); }
  else {
    const c = d1x*rx + d1y*ry + d1z*rz;
    if (e <= 1e-12) { t = 0; s = Math.max(0, Math.min(1, -c / a)); }
    else {
      const b = d1x*d2x + d1y*d2y + d1z*d2z;
      const denom = a * e - b * b;
      s = denom > 1e-12 ? Math.max(0, Math.min(1, (b * f - c * e) / denom)) : 0;
      t = (b * s + f) / e;
      if (t < 0) { t = 0; s = Math.max(0, Math.min(1, -c / a)); }
      else if (t > 1) { t = 1; s = Math.max(0, Math.min(1, (b - c) / a)); }
    }
  }
  return {
    pA: new Vec3(a1.x + d1x * s, a1.y + d1y * s, a1.z + d1z * s),
    pB: new Vec3(b1.x + d2x * t, b1.y + d2y * t, b1.z + d2z * t),
  };
}

function sphereSphereMTV(a, b) {
  // Returns the minimum translation vector to push `a` out of `b` (or null if no overlap).
  // MTV points from b → a (so applying -MTV/2 to a and +MTV/2 to b separates them).
  const dx = a.center.x - b.center.x;
  const dy = a.center.y - b.center.y;
  const dz = a.center.z - b.center.z;
  const dist = Math.hypot(dx, dy, dz);
  const overlap = a.r + b.r - dist;
  if (overlap <= 0) return null;
  if (dist < 1e-9) return { normal: new Vec3(1, 0, 0), depth: overlap };
  return { normal: new Vec3(dx / dist, dy / dist, dz / dist), depth: overlap };
}

function sphereBoxMTV(s, b) {
  // Closest point on AABB to sphere center, then test distance < r.
  const cx = Math.max(b.center.x - b.half.x, Math.min(s.center.x, b.center.x + b.half.x));
  const cy = Math.max(b.center.y - b.half.y, Math.min(s.center.y, b.center.y + b.half.y));
  const cz = Math.max(b.center.z - b.half.z, Math.min(s.center.z, b.center.z + b.half.z));
  const dx = s.center.x - cx, dy = s.center.y - cy, dz = s.center.z - cz;
  const dist = Math.hypot(dx, dy, dz);
  const overlap = s.r - dist;
  if (overlap <= 0) return null;
  if (dist < 1e-9) {
    // Sphere center is inside the box — find the smallest face exit.
    const ex = b.half.x - Math.abs(s.center.x - b.center.x);
    const ey = b.half.y - Math.abs(s.center.y - b.center.y);
    const ez = b.half.z - Math.abs(s.center.z - b.center.z);
    const minE = Math.min(ex, ey, ez);
    if (minE === ex) return { normal: new Vec3(s.center.x > b.center.x ? 1 : -1, 0, 0), depth: ex + s.r };
    if (minE === ey) return { normal: new Vec3(0, s.center.y > b.center.y ? 1 : -1, 0), depth: ey + s.r };
    return { normal: new Vec3(0, 0, s.center.z > b.center.z ? 1 : -1), depth: ez + s.r };
  }
  return { normal: new Vec3(dx / dist, dy / dist, dz / dist), depth: overlap };
}

function boxBoxMTV(a, b) {
  // AABB-AABB overlap on all 3 axes; MTV = smallest single-axis penetration.
  const ax = a.center.x - b.center.x, ay = a.center.y - b.center.y, az = a.center.z - b.center.z;
  const ox = (a.half.x + b.half.x) - Math.abs(ax);
  const oy = (a.half.y + b.half.y) - Math.abs(ay);
  const oz = (a.half.z + b.half.z) - Math.abs(az);
  if (ox <= 0 || oy <= 0 || oz <= 0) return null;
  // Smallest axis wins.
  if (ox <= oy && ox <= oz) return { normal: new Vec3(ax >= 0 ? 1 : -1, 0, 0), depth: ox };
  if (oy <= ox && oy <= oz) return { normal: new Vec3(0, ay >= 0 ? 1 : -1, 0), depth: oy };
  return { normal: new Vec3(0, 0, az >= 0 ? 1 : -1), depth: oz };
}

function testColliders(a, b) {
  if (a.kind === 'sphere' && b.kind === 'sphere') return sphereSphereMTV(a, b);
  if (a.kind === 'sphere' && b.kind === 'box')   return sphereBoxMTV(a, b);
  if (a.kind === 'box'   && b.kind === 'sphere') {
    const r = sphereBoxMTV(b, a); // Note: result normal points sphere → box, flip it.
    return r ? { normal: new Vec3(-r.normal.x, -r.normal.y, -r.normal.z), depth: r.depth } : null;
  }
  if (a.kind === 'box' && b.kind === 'box') return boxBoxMTV(a, b);
  // v0.8.1: capsule pairs. All three combinations reduce to closest-point tests.
  if (a.kind === 'capsule' && b.kind === 'sphere') return capsuleSphereMTV(a, b);
  if (a.kind === 'sphere' && b.kind === 'capsule') {
    const r = capsuleSphereMTV(b, a);
    return r ? { normal: new Vec3(-r.normal.x, -r.normal.y, -r.normal.z), depth: r.depth } : null;
  }
  if (a.kind === 'capsule' && b.kind === 'box') return capsuleBoxMTV(a, b);
  if (a.kind === 'box' && b.kind === 'capsule') {
    const r = capsuleBoxMTV(b, a);
    return r ? { normal: new Vec3(-r.normal.x, -r.normal.y, -r.normal.z), depth: r.depth } : null;
  }
  if (a.kind === 'capsule' && b.kind === 'capsule') return capsuleCapsuleMTV(a, b);
  return null;
}

// v0.8.1: rebuild a capsule collider's axis endpoints from its current pose. Called from
// stepCollisions each pair-test (cheap: 1 quat-vec rotation + 2 vec adds) so capsule rotations
// are reflected in collision response without rebuilding the whole collider object.
function refreshCapsuleAxis(cap, pose) {
  if (!pose || !pose.pos) return;
  const axisY = pose.rot ? pose.rot.rotateVec(new Vec3(0, 1, 0)) : new Vec3(0, 1, 0);
  const half = cap.h / 2;
  cap.center = pose.pos;
  cap.axisA = new Vec3(pose.pos.x + axisY.x * half, pose.pos.y + axisY.y * half, pose.pos.z + axisY.z * half);
  cap.axisB = new Vec3(pose.pos.x - axisY.x * half, pose.pos.y - axisY.y * half, pose.pos.z - axisY.z * half);
}

function stepCollisions(world) {
  // Collect all entities with colliders.
  const bodies = [];
  for (const e of world.entities) {
    if (e._pendingRemove) continue; // v0.8.12: skip despawned entities in collision
    const c = colliderWorld(e);
    if (c) bodies.push({ entity: e, collider: c });
  }
  // v0.8.2: collect collision events for delivery AFTER the pairwise pass. Each pair that
  // produces a nonzero MTV queues a `Collide` event to BOTH entities (so each can react via
  // `&on(Collide):`). The payload includes `other` (the other entity's tag name), `normal`,
  // and `depth`. Delivery happens via world.deliverBroadcast so it flows through the same
  // `&on(Event)` mechanism as `^Hit`. We defer delivery until after the full pairwise pass so
  // user code in `&on(Collide)` sees the post-push-apart positions (not mid-resolution state).
  const collisionEvents = [];
  // Pairwise test (O(n²); fine for the reference's small entity counts).
  for (let i = 0; i < bodies.length; i++) {
    for (let j = i + 1; j < bodies.length; j++) {
      const A = bodies[i], B = bodies[j];
      // v0.8.8: collision layer/mask filtering. Each entity has an optional `collision_layer`
      // (a bitfield identifying which layer(s) it belongs to) and `collision_mask` (a bitfield
      // identifying which layers it COLLIDES with). Default: layer=0 (unset), mask=0xFFFFFFFF
      // (collide with everything). A pair collides iff:
      //   (A.mask & B.layer) !== 0  AND  (B.mask & A.layer) !== 0
      // (both directions must accept the other). This is the standard Unity/Unreal model.
      //
      // IMPORTANT: when an entity has NO collision_layer set (layer=0), it's treated as
      // "unfiltered" — it collides with everything regardless of the other entity's mask. This
      // preserves backward compat: existing entities without layer/mask fields collide exactly
      // as before. Only entities that EXPLICITLY set a non-zero collision_layer participate in
      // the bitfield filtering.
      // v0.8.9: cache field reads to avoid double Map lookups per entity per pair.
      const aLayer = A.entity.fields.get('collision_layer') || 0;
      const aMaskRaw = A.entity.fields.get('collision_mask');
      const aMask = aMaskRaw !== undefined ? aMaskRaw : 0xFFFFFFFF;
      const bLayer = B.entity.fields.get('collision_layer') || 0;
      const bMaskRaw = B.entity.fields.get('collision_mask');
      const bMask = bMaskRaw !== undefined ? bMaskRaw : 0xFFFFFFFF;
      // If EITHER entity has no layer (unfiltered), skip the bitfield check entirely.
      if (aLayer !== 0 && bLayer !== 0) {
        if ((aMask & bLayer) === 0 || (bMask & aLayer) === 0) continue;
      }
      // Update collider centers from current pose (cheap, avoids rebuild).
      A.collider.center = A.entity.locals.get('pose').pos;
      B.collider.center = B.entity.locals.get('pose').pos;
      // v0.8.1: capsules also need their axis segment refreshed (rot may have changed).
      if (A.collider.kind === 'capsule') refreshCapsuleAxis(A.collider, A.entity.locals.get('pose'));
      if (B.collider.kind === 'capsule') refreshCapsuleAxis(B.collider, B.entity.locals.get('pose'));
      const mtv = testColliders(A.collider, B.collider);
      if (!mtv) continue;
      // Push apart: each entity moves by half the MTV along the collision normal.
      const push = mtv.normal.mulScalar(mtv.depth / 2);
      const poseA = A.entity.locals.get('pose');
      const poseB = B.entity.locals.get('pose');
      // Only push bodies that are dynamic (have a mass field — Body3D).
      const dynA = A.entity.decl.base === 'Body3D';
      const dynB = B.entity.decl.base === 'Body3D';
      if (dynA && dynB) {
        poseA.pos = poseA.pos.add(push);
        poseB.pos = poseB.pos.sub(push);
      } else if (dynA) {
        poseA.pos = poseA.pos.add(push.mulScalar(2));
      } else if (dynB) {
        poseB.pos = poseB.pos.sub(push.mulScalar(2));
      }
      // Simple impulse: cancel relative velocity along collision normal.
      if (dynA && dynB) {
        const relV = poseA.vel.sub(poseB.vel);
        const vn = relV.x * mtv.normal.x + relV.y * mtv.normal.y + relV.z * mtv.normal.z;
        if (vn < 0) {
          const mA = A.entity.fields.get('mass') || 1;
          const mB = B.entity.fields.get('mass') || 1;
          const impulse = (2 * vn) / (1/mA + 1/mB);
          poseA.vel = poseA.vel.sub(mtv.normal.mulScalar(impulse / mA));
          poseB.vel = poseB.vel.add(mtv.normal.mulScalar(impulse / mB));
        }
      } else if (dynA) {
        const vn = poseA.vel.x * mtv.normal.x + poseA.vel.y * mtv.normal.y + poseA.vel.z * mtv.normal.z;
        if (vn < 0) poseA.vel = poseA.vel.sub(mtv.normal.mulScalar(2 * vn));
      } else if (dynB) {
        const vn = poseB.vel.x * (-mtv.normal.x) + poseB.vel.y * (-mtv.normal.y) + poseB.vel.z * (-mtv.normal.z);
        if (vn < 0) poseB.vel = poseB.vel.sub(mtv.normal.mulScalar(-2 * vn));
      }
      // v0.8.2: queue a Collide event for BOTH entities. The payload carries `other` (the
      // other entity's tag name, accessible as `other` in the event handler), `normal` (the
      // MTV normal pointing from B → A), and `depth` (overlap distance in world units).
      // Each entity sees the event with `other` set to its counterpart, so a single
      // `&on(Collide):` block on either entity fires once per contact per physics step.
      const nameA = A.entity._tagName || A.entity.decl.name;
      const nameB = B.entity._tagName || B.entity.decl.name;
      collisionEvents.push({
        toEntity: A.entity, payload: { other: nameB, other_entity: B.entity, normal: mtv.normal, depth: mtv.depth },
      });
      collisionEvents.push({
        toEntity: B.entity, payload: { other: nameA, other_entity: A.entity, normal: new Vec3(-mtv.normal.x, -mtv.normal.y, -mtv.normal.z), depth: mtv.depth },
      });
    }
  }
  // v0.8.2: deliver collision events. We dispatch directly to the specific entity's &on(Collide)
  // blocks (not a global broadcast) so only the two touching entities react. This matches the
  // semantics of a contact event: "I am touching entity X" is delivered to me, not to everyone.
  for (const ev of collisionEvents) {
    if (ev.toEntity._pendingRemove) continue; // v0.8.12: don't deliver collision events to despawned entities
    for (const block of ev.toEntity.blocksNamed('on')) {
      if (block.eventArg !== 'Collide') continue;
      runBlock(block, ev.toEntity, world, 1 / world.physicsHz, ev.payload);
    }
  }
}

// Ground plane: any Body3D below y=0 snaps up. Run BEFORE user &physics blocks so user code
// sees the corrected pose.
function stepGroundPlane(world) {
  for (const e of world.entities) {
    if (e._pendingRemove) continue; // v0.8.13: skip despawned in ground plane
    if (e.decl.base !== 'Body3D') continue;
    const pose = e.locals.get('pose');
    if (!pose) continue;
    if (pose.pos.y < 0) {
      pose.pos.y = 0;
      if (pose.vel.y < 0) pose.vel.y = 0;
    }
  }
}

// Raycast: tests origin + dir (normalized) * maxDist against every entity's collider.
// Returns the closest hit as a Vec3, or null if no hit within maxDist.
function raycastWorld(world, origin, dir, maxDist) {
  const d = dir.mag > 1e-9 ? dir.norm : dir;
  let bestT = maxDist;
  let bestHit = null;
  for (const e of world.entities) {
    if (e._pendingRemove) continue; // v0.8.13: skip despawned in raycast
    const c = colliderWorld(e);
    if (!c) continue;
    const t = raycastCollider(c, origin, d);
    if (t !== null && t >= 0 && t < bestT) {
      bestT = t;
      bestHit = new Vec3(origin.x + d.x * t, origin.y + d.y * t, origin.z + d.z * t);
    }
  }
  return bestHit;
}

function raycastCollider(c, o, d) {
  // Returns the nearest non-negative t such that o + d*t is on the collider, or null.
  if (c.kind === 'sphere') {
    // Quadratic: |o + t*d - center|² = r² → solve for t.
    const ox = o.x - c.center.x, oy = o.y - c.center.y, oz = o.z - c.center.z;
    const a = d.x*d.x + d.y*d.y + d.z*d.z;
    const b = 2 * (ox*d.x + oy*d.y + oz*d.z);
    const cc = ox*ox + oy*oy + oz*oz - c.r*c.r;
    const disc = b*b - 4*a*cc;
    if (disc < 0) return null;
    const sq = Math.sqrt(disc);
    const t1 = (-b - sq) / (2 * a);
    const t2 = (-b + sq) / (2 * a);
    if (t1 >= 0) return t1;
    if (t2 >= 0) return t2;
    return null;
  }
  if (c.kind === 'box') {
    // Slab method: for each axis, t_low = (min - o) / d, t_high = (max - o) / d.
    let tmin = -Infinity, tmax = Infinity;
    for (const ax of ['x', 'y', 'z']) {
      if (Math.abs(d[ax]) < 1e-9) {
        if (o[ax] < c.center[ax] - c.half[ax] || o[ax] > c.center[ax] + c.half[ax]) return null;
      } else {
        const lo = (c.center[ax] - c.half[ax] - o[ax]) / d[ax];
        const hi = (c.center[ax] + c.half[ax] - o[ax]) / d[ax];
        const tLo = Math.min(lo, hi), tHi = Math.max(lo, hi);
        if (tLo > tmin) tmin = tLo;
        if (tHi < tmax) tmax = tHi;
        if (tmin > tmax) return null;
      }
    }
    if (tmin >= 0) return tmin;
    if (tmax >= 0) return tmax;
    return null;
  }
  // v0.8.1: capsule — model as a swept sphere along the axis segment. Sample the segment with
  // small steps, test each sample as a sphere of radius c.r, keep the nearest non-negative t.
  // Sampled rather than solved analytically because the segment + sphere intersection is a
  // quartic and the reference's capsules are short (h ≤ ~5 units). 16 samples is plenty.
  if (c.kind === 'capsule') {
    let best = null;
    const N = 16;
    for (let i = 0; i <= N; i++) {
      const t = i / N;
      const p = {
        kind: 'sphere', center: new Vec3(
          c.axisA.x + (c.axisB.x - c.axisA.x) * t,
          c.axisA.y + (c.axisB.y - c.axisA.y) * t,
          c.axisA.z + (c.axisB.z - c.axisA.z) * t,
        ), r: c.r,
      };
      const tt = raycastCollider(p, o, d);
      if (tt !== null && tt >= 0 && (best === null || tt < best)) best = tt;
    }
    return best;
  }
  return null;
}

// ==========================================================================================
// SECTION 3: Control Flow Signals
// ==========================================================================================

class BreakSignal { constructor() {} }
class ContinueSignal { constructor() {} }
class ReturnSignal { constructor(value) { this.value = value; } }

// v0.6.1: GLB (binary glTF) loader — implemented in Step 4 of the v0.6 follow-up. Returns
//   { vertices: Float32Array (interleaved pos.xyz, normal.xyz, uv.xy = 8 floats), indices: Uint16Array }
// (the first primitive) for backward compat with v0.6 callers, or null if the file doesn't
// exist or fails to parse. v0.7 adds parseGLBMulti which returns ALL primitives as an array,
// for multi-mesh .glb files (model + collision proxy). The loader handles glTF 2.0 with
// UNSIGNED_SHORT or UNSIGNED_INT indices, FLOAT position/normal/UV accessors.
function loadGLB(filePath) {
  try {
    const fs = require('fs');
    if (!fs.existsSync(filePath)) return null;
    const buf = fs.readFileSync(filePath);
    return parseGLB(buf);
  } catch (e) { return null; }
}

// v0.7: load all primitives from a .glb file. Returns an array of
//   { vertices: Float32Array (interleaved 8 floats), indices: Uint16Array, material: number|null }
// or null if the file doesn't exist or fails to parse. Each primitive is independent — call
// sites can register each under a separate resource name (#X_0, #X_1, ...) or use only the first.
function loadGLBMulti(filePath) {
  try {
    const fs = require('fs');
    if (!fs.existsSync(filePath)) return null;
    const buf = fs.readFileSync(filePath);
    return parseGLBMulti(buf);
  } catch (e) { return null; }
}

// Pure parser — exposed for testing. Reads a Node Buffer with glTF 2.0 binary format.
// Format reference: https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#binary-gltf-layout
// Returns the FIRST primitive as { vertices, indices } for backward compat with v0.6 callers.
function parseGLB(buf) {
  const prims = parseGLBMulti(buf);
  if (!prims || prims.length === 0) return null;
  return prims[0];
}

// v0.7: parse all primitives. Returns an array of { vertices, indices, material } or null.
// Iterates gltf.meshes[].primitives[] in declaration order: mesh 0 prim 0, mesh 0 prim 1,
// mesh 1 prim 0, etc. Each primitive's indices are local (start at 0) — they reference the
// primitive's own vertex buffer, not a global one.
//
// v0.8.1: also parses top-level glTF `skins` (inverse-bind matrices + joint node indices) and
// `animations` (channels + samplers), attaching them to each primitive as `prim.skin.ibmData`,
// `prim.skin.jointNodes`, and `prim.animations` (array of {name, channels, duration}). Existing
// callers that only read `prim.vertices/indices/targets/skin.jointsData/weightsData` are
// unaffected — the new fields are additive.
function parseGLBMulti(buf) {
  // 12-byte header: magic (0x46546C67 = 'glTF'), version (2), length (total file).
  if (buf.length < 12) return null;
  const magic = buf.readUInt32LE(0);
  if (magic !== 0x46546C67) return null; // 'glTF' little-endian
  const version = buf.readUInt32LE(4);
  if (version !== 2) return null;
  const totalLength = buf.readUInt32LE(8);
  if (totalLength > buf.length) return null;
  // Walk chunks. Each chunk: 4-byte length, 4-byte type, data. Types: 0x4E4F534A = JSON, 0x004E4942 = BIN.
  let pos = 12;
  let jsonChunk = null, binChunk = null;
  while (pos + 8 <= totalLength) {
    const chunkLen = buf.readUInt32LE(pos);
    const chunkType = buf.readUInt32LE(pos + 4);
    const data = buf.slice(pos + 8, pos + 8 + chunkLen);
    if (chunkType === 0x4E4F534A) jsonChunk = data.toString('utf8');
    else if (chunkType === 0x004E4942) binChunk = data;
    pos += 8 + chunkLen;
    // Chunk data is padded to 4-byte alignment; skip padding.
    while (pos % 4 !== 0 && pos < totalLength) pos++;
  }
  if (!jsonChunk || !binChunk) return null;
  let gltf;
  try {
    // v0.6.1: strip trailing nulls/spaces (some glb writers — including ours in tests — pad
    // the JSON chunk to 4-byte alignment with zeros, which breaks JSON.parse).
    const trimmed = jsonChunk.replace(/[\x00\s]+$/, '');
    gltf = JSON.parse(trimmed);
  } catch (e) { return null; }
  if (!gltf.meshes || !gltf.meshes.length) return null;
  // v0.7: iterate all meshes + all primitives. Most .glb files have 1 mesh + 1 primitive, but
  // multi-primitive files let one .glb hold a model + its collision proxy (or LODs).
  const outPrims = [];
  for (const mesh of gltf.meshes) {
    if (!mesh.primitives || !mesh.primitives.length) continue;
    for (const prim of mesh.primitives) {
      const built = buildPrimitive(gltf, binChunk, prim);
      if (built) outPrims.push(built);
    }
  }
  if (outPrims.length === 0) return null;
  // v0.8.1: parse top-level skins + animations and attach to each primitive that has skinning
  // data (JOINTS_0 + WEIGHTS_0). Primitives without skinning keep `prim.skin` as-is.
  if (gltf.skins && gltf.skins.length > 0) {
    const skinData = parseGLBSkin(gltf, binChunk, gltf.skins[0]);
    for (const prim of outPrims) {
      if (prim.skin) {
        prim.skin.ibmData = skinData.ibmData;       // Float32Array of 16 * numJoints, or null
        prim.skin.jointNodes = skinData.jointNodes; // array of node indices
      }
    }
  }
  if (gltf.animations && gltf.animations.length > 0) {
    const anims = parseGLBAnimations(gltf, binChunk);
    for (const prim of outPrims) prim.animations = anims;
  }
  return outPrims;
}

// v0.8.1: parse a glTF skin definition. Returns { ibmData, jointNodes }:
//   ibmData: Float32Array of 16 * numJoints (the inverse-bind matrices, row-major per glTF),
//            or null if the skin has no inverseBindMatrices accessor (joints default to identity).
//   jointNodes: array of node indices (gltf.skins[0].joints).
function parseGLBSkin(gltf, binChunk, skinDef) {
  const jointNodes = Array.isArray(skinDef.joints) ? skinDef.joints.slice() : [];
  let ibmData = null;
  if (skinDef.inverseBindMatrices != null) {
    const ibmAcc = gltf.accessors[skinDef.inverseBindMatrices];
    if (ibmAcc) {
      const raw = decodeAccessor(gltf, binChunk, ibmAcc); // Float32Array (MAT4 → numComponents=16)
      if (raw) ibmData = raw;
    }
  }
  return { ibmData, jointNodes };
}

// v0.8.1: parse glTF animations into a normalized clip list. Each clip:
//   { name, duration, channels: [{ node, path, times, values, interpolation }] }
// `path` is 'translation' | 'rotation' | 'scale'. `times` is a Float32Array of keyframe times
// (seconds); `values` is a Float32Array of keyframe values (3 floats per keyframe for
// translation/scale, 4 for rotation). `interpolation` is 'LINEAR' (default), 'STEP', or
// 'CUBICSPLINE' (the latter is parsed but treated as LINEAR for sampling in the reference).
function parseGLBAnimations(gltf, binChunk) {
  if (!gltf.animations) return [];
  return gltf.animations.map((anim, idx) => {
    const channels = (anim.channels || []).map(ch => {
      const sampler = anim.samplers && anim.samplers[ch.sampler];
      if (!sampler) return null;
      const inputAcc = gltf.accessors[sampler.input];
      const outputAcc = gltf.accessors[sampler.output];
      if (!inputAcc || !outputAcc) return null;
      const times = decodeAccessor(gltf, binChunk, inputAcc);
      const values = decodeAccessor(gltf, binChunk, outputAcc);
      if (!times || !values) return null;
      return {
        node: ch.target.node,
        path: ch.target.path, // 'translation' | 'rotation' | 'scale'
        times, values,
        interpolation: sampler.interpolation || 'LINEAR',
      };
    }).filter(Boolean);
    const duration = channels.reduce((m, ch) => {
      const last = ch.times[ch.times.length - 1];
      return (typeof last === 'number' && last > m) ? last : m;
    }, 0);
    return {
      name: anim.name || ('anim_' + idx),
      channels,
      duration,
    };
  });
}

// v0.8.1: sample an animation clip at time `t`. Returns a Map<nodeIndex, {translation, rotation,
// scale}> — the per-node TRS to apply. For channels not present in the clip, the node keeps its
// default (identity) TRS. Linear interpolation between keyframes; rotation uses slerp. If `loop`
// is true, time wraps around `clip.duration`; otherwise the last keyframe is held.
function sampleAnimation(clip, t, loop) {
  if (!clip || !clip.channels) return new Map();
  let time = t;
  if (clip.duration > 0) {
    if (loop) {
      time = ((time % clip.duration) + clip.duration) % clip.duration; // positive modulo
    } else {
      time = Math.min(time, clip.duration);
    }
  }
  const out = new Map(); // node → { translation: Vec3, rotation: Quat, scale: Vec3 }
  for (const ch of clip.channels) {
    if (!out.has(ch.node)) out.set(ch.node, {
      translation: new Vec3(0, 0, 0),
      rotation: Quat.identity(),
      scale: new Vec3(1, 1, 1),
    });
    const trs = out.get(ch.node);
    const { times, values } = ch;
    if (times.length === 0) continue;
    // Find the surrounding keyframe pair.
    let i = 0;
    while (i < times.length - 1 && times[i + 1] < time) i++;
    const t0 = times[i], t1 = times[i + 1] != null ? times[i + 1] : t0;
    let frac = (t1 > t0) ? (time - t0) / (t1 - t0) : 0;
    frac = Math.max(0, Math.min(1, frac));
    const components = ch.path === 'rotation' ? 4 : 3;
    // v0.8.10 fix: at or past last keyframe, return final values directly — no interpolation.
    // Without this, v1 = values.slice((i+1)*components, ...) reads past the array end → empty
    // → v1[0] is undefined → (undefined - v0[0]) * frac = NaN. Breaks ALL animation at loop boundary.
    if (i >= times.length - 1) {
      const v = values.slice(i * components, (i + 1) * components);
      if (ch.path === 'translation') trs.translation = new Vec3(v[0]||0, v[1]||0, v[2]||0);
      else if (ch.path === 'rotation') trs.rotation = new Quat(v[0]||0, v[1]||0, v[2]||0, v[3]||1).normalized;
      else if (ch.path === 'scale') trs.scale = new Vec3(v[0]||1, v[1]||1, v[2]||1);
      continue;
    }
    const v0 = values.slice(i * components, (i + 1) * components);
    const v1 = values.slice((i + 1) * components, (i + 2) * components);
    if (ch.path === 'translation') {
      trs.translation = new Vec3(
        v0[0] + (v1[0] - v0[0]) * frac,
        v0[1] + (v1[1] - v0[1]) * frac,
        v0[2] + (v1[2] - v0[2]) * frac,
      );
    } else if (ch.path === 'scale') {
      trs.scale = new Vec3(
        v0[0] + (v1[0] - v0[0]) * frac,
        v0[1] + (v1[1] - v0[1]) * frac,
        v0[2] + (v1[2] - v0[2]) * frac,
      );
    } else if (ch.path === 'rotation') {
      const q0 = new Quat(v0[0], v0[1], v0[2], v0[3]);
      const q1 = new Quat(v1[0], v1[1], v1[2], v1[3]);
      trs.rotation = q0.slerp(q1, frac);
    }
  }
  return out;
}

// v0.8.1: build the per-joint 4x4 matrix array used by `applySkinning` in render3d. For each
// joint node in `skin.jointNodes`, compose the joint's TRS (from `sampleAnimation` output, or
// from the entity's manual `~joint_pos`/`~joint_rot`/`~joint_scl` arrays if set) into a local
// matrix, multiply by the inverse-bind matrix, and store as a flat Float32Array of 16 * N.
//
// Returns null if the mesh has no skin or no joint data — caller falls back to non-skinned path.
function buildJointMatrices(mesh, sampledTRS, entity) {
  if (!mesh || !mesh.skin || !mesh.skin.jointNodes) return null;
  const joints = mesh.skin.jointNodes;
  const numJoints = joints.length;
  if (numJoints === 0) return null;
  const out = new Float32Array(16 * numJoints);
  // Manual override arrays (entity fields). If present, these take precedence over sampled TRS.
  const manualPos = entity && entity.fields.get('joint_pos');
  const manualRot = entity && entity.fields.get('joint_rot');
  const manualScl = entity && entity.fields.get('joint_scl');
  const ibm = mesh.skin.ibmData; // Float32Array of 16 * numJoints, or null (identity)
  for (let j = 0; j < numJoints; j++) {
    const nodeIdx = joints[j];
    // Resolve this joint's TRS. Priority: manual override (if set), else sampled (if present),
    // else identity.
    let t, r, s;
    if (manualPos && Array.isArray(manualPos) && manualPos[j]) t = manualPos[j];
    else if (sampledTRS && sampledTRS.has(nodeIdx)) t = sampledTRS.get(nodeIdx).translation;
    else t = new Vec3(0, 0, 0);
    if (manualRot && Array.isArray(manualRot) && manualRot[j]) r = manualRot[j];
    else if (sampledTRS && sampledTRS.has(nodeIdx)) r = sampledTRS.get(nodeIdx).rotation;
    else r = Quat.identity();
    if (manualScl && Array.isArray(manualScl) && manualScl[j]) s = manualScl[j];
    else if (sampledTRS && sampledTRS.has(nodeIdx)) s = sampledTRS.get(nodeIdx).scale;
    else s = new Vec3(1, 1, 1);
    // Local TRS → 4x4 matrix. glTF uses column-major; we use Mat4 (row-major) and transpose on write.
    const local = trsToMat4(t, r, s);
    // jointMatrix = local × inverseBind.
    let jointMat = local;
    if (ibm && ibm.length >= 16 * (j + 1)) {
      const ibmMat = mat4FromFlat(ibm, j * 16);
      jointMat = local.mulMat4(ibmMat);
    }
    // Write as column-major (glTF convention) so applySkinning can multiply straight through.
    const d = jointMat.d;
    const base = j * 16;
    out[base + 0] = d[0]; out[base + 1] = d[1]; out[base + 2] = d[2]; out[base + 3] = d[3];
    out[base + 4] = d[4]; out[base + 5] = d[5]; out[base + 6] = d[6]; out[base + 7] = d[7];
    out[base + 8] = d[8]; out[base + 9] = d[9]; out[base + 10] = d[10]; out[base + 11] = d[11];
    out[base + 12] = d[12]; out[base + 13] = d[13]; out[base + 14] = d[14]; out[base + 15] = d[15];
  }
  return out;
}

// Compose a TRS into a 4x4 matrix. Matches Transform.toMat4() semantics: T * R * S.
function trsToMat4(t, r, s) {
  // Start with scale, then rotate, then translate (matches glTF TRS compose).
  const rot = r || Quat.identity();
  const scl = s || new Vec3(1, 1, 1);
  const trans = t || new Vec3(0, 0, 0);
  // Build rotation matrix from quaternion.
  const { x, y, z, w } = rot;
  const rMat = new Mat4();
  rMat.d[0] = 1 - 2 * (y*y + z*z); rMat.d[4] = 2 * (x*y - z*w);     rMat.d[8] = 2 * (x*z + y*w);     rMat.d[12] = 0;
  rMat.d[1] = 2 * (x*y + z*w);     rMat.d[5] = 1 - 2 * (x*x + z*z); rMat.d[9] = 2 * (y*z - x*w);     rMat.d[13] = 0;
  rMat.d[2] = 2 * (x*z - y*w);     rMat.d[6] = 2 * (y*z + x*w);     rMat.d[10] = 1 - 2 * (x*x + y*y); rMat.d[14] = 0;
  rMat.d[3] = 0; rMat.d[7] = 0; rMat.d[11] = 0; rMat.d[15] = 1;
  // v0.8.9 fix: apply scale per-COLUMN (not per-row). In column-major, element(r,c) = d[c*4+r].
  // R×S means element(r,c) = R(r,c)*S[c], so scale varies by column c: d[c*4+r] *= S[c].
  // The old per-row code d[r*4+c] *= S[r] produced S^T×R, distorting non-uniform scale.
  rMat.d[0] *= scl.x; rMat.d[1] *= scl.x; rMat.d[2] *= scl.x;
  rMat.d[4] *= scl.y; rMat.d[5] *= scl.y; rMat.d[6] *= scl.y;
  rMat.d[8] *= scl.z; rMat.d[9] *= scl.z; rMat.d[10] *= scl.z;
  // Apply translation.
  rMat.d[12] = trans.x; rMat.d[13] = trans.y; rMat.d[14] = trans.z;
  return rMat;
}

// Read a 4x4 matrix from a flat Float32Array at the given offset (column-major per glTF).
// v0.8.9 fix: Mat4.d IS column-major (proven by mulVec3, mulMat4, lookAt, perspective).
// No transpose needed — straight copy from the glTF column-major data.
function mat4FromFlat(flat, offset) {
  const m = new Mat4();
  // glTF and Mat4.d both use column-major — no transpose needed.
  for (let i = 0; i < 16; i++) m.d[i] = flat[offset + i];
  return m;
}

// v0.7: build a single primitive's interleaved vertex + index buffers from a glTF primitive.
function buildPrimitive(gltf, binChunk, prim) {
  const attrs = prim.attributes || {};
  const posAcc = gltf.accessors[attrs.POSITION];
  const normAcc = attrs.NORMAL != null ? gltf.accessors[attrs.NORMAL] : null;
  const uvAcc = attrs.TEXCOORD_0 != null ? gltf.accessors[attrs.TEXCOORD_0] : null;
  const idxAcc = prim.indices != null ? gltf.accessors[prim.indices] : null;
  // v0.8: skinning data (JOINTS_0 + WEIGHTS_0). JOINTS_0 is typically UNSIGNED_BYTE (5121) or
  // UNSIGNED_SHORT (5123); WEIGHTS_0 is FLOAT (5126). Both are VEC4 per vertex.
  const jointsAcc = attrs.JOINTS_0 != null ? gltf.accessors[attrs.JOINTS_0] : null;
  const weightsAcc = attrs.WEIGHTS_0 != null ? gltf.accessors[attrs.WEIGHTS_0] : null;
  if (!posAcc) return null;
  // Decode each accessor via its bufferView + buffer.
  const positions = decodeAccessor(gltf, binChunk, posAcc);
  const normals = normAcc ? decodeAccessor(gltf, binChunk, normAcc) : null;
  const uvs = uvAcc ? decodeAccessor(gltf, binChunk, uvAcc) : null;
  const indices = idxAcc ? decodeAccessor(gltf, binChunk, idxAcc) : null;
  const joints = jointsAcc ? decodeAccessor(gltf, binChunk, jointsAcc) : null;
  const weights = weightsAcc ? decodeAccessor(gltf, binChunk, weightsAcc) : null;
  if (!positions) return null;
  // Build interleaved (pos.xyz, normal.xyz, uv.xy) = 8 floats per vertex.
  const numVerts = Math.floor(positions.length / 3);
  const verts = new Float32Array(numVerts * 8);
  for (let i = 0; i < numVerts; i++) {
    verts[i*8]   = positions[i*3];
    verts[i*8+1] = positions[i*3+1];
    verts[i*8+2] = positions[i*3+2];
    if (normals) { verts[i*8+3] = normals[i*3]; verts[i*8+4] = normals[i*3+1]; verts[i*8+5] = normals[i*3+2]; }
    else { verts[i*8+3] = 0; verts[i*8+4] = 1; verts[i*8+5] = 0; }
    if (uvs) { verts[i*8+6] = uvs[i*2]; verts[i*8+7] = uvs[i*2+1]; }
    else { verts[i*8+6] = 0; verts[i*8+7] = 0; }
  }
  // Build indices. If no index accessor, generate a sequential list.
  let idxArr;
  if (indices) {
    idxArr = new Uint16Array(indices.length);
    for (let i = 0; i < indices.length; i++) idxArr[i] = indices[i];
  } else {
    idxArr = new Uint16Array(numVerts);
    for (let i = 0; i < numVerts; i++) idxArr[i] = i;
  }
  const out = { vertices: verts, indices: idxArr, material: prim.material != null ? prim.material : null };
  // v0.8: morph targets — array of {positions: Float32Array(3N), normals: Float32Array(3N)|null}.
  // At draw time, the renderer blends: pos += weight * target.positions. Default weights = 0
  // (no morph); the user sets `~morph_weights: [w0, w1, ...]` on the entity to animate.
  if (prim.targets && prim.targets.length > 0) {
    out.targets = prim.targets.map(t => {
      const tPos = t.POSITION != null ? decodeAccessor(gltf, binChunk, gltf.accessors[t.POSITION]) : null;
      const tNorm = t.NORMAL != null ? decodeAccessor(gltf, binChunk, gltf.accessors[t.NORMAL]) : null;
      return { positions: tPos, normals: tNorm };
    });
  }
  // v0.8: skinning — store joint indices + weights per vertex. The renderer transforms each
  // vertex by the weighted sum of joint matrices. For the reference interpreter, joint matrices
  // default to identity (rest pose) unless the user sets them via `~joint_pos: [...]` /
  // `~joint_rot: [...]` / `~joint_scl: [...]` arrays on the entity. Each joint's matrix is the
  // inverse-bind matrix composed with the joint's current TRS.
  if (joints && weights) {
    out.skin = {
      // joints is Uint8Array or Uint16Array per vertex × 4; weights is Float32Array per vertex × 4.
      jointsData: joints,
      weightsData: weights,
      // Look up the inverse-bind matrices from the skin referenced by the mesh's parent node.
      // For simplicity, we resolve the skin at draw time using the entity's fields.
    };
  }
  return out;
}

// Decode a glTF accessor into a typed array. Returns Float32Array for FLOAT (5126),
// Uint16Array for UNSIGNED_SHORT (5123), Uint32Array for UNSIGNED_INT (5125).
function decodeAccessor(gltf, binChunk, accessor) {
  const bufferView = gltf.bufferViews[accessor.bufferView];
  if (!bufferView) return null;
  const buffer = gltf.buffers[bufferView.buffer];
  if (!buffer) return null;
  // Only buffer 0 is the BIN chunk (inline). External buffers (data URIs) are unsupported.
  if (bufferView.buffer !== 0) return null;
  // Slice the buffer view from the BIN chunk.
  const offset = (bufferView.byteOffset || 0) + (accessor.byteOffset || 0);
  const count = accessor.count;
  const componentType = accessor.componentType;
  const type = accessor.type; // 'VEC3', 'VEC2', 'VEC4', 'SCALAR', 'MAT4' (v0.8.1), 'MAT3'
  // v0.8.1: MAT4 (16 components) for inverse-bind matrices; MAT3 (9) for completeness (unused).
  const numComponents = type === 'VEC3' ? 3 : type === 'VEC2' ? 2 : type === 'VEC4' ? 4
    : type === 'MAT4' ? 16 : type === 'MAT3' ? 9 : 1;
  if (componentType === 5126) { // FLOAT
    const out = new Float32Array(count * numComponents);
    for (let i = 0; i < out.length; i++) out[i] = binChunk.readFloatLE(offset + i * 4);
    return out;
  }
  if (componentType === 5123) { // UNSIGNED_SHORT
    const out = new Uint16Array(count * numComponents);
    for (let i = 0; i < out.length; i++) out[i] = binChunk.readUInt16LE(offset + i * 2);
    return out;
  }
  if (componentType === 5121) { // v0.8: UNSIGNED_BYTE (used for JOINTS_0 in low-poly skinned meshes)
    const out = new Uint8Array(count * numComponents);
    for (let i = 0; i < out.length; i++) out[i] = binChunk[offset + i];
    return out;
  }
  if (componentType === 5125) { // UNSIGNED_INT
    const out = new Uint32Array(count * numComponents);
    for (let i = 0; i < out.length; i++) out[i] = binChunk.readUInt32LE(offset + i * 4);
    return out;
  }
  return null;
}

// ==========================================================================================
// SECTION 3.5 — v0.6 NavMesh A* Pathfinder
// ==========================================================================================
//
// Replaces the v0.5 stub `?path(from, to)` which just returned [from, to]. Now reads an optional
// grid file (text format, see below) and runs A* with 8-directional movement + Euclidean heuristic.
// If no .nav file is found, falls back to the straight-line stub (current behavior).
//
// Grid file format (text):
//
//   v0.6 single-layer (still supported, Y implicit = 0):
//     line 1: `width height` (two integers)
//     next `height` lines: `width` chars each, `0` = walkable, `1` = wall
//
//   v0.8.1 multi-layer (for multi-level geometry — stairs, platforms, bridges):
//     line 1: `layers N`
//     for each layer:
//       `layer INDEX y=Y_VALUE`
//       `width height`
//       next `height` lines: `width` chars each
//     optional connection lines (bidirectional walkable pairs between layers):
//       `connect (x1,z1,y1) (x2,z2,y2)`
//     Multiple `connect` lines establish stairs/ramps between layers. A connection is only
//     valid if both endpoint cells are walkable (0) in their respective layers.
//
// Examples (10×10 single-layer):
//   10 10
//   0000000000
//   0001111000
//   0001001000
//   ...
//
// Multi-layer (two 5×5 layers connected by a stair at cell (2,2)):
//   layers 2
//   layer 0 y=0.0
//     5 5
//     00000
//     00000
//     00000
//     00000
//     00000
//   layer 1 y=3.0
//     5 5
//     00000
//     00000
//     00000
//     00000
//     00000
//   connect (2,2,0) (2,2,3)
//
// The .nav file path is the NavMesh3D's `~source` field (e.g. `~source: "level.nav"`). The path
// is loaded once on the first `?path` call (lazy load + memoize).
//
// Return shape: `{ grid, w, h, layers, connections }`. For single-layer files, `layers` is a
// 1-element array `[{grid, w, h, y:0}]` and `connections` is `[]`. The top-level `grid/w/h`
// alias `layers[0]` for backward compat with naive callers (existing aStar/navRaycast fast path).

const _navCache = new WeakMap(); // world → Map<sourcePath, nav>

function loadNavGrid(world, sourcePath) {
  if (!sourcePath) return null;
  let cache = _navCache.get(world);
  if (!cache) { cache = new Map(); _navCache.set(world, cache); }
  if (cache.has(sourcePath)) return cache.get(sourcePath);
  let result = null;
  try {
    const fs = require('fs');
    const path = require('path');
    const fullPath = path.isAbsolute(sourcePath) ? sourcePath : path.resolve(sourcePath);
    if (fs.existsSync(fullPath)) {
      const text = fs.readFileSync(fullPath, 'utf8');
      // Strip comments + blank lines but PRESERVE the structure (we need layer index/y lines).
      const rawLines = text.split('\n').map(l => l.trim());
      // v0.9.2: a line starting with '#' is a comment unless it is a grid row — made only of
      // '#', '.', '0', '1' — so a row whose first cell is a wall is no longer dropped.
      const isGridRow = (l) => /^[#.01]+$/.test(l);
      const lines = rawLines.filter(l => l.length > 0 && (!l.startsWith('#') || isGridRow(l)));
      if (lines.length >= 1) {
        const first = lines[0];
        if (first.startsWith('layers')) {
          result = parseMultiLayerNav(lines);
        } else {
          // v0.6 single-layer: wrap as 1-layer for uniform handling.
          const single = parseSingleLayerNav(lines);
          if (single) {
            result = {
              grid: single.grid, w: single.w, h: single.h,
              layers: [{ grid: single.grid, w: single.w, h: single.h, y: 0 }],
              connections: [],
            };
          }
        }
      }
    }
  } catch (e) { /* file missing or unreadable → fall back to straight-line */ }
  cache.set(sourcePath, result);
  return result;
}

// Parse a v0.6 single-layer nav file. Returns {grid, w, h} or null.
function parseSingleLayerNav(lines) {
  if (lines.length < 1) return null;
  const [w, h] = lines[0].split(/\s+/).map(s => parseInt(s, 10));
  if (!w || !h || lines.length < h + 1) return null;
  const grid = new Uint8Array(w * h);
  for (let y = 0; y < h; y++) {
    const row = lines[y + 1] || '';
    for (let x = 0; x < w; x++) grid[y * w + x] = (row[x] === '1' || row[x] === '#') ? 1 : 0;
  }
  return { grid, w, h };
}

// Parse a v0.8.1 multi-layer nav file. Returns {grid, w, h, layers, connections} or null.
function parseMultiLayerNav(lines) {
  // First line: `layers N`
  const m = lines[0].match(/^layers\s+(\d+)$/);
  if (!m) return null;
  const numLayers = parseInt(m[1], 10);
  if (!numLayers) return null;
  const layers = [];
  const connections = [];
  let i = 1;
  while (i < lines.length && layers.length < numLayers) {
    const line = lines[i];
    // Layer header: `layer INDEX y=Y_VALUE` (Y_VALUE is a float).
    const lm = line.match(/^layer\s+(\d+)\s+y=([-\d.]+)$/);
    if (!lm) { i++; continue; }
    const layerIdx = parseInt(lm[1], 10);
    const layerY = parseFloat(lm[2]);
    if (layerIdx !== layers.length) {
      // Out-of-order layer declaration — skip (we accept layers in declaration order).
    }
    // Next line: `width height`. The width/height line may be indented (we already trimmed).
    i++;
    if (i >= lines.length) break;
    const [w, h] = lines[i].split(/\s+/).map(s => parseInt(s, 10));
    if (!w || !h) { i++; continue; }
    i++;
    // Next h lines: the grid rows.
    if (i + h > lines.length) break;
    const grid = new Uint8Array(w * h);
    for (let y = 0; y < h; y++) {
      const row = lines[i + y] || '';
      for (let x = 0; x < w; x++) grid[y * w + x] = (row[x] === '1' || row[x] === '#') ? 1 : 0;
    }
    i += h;
    layers.push({ grid, w, h, y: layerY });
  }
  // Remaining lines: `connect (x1,z1,y1) (x2,z2,y2)` — bidirectional bridges between cells.
  while (i < lines.length) {
    const line = lines[i];
    const cm = line.match(/^connect\s+\((-?\d+),(-?\d+),(-?[\d.]+)\)\s+\((-?\d+),(-?\d+),(-?[\d.]+)\)$/);
    if (cm) {
      connections.push({
        a: { x: parseInt(cm[1], 10), z: parseInt(cm[2], 10), y: parseFloat(cm[3]) },
        b: { x: parseInt(cm[4], 10), z: parseInt(cm[5], 10), y: parseFloat(cm[6]) },
      });
    }
    i++;
  }
  if (layers.length === 0) return null;
  // Top-level grid/w/h alias the first layer for backward compat with the existing aStar
  // fast path (which only handles single-layer grids).
  const first = layers[0];
  return { grid: first.grid, w: first.w, h: first.h, layers, connections };
}

// A* on the 2D grid; result is a Vec3[] (y=0, xz = grid coords + 0.5 offsets).
// v0.8.1: if the nav has multiple layers OR inter-layer connections, dispatches to aStarMultiLayer.
// v0.8.8: binary min-heap for A* open set. Replaces the O(n) linear scan that made
// pathfinding O(n²) on large grids. Standard array-based heap: parent at floor(i/2),
// children at 2i and 2i+1. Elements are {x, y, g, f} objects compared by `.f`.
class MinHeap {
  constructor() { this.data = []; }
  get length() { return this.data.length; }
  push(node) {
    this.data.push(node);
    this._siftUp(this.data.length - 1);
  }
  pop() {
    if (this.data.length === 0) return null;
    const top = this.data[0];
    const last = this.data.pop();
    if (this.data.length > 0) { this.data[0] = last; this._siftDown(0); }
    return top;
  }
  _siftUp(i) {
    const d = this.data;
    while (i > 0) {
      const parent = (i - 1) >> 1;
      if (d[i].f < d[parent].f) {
        const tmp = d[i]; d[i] = d[parent]; d[parent] = tmp;
        i = parent;
      } else break;
    }
  }
  _siftDown(i) {
    const d = this.data, n = d.length;
    while (true) {
      let smallest = i;
      const l = 2 * i + 1, r = 2 * i + 2;
      if (l < n && d[l].f < d[smallest].f) smallest = l;
      if (r < n && d[r].f < d[smallest].f) smallest = r;
      if (smallest !== i) {
        const tmp = d[i]; d[i] = d[smallest]; d[smallest] = tmp;
        i = smallest;
      } else break;
    }
  }
}

function aStar(nav, fromV3, toV3) {
  // v0.8.1: multi-layer dispatch. If there are connections between layers (or 2+ layers), use
  // the multi-layer A* which treats each layer as a separate grid and bridges them via the
  // connection list. Otherwise the original 2D A* is a strict fast path.
  if (nav && nav.layers && nav.layers.length > 0 && (nav.layers.length > 1 || (nav.connections && nav.connections.length > 0))) {
    return aStarMultiLayer(nav, fromV3, toV3);
  }
  const { grid, w, h } = nav;
  const sx = Math.max(0, Math.min(w - 1, Math.floor(fromV3.x)));
  const sy = Math.max(0, Math.min(h - 1, Math.floor(fromV3.z)));
  const ex = Math.max(0, Math.min(w - 1, Math.floor(toV3.x)));
  const ey = Math.max(0, Math.min(h - 1, Math.floor(toV3.z)));
  if (grid[sy * w + sx] === 1 || grid[ey * w + ex] === 1) return null;
  // v0.8.8: use binary min-heap instead of flat array + linear scan.
  const open = new MinHeap();
  open.push({ x: sx, y: sy, g: 0, f: Math.hypot(ex - sx, ey - sy) });
  const cameFrom = new Map();
  const gScore = new Map();
  gScore.set(sx + ',' + sy, 0);
  const DIRS = [[1,0,1],[-1,0,1],[0,1,1],[0,-1,1],[1,1,1.414],[-1,1,1.414],[1,-1,1.414],[-1,-1,1.414]];
  let iters = 0;
  while (open.length > 0 && iters < w * h * 4) {
    iters++;
    const cur = open.pop();
    if (cur.x === ex && cur.y === ey) {
      const path = [];
      let cx = cur.x, cy = cur.y;
      while (true) {
        path.unshift(new Vec3(cx + 0.5, 0, cy + 0.5));
        if (cx === sx && cy === sy) break;
        const prev = cameFrom.get(cx + ',' + cy);
        if (!prev) break;
        cx = prev.x; cy = prev.y;
      }
      return smoothPath(path);
    }
    for (const [dx, dy, cost] of DIRS) {
      const nx = cur.x + dx, ny = cur.y + dy;
      if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
      if (grid[ny * w + nx] === 1) continue;
      if (dx !== 0 && dy !== 0) {
        if (grid[cur.y * w + nx] === 1 || grid[ny * w + cur.x] === 1) continue;
      }
      const tentativeG = cur.g + cost;
      const key = nx + ',' + ny;
      if (gScore.has(key) && gScore.get(key) <= tentativeG) continue;
      gScore.set(key, tentativeG);
      cameFrom.set(key, { x: cur.x, y: cur.y });
      open.push({ x: nx, y: ny, g: tentativeG, f: tentativeG + Math.hypot(ex - nx, ey - ny) });
    }
  }
  return null;
}

function smoothPath(path) {
  if (path.length < 3) return path;
  const out = [path[0]];
  for (let i = 1; i < path.length - 1; i++) {
    const a = out[out.length - 1], b = path[i], c = path[i + 1];
    // v0.8.1: also keep b if its Y differs from a's or c's Y — this preserves bridge-transition
    // waypoints in multi-layer paths (which would otherwise be dropped as XZ-collinear).
    const xzCross = (b.x - a.x) * (c.z - b.z) - (b.z - a.z) * (c.x - b.x);
    const yChanges = Math.abs(b.y - a.y) > 1e-6 || Math.abs(c.y - b.y) > 1e-6;
    if (Math.abs(xzCross) > 1e-6 || yChanges) out.push(b); // not collinear OR Y transition → keep
  }
  out.push(path[path.length - 1]);
  return out;
}

// v0.8.1: Multi-layer A* — runs A* across multiple Y-separated grids joined by connection edges.
// Each node is identified by (layerIdx, cellX, cellZ). Within a layer, the 8-directional
// neighbour rules from the single-layer A* apply. Connections add bridge edges between two
// nodes in different layers (or the same layer at non-adjacent cells, e.g. a teleport).
//
// Layer selection for endpoints: each from/to Vec3 snaps to the layer whose Y is closest to
// the query point's Y. If multiple layers exist and from/to land on different layers, A* must
// traverse a connection (a stair/ramp) to reach the destination layer — if no connection
// exists between the two layers, no path is returned.
//
// Returns a Vec3[] path in world space (each waypoint carries its layer's Y), or null.
function aStarMultiLayer(nav, fromV3, toV3) {
  const { layers, connections } = nav;
  if (!layers || layers.length === 0) return null;
  // Pick the closest layer to each endpoint's Y. Tie-break by index (lower wins).
  const closestLayer = (y) => {
    let best = 0, bestDist = Infinity;
    for (let i = 0; i < layers.length; i++) {
      const d = Math.abs(layers[i].y - y);
      if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
  };
  const startLayer = closestLayer(fromV3.y);
  const endLayer = closestLayer(toV3.y);
  const startGrid = layers[startLayer];
  const endGrid = layers[endLayer];
  // Snap endpoints to layer cells.
  const sx = Math.max(0, Math.min(startGrid.w - 1, Math.floor(fromV3.x)));
  const sz = Math.max(0, Math.min(startGrid.h - 1, Math.floor(fromV3.z)));
  const ex = Math.max(0, Math.min(endGrid.w - 1, Math.floor(toV3.x)));
  const ez = Math.max(0, Math.min(endGrid.h - 1, Math.floor(toV3.z)));
  if (startGrid.grid[sz * startGrid.w + sx] === 1) return null;
  if (endGrid.grid[ez * endGrid.w + ex] === 1) return null;
  // Precompute a Y-aware heuristic: Euclidean distance in (x, y, z) using layer Y values.
  const layerY = (i) => layers[i].y;
  const h = (lx, lz, li) => Math.hypot(ex + 0.5 - lx - 0.5, layerY(li) - layerY(endLayer), ez + 0.5 - lz - 0.5);
  // v0.8.9: use MinHeap instead of flat array + linear scan.
  const open = new MinHeap();
  open.push({ li: startLayer, x: sx, z: sz, g: 0, f: h(sx, sz, startLayer) });
  const cameFrom = new Map(); // key "li,x,z" → { li, x, z }
  const gScore = new Map();
  gScore.set(startLayer + ',' + sx + ',' + sz, 0);
  // Precompute connections indexed by (layerIdx,x,z) for fast bridge lookup.
  const connMap = new Map(); // key → array of { li, x, z, cost }
  const connKey = (li, x, z) => li + ',' + x + ',' + z;
  for (const c of connections) {
    // Find the layer whose Y matches the connection endpoint's Y (closest match).
    const la = closestLayer(c.a.y), lb = closestLayer(c.b.y);
    // Snap connection endpoints to integer cells in their layers.
    const ax = Math.max(0, Math.min(layers[la].w - 1, c.a.x));
    const az = Math.max(0, Math.min(layers[la].h - 1, c.a.z));
    const bx = Math.max(0, Math.min(layers[lb].w - 1, c.b.x));
    const bz = Math.max(0, Math.min(layers[lb].h - 1, c.b.z));
    // Skip if either endpoint is a wall.
    if (layers[la].grid[az * layers[la].w + ax] === 1) continue;
    if (layers[lb].grid[bz * layers[lb].w + bx] === 1) continue;
    // Bridge cost = 3D distance between endpoints (handles vertical separation realistically).
    const cost = Math.hypot(bx - ax, layers[lb].y - layers[la].y, bz - az);
    if (!connMap.has(connKey(la, ax, az))) connMap.set(connKey(la, ax, az), []);
    if (!connMap.has(connKey(lb, bx, bz))) connMap.set(connKey(lb, bx, bz), []);
    connMap.get(connKey(la, ax, az)).push({ li: lb, x: bx, z: bz, cost });
    connMap.get(connKey(lb, bx, bz)).push({ li: la, x: ax, z: az, cost });
  }
  const DIRS = [[1,0,1],[-1,0,1],[0,1,1],[0,-1,1],[1,1,1.414],[-1,1,1.414],[1,-1,1.414],[-1,-1,1.414]];
  let iters = 0;
  const maxIters = layers.reduce((s, l) => s + l.w * l.h, 0) * 4;
  while (open.length > 0 && iters < maxIters) {
    iters++;
    const cur = open.pop();
    if (cur.li === endLayer && cur.x === ex && cur.z === ez) {
      // Reconstruct. Each waypoint uses its layer's Y so the path is a real 3D path.
      const path = [];
      let k = cur.li + ',' + cur.x + ',' + cur.z;
      let cx = cur.x, cz = cur.z, cl = cur.li;
      while (true) {
        path.unshift(new Vec3(cx + 0.5, layers[cl].y, cz + 0.5));
        if (cl === startLayer && cx === sx && cz === sz) break;
        const prev = cameFrom.get(k);
        if (!prev) break;
        k = prev.li + ',' + prev.x + ',' + prev.z;
        cx = prev.x; cz = prev.z; cl = prev.li;
      }
      return smoothPath(path);
    }
    const grid = layers[cur.li].grid, lw = layers[cur.li].w, lh = layers[cur.li].h;
    // 8-directional neighbours within the current layer.
    for (const [dx, dz, cost] of DIRS) {
      const nx = cur.x + dx, nz = cur.z + dz;
      if (nx < 0 || nx >= lw || nz < 0 || nz >= lh) continue;
      if (grid[nz * lw + nx] === 1) continue;
      if (dx !== 0 && dz !== 0) {
        if (grid[cur.z * lw + nx] === 1 || grid[nz * lw + cur.x] === 1) continue;
      }
      const tentativeG = cur.g + cost;
      const key = cur.li + ',' + nx + ',' + nz;
      if (gScore.has(key) && gScore.get(key) <= tentativeG) continue;
      gScore.set(key, tentativeG);
      cameFrom.set(key, { li: cur.li, x: cur.x, z: cur.z });
      open.push({ li: cur.li, x: nx, z: nz, g: tentativeG, f: tentativeG + h(nx, nz, cur.li) });
    }
    // Bridge edges via connections (cross-layer).
    const ck = connKey(cur.li, cur.x, cur.z);
    const bridges = connMap.get(ck);
    if (bridges) {
      for (const b of bridges) {
        const tentativeG = cur.g + b.cost;
        const key = b.li + ',' + b.x + ',' + b.z;
        if (gScore.has(key) && gScore.get(key) <= tentativeG) continue;
        gScore.set(key, tentativeG);
        cameFrom.set(key, { li: cur.li, x: cur.x, z: cur.z });
        open.push({ li: b.li, x: b.x, z: b.z, g: tentativeG, f: tentativeG + h(b.x, b.z, b.li) });
      }
    }
  }
  return null;
}

// v0.6.1: NavMesh grid-aware raycast via DDA (Digital Differential Analyzer) traversal.
// Steps cell-by-cell along the ray, returning the world-space point where the ray enters
// the first wall cell. The traversal is the classic Amanatides-Woo voxel traversal: at each
// step, advance to the next cell boundary on whichever axis is closer (tMaxX vs tMaxY), and
// check if the new cell is a wall.
//
// Returns:
//   - Vec3 hit point at the wall cell boundary, if the ray reaches a wall cell within maxDist
//   - null if the ray exits the grid, or reaches maxDist without hitting a wall
//   - null if the origin is inside a wall cell (degenerate — caller should clamp)
//
// v0.8.1: for multi-layer navs, the raycast walks each layer independently — at every step
// along the XZ-projected ray, it checks the wall state of the layer whose Y-band the ray is
// currently passing through. This makes ?raycast consider Y when testing hits against stairs,
// platforms, and bridges. For single-layer navs (no `layers` field or 1-element `layers`),
// the behavior is unchanged from v0.6.1.
function navRaycast(nav, origin, dir, maxDist) {
  if (!nav || !origin || !dir) return null;
  // 2D-in-3D: project onto XZ plane. dir may not be normalized — normalize for t accounting.
  const dx = dir.x, dz = dir.z;
  const dMag = Math.hypot(dx, dz);
  if (dMag < 1e-9) return null;
  const ndx = dx / dMag, ndz = dz / dMag;
  // v0.8.1: if multi-layer, dispatch to the layer-aware raycast. We pick the layer whose Y
  // is closest to the ray's CURRENT Y at each DDA step (the ray may climb/descend through
  // layers as t advances, since dir.y may be non-zero).
  const isMultiLayer = nav.layers && nav.layers.length > 1;
  // Single-layer fast path uses the original v0.6.1 DDA on nav.grid/w/h.
  const grid = isMultiLayer ? null : nav.grid;
  const w = isMultiLayer ? 0 : nav.w;
  const h = isMultiLayer ? 0 : nav.h;
  // Start cell.
  let cx = Math.floor(origin.x);
  let cz = Math.floor(origin.z);
  // If origin is outside the grid, ray misses entirely (we don't enter the grid).
  // For multi-layer, "outside the grid" means outside EVERY layer's bounds.
  if (isMultiLayer) {
    let inAny = false;
    for (const l of nav.layers) {
      if (cx >= 0 && cx < l.w && cz >= 0 && cz < l.h) { inAny = true; break; }
    }
    if (!inAny) return null;
  } else {
    if (cx < 0 || cx >= w || cz < 0 || cz >= h) return null;
  }
  // v0.8.1: helper to test the wall state of cell (cx, cz) at world Y = curY. For multi-layer,
  // picks the layer whose Y is closest to curY and tests that layer's cell.
  const testCell = (cx, cz, curY) => {
    if (isMultiLayer) {
      let bestLayer = null, bestDist = Infinity;
      for (const l of nav.layers) {
        if (cx < 0 || cx >= l.w || cz < 0 || cz >= l.h) continue;
        const d = Math.abs(l.y - curY);
        if (d < bestDist) { bestDist = d; bestLayer = l; }
      }
      if (!bestLayer) return 0; // no layer covers this cell at this Y → treat as walkable (ray passes)
      return bestLayer.grid[cz * bestLayer.w + cx];
    }
    if (cx < 0 || cx >= w || cz < 0 || cz >= h) return -1; // out of bounds (caller handles)
    return grid[cz * w + cx];
  };
  // If origin is inside a wall, the ray is blocked at the origin (return the origin).
  if (testCell(cx, cz, origin.y) === 1) return new Vec3(origin.x, origin.y, origin.z);
  // Step direction per axis: +1, -1, or 0.
  const stepX = ndx > 0 ? 1 : ndx < 0 ? -1 : 0;
  const stepZ = ndz > 0 ? 1 : ndz < 0 ? -1 : 0;
  // tMax: distance (in normalized t units, 0..maxDist) to the next cell boundary on each axis.
  // tDelta: distance per cell.
  let tMaxX, tMaxZ, tDeltaX, tDeltaZ;
  if (stepX !== 0) {
    const nextBoundary = stepX > 0 ? (cx + 1) : cx; // cell boundary at x = cx+1 (forward) or cx (backward)
    tMaxX = (nextBoundary - origin.x) / ndx; // signed; positive since we move forward
    tDeltaX = Math.abs(1 / ndx);
  } else { tMaxX = Infinity; tDeltaX = Infinity; }
  if (stepZ !== 0) {
    const nextBoundary = stepZ > 0 ? (cz + 1) : cz;
    tMaxZ = (nextBoundary - origin.z) / ndz;
    tDeltaZ = Math.abs(1 / ndz);
  } else { tMaxZ = Infinity; tDeltaZ = Infinity; }
  // Walk the grid until we hit a wall or exceed maxDist.
  let t = 0;
  const maxT = maxDist / dMag; // convert maxDist (world units) to t units (in normalized dir)
  // v0.8.1: max iters based on total cell count across layers (multi-layer) or w*h (single).
  const maxIters = isMultiLayer
    ? nav.layers.reduce((s, l) => s + l.w * l.h, 0) * 2
    : w * h * 2;
  for (let iter = 0; iter < maxIters; iter++) {
    if (tMaxX < tMaxZ) {
      cx += stepX;
      t = tMaxX;
      tMaxX += tDeltaX;
    } else {
      cz += stepZ;
      t = tMaxZ;
      tMaxZ += tDeltaZ;
    }
    if (t > maxT) return null; // exceeded max distance
    // Compute the current world Y at parameter t (ray may have a Y component for multi-layer).
    const worldT = t / dMag;
    const curY = origin.y + dir.y * worldT;
    if (isMultiLayer) {
      // Out-of-bounds check: if no layer covers (cx, cz), the ray has exited the navmesh.
      let inAny = false;
      for (const l of nav.layers) {
        if (cx >= 0 && cx < l.w && cz >= 0 && cz < l.h) { inAny = true; break; }
      }
      if (!inAny) return null;
    } else {
      if (cx < 0 || cx >= w || cz < 0 || cz >= h) return null; // exited grid
    }
    if (testCell(cx, cz, curY) === 1) {
      // Hit a wall. Compute the world-space hit point.
      return new Vec3(origin.x + dir.x * worldT, origin.y + dir.y * worldT, origin.z + dir.z * worldT);
    }
  }
  return null;
}

// ==========================================================================================
// SECTION 4: Native Subsystems (extended with Body3D, Camera, Audio3D)
// ==========================================================================================

const NATIVE_SUBSYSTEMS = {
  Fluid3D: {
    fields: ['resolution', 'bounds', 'viscosity', 'particles'],
    actions: ['fluid_step'],
    queries: [],
    sealed: true,
    actionImpl(name, args, entity) {
      if (name === 'fluid_step') {
        const poolName = [...entity.fields.keys()].find(k => entity.fields.get(k) instanceof Pool);
        void poolName;
        return;
      }
      throw new Error(`unknown '&Fluid3D' action '${name}'`);
    },
  },
  NavMesh3D: {
    fields: ['source', 'agent_radius'],
    actions: [],
    queries: ['path', 'raycast', 'block_cell', 'unblock_cell', 'is_blocked'],
    queryImpl(name, args, entity, world) {
      if (name === 'path') {
        const [from, to] = args;
        // v0.6: try real A* on the loaded grid; fall back to straight-line stub if no .nav file.
        const sourcePath = entity.fields.get('source');
        if (sourcePath) {
          const nav = loadNavGrid(world, sourcePath);
          if (nav) {
            const path = aStar(nav, from, to);
            if (path) return path;
          }
        }
        return [from, to];
      }
      if (name === 'raycast') {
        // v0.6.1: grid-aware raycast via DDA traversal. Returns the world-space hit point where
        // the ray enters a wall cell, or null if the ray exits the grid or reaches maxDist
        // without hitting anything. Syntax: `?raycast(origin, dir, maxDist)` — same shape as
        // the global `?>` operator, but the ray is sampled against the NavMesh grid (cell-by-cell)
        // instead of against entity colliders. This is useful for "can the AI see the player?"
        // checks where a wall between enemy and player should block line-of-sight.
        const [origin, dir, maxDistArg] = args;
        const sourcePath = entity.fields.get('source');
        const maxDist = (typeof maxDistArg === 'number') ? maxDistArg : 100;
        if (!sourcePath) return null;
        const nav = loadNavGrid(world, sourcePath);
        if (!nav) return null;
        return navRaycast(nav, origin, dir, maxDist);
      }
      // v0.8.2: dynamic obstacles. ?block_cell(x, z) temporarily marks a cell as a wall;
      // ?unblock_cell(x, z) restores it. ?is_blocked(x, z) returns true if the cell is currently
      // blocked (either permanently in the .nav file or temporarily via block_cell). The
      // overrides are stored on the nav object (not written back to disk) and persist until
      // unblocked or the world is reloaded. Useful for doors, destroyed bridges, collapsed floors.
      if (name === 'block_cell' || name === 'unblock_cell' || name === 'is_blocked') {
        const [xArg, zArg] = args;
        const sourcePath = entity.fields.get('source');
        if (!sourcePath) return name === 'is_blocked' ? false : null;
        const nav = loadNavGrid(world, sourcePath);
        if (!nav) return name === 'is_blocked' ? false : null;
        // v0.8.1 multi-layer: operate on the closest layer to the caller's Y (if multi-layer).
        // For single-layer navs, nav.layers is a 1-element array and nav.grid/w/h alias layer 0.
        let layer = nav.layers && nav.layers[0];
        if (nav.layers && nav.layers.length > 1 && xArg && typeof xArg.y === 'number') {
          // If the caller passed a Vec3, pick the closest layer by Y.
          let bestDist = Infinity;
          for (const l of nav.layers) {
            const d = Math.abs(l.y - xArg.y);
            if (d < bestDist) { bestDist = d; layer = l; }
          }
        }
        if (!layer) return name === 'is_blocked' ? false : null;
        // Accept either (x, z) as two numbers or a Vec3 (using .x and .z).
        let cx, cz;
        if (typeof xArg === 'number' && typeof zArg === 'number') { cx = xArg; cz = zArg; }
        else if (xArg && typeof xArg.x === 'number' && typeof xArg.z === 'number') { cx = xArg.x; cz = xArg.z; }
        else return name === 'is_blocked' ? false : null;
        cx = Math.floor(cx); cz = Math.floor(cz);
        if (cx < 0 || cx >= layer.w || cz < 0 || cz >= layer.h) return name === 'is_blocked' ? false : null;
        const idx = cz * layer.w + cx;
        if (name === 'block_cell') {
          layer.grid[idx] = 1;
          return null;
        }
        if (name === 'unblock_cell') {
          layer.grid[idx] = 0;
          return null;
        }
        if (name === 'is_blocked') {
          return layer.grid[idx] === 1;
        }
      }
      throw new Error(`unknown '&NavMesh3D' query '${name}'`);
    },
  },
  // v0.4: Body3D — physics body (stub; real implementation would be a native physics solver)
  Body3D: {
    fields: ['mass', 'drag'],
    actions: ['force', 'impulse', 'torque'],
    queries: [],
    actionImpl(name, args, entity) {
      if (name === 'force') {
        const f = args[0];
        const pose = entity.locals.get('pose');
        if (pose && f instanceof Vec3) {
          const mass = entity.fields.get('mass') || 1;
          pose.vel = pose.vel.add(f.divScalar(mass));
        }
        return;
      }
      if (name === 'impulse') {
        const imp = args[0];
        const pose = entity.locals.get('pose');
        if (pose && imp instanceof Vec3) pose.vel = pose.vel.add(imp);
        return;
      }
      if (name === 'torque') { return; } // stub
      throw new Error(`unknown '&Body3D' action '${name}'`);
    },
  },
  // v0.4: Camera — marks entity as the active camera
  // v0.8: ~rt: #Tex makes this an off-screen camera (renders to a texture instead of the
  // main display). ~rt_size: N sets the render-target resolution (default 128).
  // (Note: `~target: v3(...)` is the look-at target point, used by buildViewMatrix.)
  Camera: {
    fields: ['fov', 'near', 'far', 'target', 'rt', 'rt_size', 'up'],
    actions: [],
    queries: [],
    actionImpl() {},
  },
  // v0.7: Light — light source (directional or point). The rasterizer collects all &Light
  // entities at frame start and accumulates their contributions per pixel. Like &Camera, this
  // is a marker + config container — no actions, no queries, not sealed. The renderer reads
  // the fields directly (kind, dir, color, intensity, ambient, range). Point lights use the
  // entity's pose.pos as their world-space position.
  // v0.8: ~shadows: 1 enables shadow casting (renders a shadow map from this light's POV).
  // ~shadow_bias: 0.005 adjusts the depth comparison tolerance (default 0.005 — prevents acne).
  Light: {
    fields: ['kind', 'dir', 'color', 'intensity', 'ambient', 'range', 'shadows', 'shadow_bias'],
    actions: [],
    queries: [],
    actionImpl() {},
  },
  // v0.8.6: Environment — scene-wide rendering configuration. This is the LLM-first way to
  // control the look of the world: a single `~preset:horror` configures sky, sun, ambient,
  // fog, exposure, tonemapping, and shadow bias all at once. Optional override fields let
  // the LLM dial in a specific look without losing the preset's other settings.
  //
  // Recognized fields (all optional — the preset provides defaults):
  //   ~preset: day | night | dusk | horror | forest | desert | cave | interior | sci_fi
  //   ~sky: solid | gradient | procedural   (override sky mode)
  //   ~fog: none | light | heavy | exp | linear | height   (override fog mode/density)
  //   ~time: day | night | dusk | dawn       (override sun dir + sky + ambient)
  //   ~exposure: number                      (HDR exposure multiplier, default 1.0)
  //   ~sun_dir: v3                            (override sun direction, normalized)
  //   ~ambient: number                        (override ambient intensity)
  //   ~fog_color: 0xRRGGBB                    (override fog color)
  //   ~sun_color: 0xRRGGBB                    (override sun light color)
  //   ~sun_intensity: number                  (override sun intensity)
  //   ~shadow_bias: number                    (override shadow bias)
  //   ~tonemap: aces | reinhard | clamp       (override tonemapping curve)
  //
  // The renderer's resolveEnvironment() (in environment.js) reads these fields at frame start
  // and produces a fully-resolved env descriptor. If no &Environment entity exists, the
  // renderer uses the DEFAULT_ENV (a sensible "day"-like baseline) so the default scene looks
  // polished without any configuration.
  Environment: {
    fields: ['preset', 'sky', 'fog', 'time', 'exposure', 'sun_dir', 'ambient', 'fog_color', 'sun_color', 'sun_intensity', 'shadow_bias', 'tonemap'],
    actions: [],
    queries: [],
    actionImpl() {},
  },
  // v0.4: Audio3D — spatial audio emitter (stub)
  Audio3D: {
    fields: ['snd_r', 'snd_f'],
    actions: [],
    queries: [],
    actionImpl() {},
  },
  // v0.4: Listener — audio listener (stub)
  Listener: {
    fields: [],
    actions: [],
    queries: [],
    actionImpl() {},
  },
  // v0.4: Net — network replication (stub)
  Net: {
    fields: ['owner', 'sync'],
    actions: ['rpc'],
    queries: [],
    actionImpl(name, args, entity, world) {
      if (name === 'rpc') {
        const target = args[0];
        const method = args[1];
        const rpcArgs = args.slice(2);
        world.log.push({ type: 'rpc', target, method, args: rpcArgs });
        return;
      }
      throw new Error(`unknown '&Net' action '${name}'`);
    },
  },
  // v0.5: Base — minimal passthrough subsystem (no sealed-surface restrictions)
  Base: {
    fields: [],
    actions: [],
    queries: [],
    actionImpl() {},
  },
};


// ==========================================================================================
// SECTION 5: Channel Bus
// ==========================================================================================

class ChannelBus {
  constructor() { this.latest = new Map(); this.listeners = []; }
  publish(path, value, fromEntity) {
    this.latest.set(path, value);
    for (const l of this.listeners) l(path, value, fromEntity);
  }
  get(path) { return this.latest.get(path); }
}

// ==========================================================================================
// SECTION 6: Entity Instance
// ==========================================================================================

class EntityInstance {
  constructor(decl, world) {
    this.decl = decl;
    this.world = world;
    this.fields = new Map();
    this.locals = new Map();
    // Backward compat: auto-provide position/velocity/facing (Vec2 for old 2D code)
    this.locals.set('position', new Vec2(0, 0));
    this.locals.set('velocity', new Vec2(0, 0));
    this.locals.set('facing', new Vec2(0, -1));
    // v0.4: 3D pose (independent from legacy position/velocity for backward compat)
    const pose = new Transform();
    pose._entity = this;
    this.locals.set('pose', pose);
    // v0.4: tag name stored for spawning lookups
    this._tagName = decl.name;
    this._nosave = false;
    this._initFields();
  }
  _initFields() {
    for (const m of this.decl.members) {
      if (m.type !== 'FieldDecl') continue;
      if (m.sigil === '~') {
        if (m.value && (m.value.type === 'PoolType' || m.value.type === 'VecType' || m.value.type === 'MapType')) {
          if (m.value.type === 'PoolType') this.fields.set(m.name, new Pool(m.value.elementType, m.value.capacity));
          else if (m.value.type === 'VecType') this.fields.set(m.name, new BVec(m.value.elementType, m.value.capacity));
          else if (m.value.type === 'MapType') this.fields.set(m.name, new BMap(m.value.keyType, m.value.valType, m.value.capacity));
        } else if (m.value && m.value.type === 'Call' && (m.value.callee === 'sphere' || m.value.callee === 'box' || m.value.callee === 'capsule' || m.value.callee === 'mesh')) {
          // v0.4: collider shape declaration ~hit: sphere(r), box(v3), mesh(#M)
          // v0.6 fix: evaluate the args (was storing raw AST nodes, so the collision system
          // couldn't read numeric values). The intrinsic `sphere(r)` already returns a
          // ColliderShape; here we mirror that and pass evaluated values.
          const evaluatedArgs = m.value.args.map(a => evalExpr(a.value, this.rootCtx()));
          this.fields.set(m.name, new ColliderShape(m.value.callee, evaluatedArgs));
        } else {
          const v = m.value ? evalExpr(m.value, this.rootCtx()) : null;
          // v0.4: timer fields — values with unit 's' or 'ms' get special tracking
          if (m.value && m.value.unit && (m.value.unit === 's' || m.value.unit === 'ms')) {
            this.fields.set(m.name, makeTimer(v, m.value.unit));
          } else {
            this.fields.set(m.name, v);
          }
        }
      } else if (m.sigil === '$') {
        const shape = { callee: m.dist.shape.callee, args: m.dist.shape.args.map(a => evalExpr(a.value, this.rootCtx())) };
        const prior = m.dist.prior ? { callee: m.dist.prior.callee, args: m.dist.prior.args.map(a => evalExpr(a.value, this.rootCtx())) } : null;
        const infer = m.infer ? { callee: m.infer.callee, args: m.infer.args.map(a => evalExpr(a.value, this.rootCtx())) } : null;
        this.fields.set(m.name, new Distribution(shape, prior, infer, this.world && this.world.intrinsics.random));
      }
    }
    // v0.4: check for ~nosave flag (a field with no value)
    for (const m of this.decl.members) {
      if (m.type === 'FieldDecl' && m.sigil === '~' && m.name === 'nosave' && !m.value) {
        this._nosave = true;
      }
    }
  }
  rootCtx() { return { entity: this, world: this.world, dt: 0, scope: this.world ? new Scope(this.world.globalScope) : null, inFn: false, hot: false }; }
  get(name) {
    if (name === 'pose') return this.locals.get('pose');
    if (name === 'wpose') {
      const p = this.locals.get('pose');
      return p ? p.worldPose(this.world) : new Transform();
    }
    if (this.fields.has(name)) return this.fields.get(name);
    if (this.locals.has(name)) return this.locals.get(name);
    // v0.5: shorthand #Entity.pos -> pose.pos, .vel -> pose.vel, .wpose -> worldPose.
    // v0.8.4 (Bug #8 fix): added .scl -> pose.scl for symmetry with the setter (it was
    // previously only routable when reading `pose.scl` directly — `#Tag.scl` returned
    // undefined because the EntityInstance branch below only listed pos/vel/rot/wpose).
    const pose = this.locals.get('pose');
    if (pose) {
      if (name === 'pos') return pose.pos;
      if (name === 'vel') return pose.vel;
      if (name === 'rot') return pose.rot;
      if (name === 'scl') return pose.scl;
      if (name === 'wpose') return pose.worldPose(this.world);
    }
    return undefined;
  }
  set(name, value) {
    // v0.8.4 (Bug #8 fix): cross-entity (and same-entity shorthand) writes to `pos` / `rot`
    // / `scl` / `vel` must route into the entity's `pose` Transform object, not into a dead
    // local. Before this fix, `#A.pos = v3(9,9,9)` landed in `A.locals.get('pos')` — a key
    // nothing ever reads — while the real `pose.pos` stayed at its prior value. The getter
    // already had a special case routing reads through to `pose.{pos,vel,rot}` (v0.5); the
    // setter did not, which is why this was a silent no-op instead of a syntax error.
    //
    // These names are reserved: they always refer to the live Transform, never to a field
    // or local of the same name. If a user really wants a field called `pos`, they must
    // declare it explicitly (`~pos: v3(0,0,0)`) — the field check below takes precedence
    // over this reserved-name routing. (In practice nobody does this; the shorthand is
    // universal in the existing corpus and the spec.)
    if (!this.fields.has(name)) {
      const pose = this.locals.get('pose');
      if (pose && (name === 'pos' || name === 'rot' || name === 'scl' || name === 'vel')) {
        pose[name] = value;
        return;
      }
    }
    if (this.fields.has(name)) {
      // v0.4: timer reassignment resets the timer
      const existing = this.fields.get(name);
      if (existing && existing.__timer) {
        this.fields.set(name, makeTimer(value, existing.unit));
        return;
      }
      // v0.5.1: if assigning a timer to a non-timer field, CLONE the timer so the
      // target field is independent of the source (`cd = shoot_cd` would otherwise
      // share the same object, so decrementing one decrements the other).
      if (value && typeof value === 'object' && value.__timer) {
        this.fields.set(name, makeTimer(value, value.unit));
        return;
      }
      this.fields.set(name, value);
      return;
    }
    this.locals.set(name, value);
  }
  blocksNamed(name) { return this.decl.members.filter(m => m.type === 'Block' && m.name === name); }
}

// ==========================================================================================
// SECTION 7: World
// ==========================================================================================

class World {
  constructor() {
    this.entities = [];
    this.tags = new Map();
    this.resources = new Map();
    this.eventSchemas = new Map();
    this.typeDecls = new Map();
    this.channels = new ChannelBus();
    this.input = { move: new Vec2(0, 0), jump: false, fire: false, aim: new Vec2(0, 0) };
    // v0.9.0: the general-purpose standard library is merged over the engine intrinsics. It
    // is bound to this World so its I/O respects the sandbox settings, and it receives a small
    // runtime bridge so its higher-order functions can invoke AxiomScript lambdas.
    this.intrinsics = Object.assign(defaultIntrinsics(), stdlibIntrinsics(this, {
      callValue, truthy, equalsVal, stringify: stringifyFStringVal, AxiomError, Atom, Vec3, Vec2,
    }));
    this._tickAcc = new Map();
    this._physicsAcc = 0;
    this.physicsHz = 60;
    this._pendingSKernelEvents = [];
    this.log = [];
    this.runtimeDiagnostics = [];
    this.sourceText = null;
    // v0.4 additions
    this.fns = new Map();      // name -> FnDecl
    this.procs = new Map();    // name -> ProcDecl
    this.mixins = new Map();   // name -> MixinDecl
    this.materials = new Map();// name -> MaterialDecl
    this.entityDecls = new Map(); // name -> EntityDecl (for runtime spawning)
    this.tweens = [];         // active Tween objects
    this.drawList = [];        // per-frame draw commands, cleared each render step
    this.hudElements = [];    // HUD element descriptors
    this.saveSlots = new Map();// slot name -> serialized state
    this._simTime = 0;        // cumulative simulation time
    this.gravity = new Vec3(0, -9.8, 0); // world default gravity
    // v0.8.7: sandbox mode — restricts unsafe operations for public/release deployments.
    //   - sandbox: true → !play stubs (no subprocess), !save/!load use in-memory Map only (no fs),
    //              ^fn FFI is blocked (fatal runtime error if used — see AX-SANDBOX-001 below).
    //   - allowNet: true → re-enables network access (no-op in this codebase since there's no
    //              network code; documented for forward compat).
    //   - allowReadPaths: Set of paths that fs.readFileSync is permitted to access. Empty by
    //              default — sandbox mode allows NO fs reads. Add paths via --allow-read <path>.
    //   The sandbox flag is set via World.setSandbox(opts) from main.js when --sandbox is passed.
    this.sandbox = false;
    this.allowNet = false;
    this.allowReadPaths = new Set();
    // v0.9.0: sandbox gates for the general-purpose stdlib. Reads were already gated; writes
    // and subprocess execution are new capabilities and default to OFF in sandbox mode.
    this.allowWritePaths = new Set();
    this.allowExec = false;
    // v0.9.0: the root lexical scope. Globals assigned at the top of ^main, and anything a
    // ^fn declares with `~name: value` at its outermost level, live here.
    this.globalScope = new Scope(null, true);
    this._callDepth = 0;
    this._closureCache = new WeakMap();
    // v0.9.0: `^main:` entry point (set by loadProgram) and the value it returned.
    this.mainDecl = null;
    this.mainResult = null;
    this.exitCode = 0;
    // v0.9.0: argv visible to the program through `args()`.
    this.argv = [];
  }

  // v0.9.0: wrap a declared ^fn/^proc as a first-class value so it can be passed to .map,
  // stored in a dict, or piped into. Cached per declaration so `f == f` holds.
  closureFor(decl) {
    let c = this._closureCache.get(decl);
    if (!c) {
      c = new Closure({ params: decl.params, defaults: decl.defaults, body: decl.body, isExpr: false, scope: this.globalScope, entity: null, name: decl.name });
      this._closureCache.set(decl, c);
    }
    return c;
  }

  // v0.9.0: the root context for entity-free execution — scripts, ^main, and any ^fn called
  // from the host. `entity: null` is now a supported state everywhere.
  scriptCtx() {
    return { entity: null, world: this, dt: 0, scope: new Scope(this.globalScope), blockName: null, eventPayload: null, inFn: true, hot: false };
  }

  // v0.9.0: run `^main`. Returns the value ^main returned (used as the process exit code when
  // it is a number). Faults are reported the same way block faults are, and also rethrown to
  // the caller so a CLI can exit non-zero.
  runMain(argv) {
    if (!this.mainDecl) return null;
    this.argv = argv || [];
    const decl = this.mainDecl;
    const ctx = this.scriptCtx();
    const scope = new Scope(this.globalScope, true);
    if (decl.params.length) scope.declare(decl.params[0], this.argv.slice());
    scope.declare('args', this.argv.slice());
    const mainCtx = { ...ctx, scope };
    try {
      for (const stmt of decl.body) {
        const r = execStmtInner(stmt, mainCtx);
        if (r instanceof ReturnSignal) { this.mainResult = r.value; break; }
      }
    } catch (err) {
      if (err instanceof ReturnSignal) { this.mainResult = err.value; }
      else {
        const v = errorToValue(err);
        this.runtimeDiagnostics.push(makeRuntimeFault(null, { name: 'main' }, { line: decl.line, col: decl.col }, err, this));
        this.exitCode = 1;
        throw err;
      }
    }
    if (typeof this.mainResult === 'number') this.exitCode = this.mainResult | 0;
    return this.mainResult;
  }

  // v0.9.0: true when the program is a script (has ^main) rather than a simulation. A program
  // can be both — ^main runs first, then the frame loop, which is how a game does setup.
  get isScript() { return !!this.mainDecl; }

  // v0.8.7: enable sandbox mode. Call from main.js when --sandbox is passed.
  // Options: { allowNet: bool, allowReadPaths: string[] }
  setSandbox(opts = {}) {
    const nodePath = require('path');
    this.sandbox = true;
    this.allowNet = !!opts.allowNet;
    // Paths are stored both as written and resolved, so `--allow-read ./data` matches a read
    // of the same file spelled as an absolute path (and vice versa).
    if (Array.isArray(opts.allowReadPaths)) {
      for (const p of opts.allowReadPaths) { this.allowReadPaths.add(p); this.allowReadPaths.add(nodePath.resolve(p)); }
    }
    // v0.9.0: the standard library can write files and run commands, so both need gates.
    if (Array.isArray(opts.allowWritePaths)) {
      for (const p of opts.allowWritePaths) { this.allowWritePaths.add(p); this.allowWritePaths.add(nodePath.resolve(p)); }
    }
    this.allowExec = !!opts.allowExec;
  }

  // v0.8.7: check whether a given file path is readable in the current sandbox configuration.
  // Returns true if sandbox is off, or if the path is in the allowReadPaths set, or if the path
  // is a prefix of an allowed path (subdirectory access). Used by !save/!load and resource loaders.
  isPathReadable(absPath) {
    if (!this.sandbox) return true;
    for (const allowed of this.allowReadPaths) {
      if (absPath === allowed || absPath.startsWith(allowed + '/') || absPath.startsWith(allowed + '\\')) {
        return true;
      }
    }
    return false;
  }
  loadProgram(program, sourceText) {
    if (sourceText !== undefined) this.sourceText = sourceText;
    // v0.8.1: store the program reference so save/load schema helpers can read the version
    // pragma (axiomVersion) for the save fingerprint.
    this._program = program;
    // v0.6: auto-generate procedural mesh geometry for `#Mesh3D Name: "sphere.glb"` (or
    // box/cube/plane/ground) so the software rasterizer has something to draw. The mesh data
    // is stored on the resource descriptor under `vertices`/`indices` (Float32Array/Uint16Array).
    // The renderer reads these directly; if missing, the entity renders as nothing.
    // v0.6: also load `#Texture Name: "path.png"` into a {__texture, width, height, data} descriptor
    // via the render3d PNG decoder; if the path is one of the procedural names ("checker", "stripe",
    // "noise", "grid"), use the procedural generator instead. If loading fails, the entity just
    // renders flat-shaded — the runtime never throws on texture load failure.
    // v0.9.0: the renderer is loaded lazily and optionally. A program that declares no visual
    // resources — a CLI tool, a solver, a test — must not need a rasterizer to be present at
    // all, and a missing render3d.js is now a warning on the resources that needed it rather
    // than a crash on every program.
    const renderer = (program.resources && program.resources.length) ? loadRendererModule(this) : null;
    const proceduralMeshForPath = renderer ? renderer.proceduralMeshForPath : () => null;
    const loadTexture = renderer ? renderer.loadTexture : () => null;
    const proceduralTexture = renderer ? renderer.proceduralTexture : () => null;
    for (const r of program.resources || []) {
      const res = { kind: r.kind, path: r.path, name: r.name };
      if (r.kind === 'Texture') {
        // Try procedural name first (cheaper, no fs).
        const proc = proceduralTexture(r.path);
        if (proc) { Object.assign(res, proc); }
        else {
          // Try loading as a PNG file. Best-effort: silent on failure.
          const tex = loadTexture(r.path);
          if (tex) Object.assign(res, tex);
        }
        this.resources.set(r.name, res);
        continue;
      }
      // v0.8.4: inline .glb support. If `r.inlineB64` is set, the mesh bytes are embedded
      // directly in the .ax source as a base64 string (single-line `base64("...")` or
      // multi-line `glb:` heredoc — see parser.js parseResourceDecl). Decode and call
      // parseGLBMulti(buf) directly — no fs.existsSync / readFileSync, no path lookup, and
      // NO procedural-primitive fallback. A corrupt inline blob raises AX-MESH-001 at compile
      // time (checker.js checkInlineMeshes), so by the time we get here the payload is known
      // good. If somehow we still get null at runtime, fall back to a unit box (same as the
      // path-based form's "file exists but parse failed" branch) so the entity is at least
      // visible — but this should never happen in practice.
      if (r.kind === 'Mesh3D' && r.inlineB64) {
        let prims = null;
        try {
          const buf = Buffer.from(r.inlineB64, 'base64');
          prims = parseGLBMulti(buf);
        } catch (e) { prims = null; }
        if (prims && prims.length > 0) {
          res.vertices = prims[0].vertices; res.indices = prims[0].indices;
          res.glb = true; res.inline = true;
          if (prims[0].skin) res.skin = prims[0].skin;
          if (prims[0].targets) res.targets = prims[0].targets;
          if (prims[0].animations) res.animations = prims[0].animations;
          if (prims[0].material != null) res.material = prims[0].material;
          // Register every primitive under X_0, X_1, ... — same multi-prim convention as the
          // path-based loader (a single .glb can hold model + collision proxy).
          for (let pi = 0; pi < prims.length; pi++) {
            const primRes = {
              kind: r.kind, path: null, name: `${r.name}_${pi}`,
              vertices: prims[pi].vertices, indices: prims[pi].indices, glb: true, inline: true,
            };
            if (prims[pi].skin) primRes.skin = prims[pi].skin;
            if (prims[pi].targets) primRes.targets = prims[pi].targets;
            if (prims[pi].animations) primRes.animations = prims[pi].animations;
            if (prims[pi].material != null) primRes.material = prims[pi].material;
            this.resources.set(`${r.name}_${pi}`, primRes);
          }
        } else {
          // Defensive: should never happen (checker validates at compile time), but if a
          // corrupt blob slips through, fall back to a unit box so the entity is visible.
          // v0.9.2: without a renderer there is no procedural box to fall back to; the
          // resource stays a plain descriptor instead of crashing the load.
          const box = proceduralMeshForPath('box.glb');
          if (box) { res.vertices = box.vertices; res.indices = box.indices; }
        }
        this.resources.set(r.name, res);
        continue;
      }
      const proc = proceduralMeshForPath(r.path);
      if (proc) { res.vertices = proc.vertices; res.indices = proc.indices; }
      // v0.6: for non-procedural Mesh3D paths (e.g. "hero.glb"), try the real .glb loader
      // (see loadGLB below). If that fails, fall back to a unit box so the entity is at least
      // visible.
      // v0.7: multi-primitive .glb files register the first primitive as `X` (backward compat
      // with `!mesh(#X)`) AND every primitive as `X_0`, `X_1`, ... — so a single .glb can hold
      // a model + collision proxy, addressable as `#X_0` and `#X_1` separately.
      else if (r.kind === 'Mesh3D') {
        const prims = loadGLBMulti(r.path);
        if (prims && prims.length > 0) {
          res.vertices = prims[0].vertices; res.indices = prims[0].indices; res.glb = true;
          // v0.8.1: carry over skin, morph targets, and animation clips so !play_anim and
          // renderScene's skinning pass can find them on the resolved resource.
          if (prims[0].skin) res.skin = prims[0].skin;
          if (prims[0].targets) res.targets = prims[0].targets;
          if (prims[0].animations) res.animations = prims[0].animations;
          if (prims[0].material != null) res.material = prims[0].material;
          // Register every primitive under X_0, X_1, ... — the first one is also X (above).
          for (let pi = 0; pi < prims.length; pi++) {
            const primRes = {
              kind: r.kind, path: r.path, name: `${r.name}_${pi}`,
              vertices: prims[pi].vertices, indices: prims[pi].indices, glb: true,
            };
            if (prims[pi].skin) primRes.skin = prims[pi].skin;
            if (prims[pi].targets) primRes.targets = prims[pi].targets;
            if (prims[pi].animations) primRes.animations = prims[pi].animations;
            if (prims[pi].material != null) primRes.material = prims[pi].material;
            this.resources.set(`${r.name}_${pi}`, primRes);
          }
        } else {
          // v0.9.2: without a renderer there is no procedural box to fall back to; the
          // resource stays a plain descriptor instead of crashing the load.
          const box = proceduralMeshForPath('box.glb');
          if (box) { res.vertices = box.vertices; res.indices = box.indices; }
        }
      }
      this.resources.set(r.name, res);
    }
    for (const ev of program.events || []) this.eventSchemas.set(ev.name, ev);
    for (const t of program.types || []) this.typeDecls.set(t.name, t);
    for (const fn of program.fns || []) this.fns.set(fn.name, fn);
    for (const p of program.procs || []) this.procs.set(p.name, p);
    for (const m of program.mixins || []) this.mixins.set(m.name, m);
    for (const mat of program.materials || []) this.materials.set(mat.name, mat);
    // v0.9.0: program globals (`~NAME: expr` at top level) are evaluated once, in declaration
    // order, into the root scope — so a global may be defined in terms of an earlier one.
    for (const g of program.globals || []) {
      const ctx = this.scriptCtx();
      let v = null;
      try { v = evalExpr(g.value, ctx); }
      catch (err) {
        this.runtimeDiagnostics.push(makeRuntimeFault(null, { name: 'global' }, g, err, this));
      }
      for (const n of g.names) this.globalScope.declare(n, v);
    }
    // v0.9.0: the ^main entry point, if the program declares one.
    if (program.main) this.mainDecl = program.main;
    for (const e of program.entities) {
      // v0.4: compose mixin members into entity before instantiation
      const composedMembers = this._composeMixins(e);
      const composedDecl = { ...e, members: composedMembers };
      this.entityDecls.set(e.name, composedDecl);
      this.addEntity(composedDecl);
    }
    // v0.4: apply initial poses from 'at' syntax
    for (const e of program.entities) {
      if (e.initPose) {
        const inst = this.tags.get(e.name);
        if (inst) {
          const pose = inst.locals.get('pose');
          const val = evalExpr(e.initPose, inst.rootCtx());
          if (val instanceof Vec3) pose.pos = val;
          else if (val instanceof Transform) { pose.pos = val.pos; pose.rot = val.rot; pose.scl = val.scl; }
          // Also update legacy position for backward compat
          if (val instanceof Vec3) inst.locals.set('position', new Vec2(val.x, val.y));
        }
      }
    }
    return this;
  }
  _composeMixins(entityDecl) {
    let members = [...entityDecl.members];
    for (const mixinName of entityDecl.mixins || []) {
      const mixin = this.mixins.get(mixinName);
      if (!mixin) continue;
      // Prepend mixin members (entity's own shadow them)
      const existingNames = new Set(members.filter(m => m.type === 'FieldDecl').map(m => m.name));
      const newMembers = [];
      for (const m of mixin.members) {
        if (m.type === 'FieldDecl' && existingNames.has(m.name)) continue; // entity shadows
        newMembers.push(m);
      }
      members = [...newMembers, ...members];
    }
    return members;
  }
  addEntity(decl) {
    const inst = new EntityInstance(decl, this);
    this.entities.push(inst);
    this.tags.set(decl.name, inst);
    return inst;
  }
  // v0.8.2: remove an entity from the world. The entity stops receiving ticks/physics/render
  // passes immediately. Its tag is also removed from the tags map (so #Tag lookups return
  // undefined). If the entity is referenced by other entities' fields (e.g. a cached #Tag
  // reference), those references become stale — the caller is responsible for not using them
  // after despawn. Returns true if the entity was found and removed, false otherwise.
  removeEntity(inst) {
    if (!inst) return false;
    // v0.8.10 fix: mark for deferred removal instead of splicing immediately. Splicing during
    // for...of iteration in stepPhysics/deliverBroadcast causes the next entity to be skipped
    // (the splice shifts it down, but the iterator has already advanced past its old index).
    inst._pendingRemove = true;
    return true;
  }
  // v0.8.10: resolveTag skips entities marked for removal (_pendingRemove). This ensures
  // !despawn(self) immediately makes the entity invisible to #Tag lookups and ?exists queries
  // within the same frame, even though the actual array splice is deferred to end-of-update.
  resolveTag(name) {
    const inst = this.tags.get(name);
    if (inst && inst._pendingRemove) return undefined;
    return inst;
  }
  deliverBroadcast(eventName, payload, fromEntity, addressing) {
    for (const e of this.entities) {
      if (e._pendingRemove) continue; // v0.8.13: skip despawned in broadcast delivery
      if (addressing.mode === 'to') {
        const target = this.resolveTag(addressing.target);
        if (e !== target) continue;
      } else if (addressing.mode === 'within') {
        const pos = (e.locals.get('pose') || new Transform()).pos;
        const d = Math.hypot(pos.x - addressing.origin.x, pos.y - addressing.origin.y, pos.z - addressing.origin.z);
        if (d > addressing.radius) continue;
      }
      for (const block of e.blocksNamed('on')) {
        if (block.eventArg !== eventName) continue;
        runBlock(block, e, this, 1 / this.physicsHz, payload);
      }
    }
  }
  stepPhysics(dt) {
    const pending = this._pendingSKernelEvents;
    this._pendingSKernelEvents = [];
    for (const ev of pending) this.deliverBroadcast(ev.name, ev.payload, ev.fromEntity, ev.addressing);
    for (const e of this.entities) {
      if (e._pendingRemove) continue; // v0.8.13: skip despawned in physics
      // v0.4: tick timers
      for (const [k, v] of e.fields) {
        if (v && v.__timer) {
          const step = v.unit === 'ms' ? dt * 1000 : dt;
          v.remaining = Math.max(0, v.remaining - step);
        }
      }
      // v0.4: apply gravity to &Body3D entities
      if (e.decl.base === 'Body3D') {
        const pose = e.locals.get('pose');
        if (pose) {
          // v0.5.1: auto-promote Vec2 vel → Vec3(v.x, 0, v.y) so input.move-style 2D vectors
          // (x=right, y=forward) integrate correctly with 3D gravity (y=down). Without this,
          // `pose.vel = input.move.norm * speed` makes pose.vel a Vec2, and Vec3.add(Vec2)
          // produces NaN z on the next gravity tick.
          if (pose.vel instanceof Vec2) pose.vel = new Vec3(pose.vel.x, 0, pose.vel.y);
          const mass = e.fields.get('mass') || 1;
          const drag = e.fields.get('drag') || 0;
          const grav = this.gravity.mulScalar(mass);
          const dragForce = pose.vel.mulScalar(-drag);
          pose.vel = pose.vel.add(grav.add(dragForce).divScalar(mass).mulScalar(dt));
          pose.pos = pose.pos.add(pose.vel.mulScalar(dt));
        }
      }
      for (const block of e.blocksNamed('physics')) {
        runBlock(block, e, this, dt);
      }
    }
    // v0.6: ground plane clamp — any Body3D that fell below y=0 snaps up. Runs AFTER gravity
    // integration but BEFORE collision resolution so colliders see the corrected y.
    stepGroundPlane(this);
    // v0.6: pairwise collision resolution — runs after every entity's physics block (so the
    // resolver sees final per-entity positions, not mid-step ones). Pure JS, O(n²), no native.
    stepCollisions(this);
    // v0.8.10: sweep deferred entity removals after collision resolution (same as in update()).
    // This ensures despawned entities are removed from the entities array within the same
    // stepPhysics call, so tests that call stepPhysics directly see the removal.
    if (this.entities.some(e => e._pendingRemove)) {
      this.entities = this.entities.filter(e => {
        if (!e._pendingRemove) return true;
        const name = e._tagName || e.decl.name;
        if (this.tags.get(name) === e) this.tags.delete(name);
        return false;
      });
    }
    // v0.4: tick tweens
    this.tweens = this.tweens.filter(tw => { tw.tick(dt); return !tw.done; });
    this._simTime += dt;
  }
  stepRender(dt) {
    this.drawList = []; // clear per frame
    this.hudElements = [];
    for (const e of this.entities) {
      if (e._pendingRemove) continue; // v0.8.12: skip despawned entities in render
      // v0.4: collect HUD elements from ~hud fields (v0.5.1: wrap under `hud:` for clean access)
      for (const [name, val] of e.fields) {
        if (name === 'hud' && val && val.__hud) this.hudElements.push({ entity: e, hud: val });
      }
      for (const block of e.blocksNamed('render')) {
        runBlock(block, e, this, dt);
      }
      // v0.8.1: advance any active animation clip on this entity. This runs AFTER the &render
      // block so that !mesh has already recorded _skinningMesh on the entity (needed for the
      // manual-joints path). The sampled TRS is cached as `_jointMatrices` so renderScene
      // (called next, by rasterizeFrame) can pick it up.
      const hasManual = e.fields.get('joint_pos') || e.fields.get('joint_rot') || e.fields.get('joint_scl');
      if (e._activeAnim) {
        e._activeAnim.time += dt;
        const sampled = sampleAnimation(e._activeAnim.clip, e._activeAnim.time, e._activeAnim.loop);
        e._jointMatrices = buildJointMatrices(e._activeAnim.meshRef, sampled, e);
      } else if (hasManual && e._skinningMesh) {
        // No anim, but manual joint arrays are set AND we know which mesh has skinning data
        // (recorded by !mesh when the mesh has a `skin` field). Build joint matrices from the
        // manual arrays so the user can drive skinning without !play_anim.
        e._jointMatrices = buildJointMatrices(e._skinningMesh, null, e);
      } else if (!hasManual && !e._activeAnim) {
        // Neither anim nor manual joints — clear cached matrices so the entity returns to rest.
        e._jointMatrices = null;
      }
    }
  }
  stepCognition(realDt) {
    for (const e of this.entities) {
      if (e._pendingRemove) continue; // v0.8.13: skip despawned in cognition
      for (const block of e.blocksNamed('tick')) {
        const hz = block.freq || 10;
        const key = e.decl.name + '::' + block.name + block.freq;
        const acc = (this._tickAcc.get(key) || 0) + realDt;
        const period = 1 / hz;
        let a = acc;
        let ran = 0;
        while (a >= period && ran < 8) { a -= period; runBlock(block, e, this, period); ran++; }
        this._tickAcc.set(key, a);
      }
    }
  }
  update(realDt) {
    this._physicsAcc += realDt;
    const period = 1 / this.physicsHz;
    let ran = 0;
    while (this._physicsAcc >= period && ran < 8) { this._physicsAcc -= period; this.stepPhysics(period); ran++; }
    this.stepCognition(realDt);
    this.stepRender(realDt);
    // v0.8.7: flush the !d debug ring buffer to world.log now that the frame is complete.
    // This keeps the hot path (&physics/&render) allocation-free during the frame — debug
    // entries are batched into a fixed-size ring buffer and only materialized as log entries
    // once per frame, after all blocks have run.
    this._flushDebugRing();
    // v0.8.10: sweep deferred entity removals (!despawn). We do this after all physics/tick/render
    // blocks have run so no entity is skipped during iteration (the old splice-during-iter bug).
    if (this.entities.some(e => e._pendingRemove)) {
      this.entities = this.entities.filter(e => {
        if (!e._pendingRemove) return true;
        const name = e._tagName || e.decl.name;
        if (this.tags.get(name) === e) this.tags.delete(name);
        return false;
      });
    }
  }

  // v0.8.7: drain the !d ring buffer into world.log. Each entry becomes a 'debug' log entry
  // with the entity, block, args (stringified via the same rules as f-string interpolation),
  // and the sim time. The ring is cleared after flushing. Called once per frame from update().
  _flushDebugRing() {
    if (!this._debugRing || this._debugRing.count === 0) return;
    const ring = this._debugRing;
    const start = (ring.head - ring.count + ring.buf.length) % ring.buf.length;
    for (let i = 0; i < ring.count; i++) {
      const idx = (start + i) % ring.buf.length;
      const e = ring.buf[idx];
      if (!e) continue;
      // Stringify args the same way f-string interpolation does, for consistency.
      const msg = e.args.map(a => {
        if (typeof a === 'string') return a;
        return stringifyFStringVal(a);
      }).join(' ');
      this.log.push({ type: 'debug', msg, entity: e.entity, block: e.block, t: e.t });
    }
    ring.head = 0;
    ring.count = 0;
  }
}

// ==========================================================================================
// SECTION 8: Intrinsics
// ==========================================================================================

// v0.9.0: optional renderer. AxiomScript's 3D backend is a peer dependency of the *language*,
// not a prerequisite for running a program. Programs that declare visual resources load it on
// demand; everything else never touches it.
let _rendererModule; // undefined = not tried, null = unavailable
function loadRendererModule(world) {
  if (_rendererModule !== undefined) return _rendererModule;
  try {
    _rendererModule = require('./render3d');
  } catch (e) {
    _rendererModule = null;
    if (world) {
      world.runtimeDiagnostics.push({
        error_code: 'AX-RENDER-000', severity: 'advisory',
        message_for_human: `renderer module (render3d.js) is unavailable — visual resources will not load: ${e.message}`,
        message_for_agent: `This program declares #Mesh3D/#Texture resources, but render3d.js could not be loaded. Non-visual execution (^main, ^fn, physics) is unaffected.`,
        location: { line: null, col: null },
      });
    }
  }
  return _rendererModule;
}

function defaultIntrinsics() {
  return {
    // v0.8.9: v2 with 1 arg = uniform Vec2(v, v).
    v2: (x, y) => y === undefined ? new Vec2(x, x) : new Vec2(x, y),
    // v0.8.8: v3 with 2 args = horizontal plane vec (x, 0, z). Saves 3 tokens vs v3(x,0,z).
    // 1-arg form is allowed (treats as uniform v3(v,v,v) — but `3v` literal is preferred for that).
    // 3-arg form unchanged.
    v3: (x, y, z) => {
      if (z === undefined) {
        if (y === undefined) return new Vec3(x, x, x);  // 1-arg: uniform
        return new Vec3(x, 0, y);  // 2-arg: (x, 0, z) — horizontal plane
      }
      return new Vec3(x, y, z);  // 3-arg: full
    },
    // v0.8.8: Vec3 axis-aligned shorthands. Common in game code (e.g. gravity = v3y(-9.8)).
    v3x: (v) => new Vec3(v, 0, 0),
    v3y: (v) => new Vec3(0, v, 0),
    v3z: (v) => new Vec3(0, 0, v),
    v3xz: (x, z) => new Vec3(x, 0, z),
    // v0.8.10: 2D direction from angle — v2dir(0) = v2(1,0), v2dir(PI/2) = v2(0,1).
    v2dir: (a) => new Vec2(Math.cos(a), Math.sin(a)),
    q: (axis, angle) => Quat.fromAxisAngle(axis, angle),
    euler: (p, y, r) => Quat.fromEuler(p, y, r),
    m4: () => Mat4.identity(),
    persp: (fov, aspect, n, f) => Mat4.perspective(fov, aspect, n, f),
    ortho: (l, r, b, t, n, f) => Mat4.ortho(l, r, b, t, n, f),
    lookat: (eye, tgt, up) => Mat4.lookAt(eye, tgt, up || new Vec3(0, 1, 0)),
    aabb: (min, max) => ({ __aabb: true, min, max }),
    clamp: (x, lo, hi) => clampNum(x, lo, hi),
    dist: (a, b) => (typeof a === 'number' && typeof b === 'number') ? Math.abs(a - b) : a.sub(b).mag,
    sphere: (r) => new ColliderShape('sphere', [r]),
    box: (v) => new ColliderShape('box', [v]),
    capsule: (r, h) => new ColliderShape('capsule', [r, h]),
    // v0.8.8: math intrinsics. Each wraps Math.* — saves 3-6 tokens per call vs inline workarounds.
    // Single-arg:
    abs: (x) => Math.abs(x),
    floor: (x) => Math.floor(x),
    ceil: (x) => Math.ceil(x),
    round: (x) => Math.round(x),
    sqrt: (x) => Math.sqrt(x),
    sin: (x) => Math.sin(x),
    cos: (x) => Math.cos(x),
    tan: (x) => Math.tan(x),
    asin: (x) => Math.asin(x),
    acos: (x) => Math.acos(x),
    atan: (x) => Math.atan(x),
    log: (x) => Math.log(x),
    log10: (x) => Math.log10(x),
    log2: (x) => Math.log2(x),
    exp: (x) => Math.exp(x),
    sign: (x) => Math.sign(x),
    // Two-arg:
    // v0.8.10: variadic min/max — min(a,b,c) instead of min(a,min(b,c))
    // v0.9.0: min/max also accept a single array — `min(xs)` instead of `min(...)` spreads.
    min: (...args) => (args.length === 1 && Array.isArray(args[0])) ? (args[0].length ? Math.min(...args[0]) : null) : Math.min(...args),
    max: (...args) => (args.length === 1 && Array.isArray(args[0])) ? (args[0].length ? Math.max(...args[0]) : null) : Math.max(...args),
    pow: (base, exp) => Math.pow(base, exp),
    atan2: (y, x) => Math.atan2(y, x),
    // v0.8.8: random intrinsics. `random()` returns [0, 1). `randomRange(lo, hi)` returns [lo, hi).
    // Both are non-deterministic — DO NOT use in &physics: (use a seeded RNG if determinism matters).
    random: () => Math.random(),
    randomRange: (lo, hi) => lo + Math.random() * (hi - lo),
    randomInt: (lo, hi) => Math.floor(lo + Math.random() * (hi - lo + 1)),
    // v0.8.8: math constants. Registered as 0-arg functions returning the value. LLMs write
    // `PI` (1 token) instead of `3.14159` (which is also 1 token but error-prone) or a workaround.
    PI: () => Math.PI,
    TAU: () => Math.PI * 2,
    E: () => Math.E,
    // Degree/radian conversions — common in game code (LLMs often mix them up).
    deg2rad: (deg) => deg * Math.PI / 180,
    rad2deg: (rad) => rad * 180 / Math.PI,
    // v0.8.8: lerp intrinsic — `lerp(a, b, t)` returns a + (b - a) * t. Saves 5 tokens vs the
    // manual `a + (b - a) * t` form. Works on numbers; for Vec3 use .lerp() method.
    lerp: (a, b, t) => a + (b - a) * t,
    // v0.8.8: map_range — remap a value from one range to another. Common in shader/game code.
    // map_range(v, in_lo, in_hi, out_lo, out_hi) → out_lo + (v - in_lo) / (in_hi - in_lo) * (out_hi - out_lo)
    map_range: (v, inLo, inHi, outLo, outHi) => { if (inHi === inLo) return outLo; return outLo + (v - inLo) / (inHi - inLo) * (outHi - outLo); },
    vision_cells: (origin, facing, playerPos, range, halfAngleDeg) => {
      const W = 10, H = 10;
      const visibleCells = [];
      for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
        const c = new Vec3(x + 0.5, 0, y + 0.5);
        const toCell = c.sub(origin);
        const d = toCell.mag;
        if (d > range) continue;
        if (d > 1e-6) {
          const cosA = clampNum((origin instanceof Vec3 ? facing : new Vec3(facing.x, 0, facing.y).norm).dot(toCell.norm), -1, 1);
          const angDeg = Math.acos(cosA) * 180 / Math.PI;
          if (angDeg > halfAngleDeg) continue;
        }
        visibleCells.push({ x, y });
      }
      let seenCell = null;
      const pc = { x: clampNum(Math.floor(playerPos.x), 0, W - 1), y: clampNum(Math.floor(playerPos.z !== undefined && playerPos.z !== null ? playerPos.z : (playerPos.y || 0)), 0, H - 1) };
      if (visibleCells.some(c => c.x === pc.x && c.y === pc.y)) seenCell = pc;
      return { visibleCells, seenCell };
    },
    cell_to_world: (cell) => new Vec3(cell.x + 0.5, 0, cell.y + 0.5),
    linear: atom('linear'),
    in: atom('in'),
    out: atom('out'),
    inout: atom('inout'),
    bounce: atom('bounce'),
    elastic: atom('elastic'),
    // v0.5.1: HUD bar descriptor — `bar(pos: v3, w: N, h: N, fg: 0xRRGGBB)`. Sets `__hud: true`
    // so stepRender collects it into world.hudElements (§3.2 design — Native stubs pattern).
    bar: (pos, w, h, fg) => ({ __hud: true, kind: 'bar', pos, w, h, fg }),
    // v0.8.11: general-purpose intrinsics
    // v0.8.12: wrap json_parse/json_stringify in try/catch to prevent crashes
    json_parse: (s) => { try { return JSON.parse(s); } catch(e) { return null; } },
    json_stringify: (v, pretty) => { try { return pretty ? JSON.stringify(v, null, 2) : JSON.stringify(v); } catch(e) { return '<circular>'; } },
    int: (s) => parseInt(s, 10),
    float: (s) => parseFloat(s),
    // v0.9.2: str(v) is exactly what f"{v}" shows.
    str: (v) => stringifyFStringVal(v),
    // v0.9.0: `type()` reports a record's declared ^type name and recognizes functions.
    type: (v) => v == null ? 'null' : typeof v === 'function' ? 'fn' : Array.isArray(v) ? 'array' : typeof v === 'object' ? (v.__callable ? 'fn' : v.__type ? v.__type : v instanceof Atom ? 'atom' : v instanceof Vec3 ? 'vec3' : v instanceof Vec2 ? 'vec2' : v instanceof Quat ? 'quat' : v instanceof EntityInstance ? 'entity' : v instanceof Transform ? 'transform' : v instanceof Mat4 ? 'mat4' : 'dict') : typeof v,
    is_null: (v) => v == null,
    is_number: (v) => typeof v === 'number',
    is_string: (v) => typeof v === 'string',
    is_array: (v) => Array.isArray(v),
    clock: () => (typeof performance !== 'undefined' ? performance.now() : Date.now()) / 1000,
    // v0.8.11: additional math intrinsics
    // v0.8.12: fix NaN on zero-width ranges
    wrap: (x, lo, hi) => { if (hi === lo) return lo; const r = hi - lo; return lo + (((x - lo) % r + r) % r); },
    fract: (x) => x - Math.floor(x),
    smoothstep: (lo, hi, x) => { if (hi === lo) return x >= hi ? 1 : 0; const t = Math.max(0, Math.min(1, (x - lo) / (hi - lo))); return t * t * (3 - 2 * t); },
    hypot: (a, b) => Math.hypot(a, b),
    trunc: (x) => Math.trunc(x),
    cbrt: (x) => Math.cbrt(x),
    log1p: (x) => Math.log1p(x),
    // v0.8.11: bitwise intrinsics (as functions — & sigil is reserved for blocks)
    band: (a, b) => (a | 0) & (b | 0),
    bor: (a, b) => (a | 0) | (b | 0),
    bxor: (a, b) => (a | 0) ^ (b | 0),
    bnot: (x) => ~(x | 0),
    shl: (a, n) => (a | 0) << n,
    shr: (a, n) => (a | 0) >> n,
  };
}

const _patrolState = new WeakMap();
function patrolPoint(entity) {
  let s = _patrolState.get(entity);
  const now = entity.world._simTime || 0;
  if (!s || now - s.t0 > 3.0 || !s.target) {
    const rand = entity.world.intrinsics.random;   // v0.9.2: seeded, like random()
    const x = 1 + rand() * 8;
    s = { t0: now, target: new Vec3(x, 0, 1 + rand() * 8) };
    _patrolState.set(entity, s);
  }
  return s.target;
}

// ==========================================================================================
// SECTION 9: Expression Evaluation
// ==========================================================================================

function flattenPath(node) {
  if (node.type === 'Ident') return [node.name];
  if (node.type === 'Member') return [...flattenPath(node.obj), node.prop];
  return null;
}

function resolveIdent(name, ctx) {
  if (name === 'dt') return ctx.dt;
  if (name === 'self') return ctx.entity;
  // v0.9.0: lexical scope first — parameters, loop variables, and locals shadow entity fields.
  if (ctx.scope) {
    const hit = ctx.scope.lookup(name);
    if (hit !== NOT_BOUND) return hit === undefined ? null : hit;
  }
  // v0.8.2: recognize `null` as a literal (not an Atom). Queries like ?nearest return JS null
  // when nothing is found; without this, `t == null` in axiom source would compare against
  // Atom('null') (the fallback) and always be false — a silent dead-end for null checks.
  if (name === 'null') return null;
  if (name === 'true') return true;
  if (name === 'false') return false;
  if (ctx.eventPayload && Object.prototype.hasOwnProperty.call(ctx.eventPayload, name)) return ctx.eventPayload[name];
  if (ctx.fnParams && Object.prototype.hasOwnProperty.call(ctx.fnParams, name)) return ctx.fnParams[name];
  const e = ctx.entity;
  if (e) {
    const v = e.get(name);
    if (v !== undefined) return v;
  }
  if (name === 'input') return ctx.world.input;
  // v0.9.0: a declared ^fn/^proc used WITHOUT parentheses is a function value, so `xs.map(sq)`,
  // `sort_by(people, age)` and `handlers.push(retry)` all work without a lambda wrapper.
  if (ctx.world) {
    const decl = ctx.world.fns.get(name) || ctx.world.procs.get(name);
    if (decl) return ctx.world.closureFor(decl);
  }
  // Unknown bare identifiers stay atoms (`state = idle`) — the symbol type the language has
  // always used for enum-ish values.
  return atom(name);
}

// v0.8.7: stringify a value for f-string interpolation. Mirrors the implicit coercion used by
// the existing `+` operator (which calls JS `+` and lets the runtime convert), but explicit so
// Vec3 / Quat / Atom / EntityInstance get useful string forms instead of "[object Object]".
function stringifyFStringVal(v) {
  // v0.9.2: one display rule for f-strings, print(), str() and `"text" + v`, shared with the
  // native runtime: records and dicts show as their JSON (a record with a `name` field used to
  // print as just the name), functions as <fn name>, containers by their class name.
  if (v === null || v === undefined) return 'null';
  if (typeof v === 'string') return v;
  if (typeof v === 'number') return String(v);
  if (typeof v === 'boolean') return String(v);
  if (v instanceof Vec3) return `(${v.x},${v.y},${v.z})`;
  if (v instanceof Vec2) return `(${v.x},${v.y})`;
  if (v instanceof Quat) return `(${v.x},${v.y},${v.z},${v.w})`;
  if (v instanceof Atom) return v.name;
  if (v instanceof EntityInstance) return v._tagName || v.decl.name;
  if (Array.isArray(v)) return '[' + v.map(stringifyFStringVal).join(',') + ']';
  if (v instanceof Closure) return `<fn ${v.name === '<lambda>' ? 'lambda' : v.name}>`;
  if (typeof v === 'function') return `<fn ${v.name || 'fn'}>`;
  if (v && typeof v === 'object') {
    // A resource or draw descriptor ({kind, name, …}) shows as its name.
    if (typeof v.kind === 'string' && typeof v.name === 'string') return v.name;
    if (v.constructor && v.constructor.name && v.constructor.name !== 'Object') return v.constructor.name;
    try { return JSON.stringify(v); } catch (e) { return String(v); }
  }
  return String(v);
}

// v0.9.2: apply an f-string format spec (grammar in parser.js splitFormatSpec). Numbers round
// the way toFixed/toExponential do; lengths are counted in characters (code points).
const FORMAT_SPEC_RE = /^(?:([^{}])?([<>^=]))?([+-])?(0)?(\d+)?(,)?(?:\.(\d+))?([fedxXobs%])?$/;
function groupThousands(body) {
  const m = /^(\d+)(.*)$/.exec(body);
  if (!m) return body;
  return m[1].replace(/\B(?=(\d{3})+(?!\d))/g, ',') + m[2];
}
function formatSpec(v, spec) {
  const m = FORMAT_SPEC_RE.exec(spec);
  if (!m) return stringifyFStringVal(v);
  let [, fill, align, sign, zero, width, comma, prec, type] = m;
  width = width ? parseInt(width, 10) : 0;
  const p = prec !== undefined ? Math.min(100, parseInt(prec, 10)) : null;
  if (v && v.__timer) v = v.remaining;
  let signStr = '', body, numeric = false;
  if (typeof v === 'number' && type !== 's') {
    numeric = true;
    const x = type === '%' ? v * 100 : v;
    const a = Math.abs(x);
    if (x < 0) signStr = '-';
    else if (sign === '+') signStr = '+';
    if (!Number.isFinite(a)) body = Number.isNaN(a) ? 'NaN' : 'Infinity';
    else if (type === 'f' || type === '%') body = a.toFixed(p === null ? 6 : p);
    else if (type === 'e') body = a.toExponential(p === null ? 6 : p);
    else if (type === 'd') body = a.toFixed(0);
    else if (type === 'x' || type === 'X' || type === 'o' || type === 'b') {
      body = Math.trunc(a).toString(type === 'o' ? 8 : type === 'b' ? 2 : 16);
      if (type === 'X') body = body.toUpperCase();
    }
    else body = p !== null ? a.toFixed(p) : String(a);
    if (comma) body = groupThousands(body);
    if (type === '%') body += '%';
  } else {
    body = typeof v === 'string' ? v : stringifyFStringVal(v);
    if (p !== null) body = [...body].slice(0, p).join('');
  }
  if (zero && !align) { fill = '0'; align = '='; }
  if (!align) align = numeric ? '>' : '<';
  if (fill === undefined) fill = ' ';
  const len = [...(signStr + body)].length;
  if (width <= len) return signStr + body;
  const pad = width - len;
  if (align === '<') return signStr + body + fill.repeat(pad);
  if (align === '^') { const l = Math.floor(pad / 2); return fill.repeat(l) + signStr + body + fill.repeat(pad - l); }
  if (align === '=' && numeric) return signStr + fill.repeat(pad) + body;
  return fill.repeat(pad) + signStr + body;
}

// v0.9.0: the name to show a model in an error message. `typeof null` is "object" and
// `constructor.name` is absent on a null — both produce messages that point at the wrong
// thing, and a wrong hint is worse than none because it sends the next attempt sideways.
function typeNameOf(v) {
  if (v === null || v === undefined) return 'null';
  if (Array.isArray(v)) return 'array';
  if (typeof v === 'object') {
    if (v.__callable) return 'function';
    if (v.__type) return `record ${v.__type}`;
    if (v.constructor && v.constructor.name && v.constructor.name !== 'Object') return v.constructor.name;
    return 'dict';
  }
  if (typeof v === 'function') return 'function';
  return typeof v;
}

// v0.9.0: dict keys are strings. A computed key is coerced the same way an f-string would
// render it, so `{[1]: "a"}` and `d["1"]` agree.
function stringifyKey(v) {
  if (typeof v === 'string') return v;
  if (typeof v === 'number' || typeof v === 'boolean') return String(v);
  if (v instanceof Atom) return v.name;
  return stringifyFStringVal(v);
}

// v0.9.0: destructuring for multi-variable loops and comprehensions. Arrays destructure by
// position; a dict entry pair from items() is already [key, value]; a record destructures by
// field NAME, so `*name, hp in people:` works on a list of records too.
function destructureElement(item, index, varName) {
  if (item == null) return null;
  if (Array.isArray(item)) return item[index];
  if (item instanceof Vec2) return index === 0 ? item.x : item.y;
  if (item instanceof Vec3) return index === 0 ? item.x : index === 1 ? item.y : item.z;
  if (typeof item === 'object') {
    if (varName !== undefined && Object.prototype.hasOwnProperty.call(item, varName)) return item[varName];
    const keys = Object.keys(item).filter(k => k !== '__type');
    return item[keys[index]];
  }
  return index === 0 ? item : null;
}

function evalExpr(node, ctx) {
  switch (node.type) {
    case 'NumberLit':
      // v0.8.7: numeric literal shortcuts — `3f` (float) and `3v` (uniform Vec3).
      //   - `3f` → 3 (JS numbers are already float64, so this is a no-op semantically; the
      //     token saving is in source: `3f` is 1 token vs `3.0` which is also 1 token but
      //     requires the LLM to emit a decimal point — `3f` is more uniform with `3v`).
      //   - `3v` → Vec3(3, 3, 3) — uniform vector. Saves `v3(3, 3, 3)` (5 tokens) → `3v` (1 token).
      //   - `0.5v` → Vec3(0.5, 0.5, 0.5).
      //   The `f`/`v` suffix is attached by the lexer as `unit` (the existing number+unit
      //   mechanism, e.g. `0.3s` for seconds). Only `f` and `v` are interpreted as type
      //   suffixes here; all other units (s, hz, etc.) are preserved on the NumberLit for
      //   downstream consumers (timers, tick frequencies).
      if (node.unit === 'v') return new Vec3(node.value, node.value, node.value);
      if (node.unit === 'f') return node.value; // already a float
      return node.value;
    case 'StringLit': return node.value;
    // v0.8.7: f-string interpolation — parts is a mix of {kind:'lit', text} and {kind:'expr', node}.
    // Each expr part is evaluated and stringified via the rules below; literals are appended
    // verbatim. The whole thing is one string at the end — token-efficient equivalent of
    // chained `+` concatenation, with no AX-ALLOC-003 contract violation (the cat happens
    // outside hot blocks by convention; if used inside a hot block, it's still flagged).
    case 'FString': {
      let s = '';
      for (const p of node.parts) {
        if (p.kind === 'lit') s += p.text;
        else {
          const v = evalExpr(p.node, ctx);
          s += p.spec !== undefined ? formatSpec(v, p.spec) : stringifyFStringVal(v);
        }
      }
      return s;
    }
    case 'Ident': return resolveIdent(node.name, ctx);
    case 'TagRef': {
      const inst = ctx.world.resolveTag(node.path[0]);
      if (inst !== undefined) {
        let v = inst;
        for (let i = 1; i < node.path.length; i++) v = memberOf(v, node.path[i]);
        return v;
      }
      const res = ctx.world.resources.get(node.path[0]);
      if (res) return res;
      // v0.8.3: if the tag doesn't resolve to an entity or resource, return null instead of
      // throwing AX-RUNTIME-TAG. This makes `#Tag.field` safe to use when the tag might have
      // been despawned mid-game (a common case for AI that tracks other entities). The user
      // can check `#Tag == null` or use `?exists(#Tag)` before accessing fields. A bare `#Tag`
      // (no field path) that doesn't resolve still returns null — the caller can null-check.
      // This matches how `?nearest` already returns null for "not found."
      if (node.path.length === 1) return null; // bare #Tag → null
      // #Tag.field where Tag doesn't exist → null (field access on null is null)
      return null;
    }
    case 'Query': {
      // v0.5.1: receiverless `?name(args)` query → node.obj is null; skip side-effect eval.
      if (node.obj) evalExpr(node.obj, ctx);
      return callQuery(ctx.entity, node.name, node.args, ctx);
    }
    case 'Member': {
      const obj = evalExpr(node.obj, ctx);
      return memberOf(obj, node.prop);
    }
    case 'Index': {
      const obj = evalExpr(node.obj, ctx);
      const idx = evalExpr(node.index, ctx);
      if (obj instanceof Distribution) return obj.massAt(idx);
      // v0.8.11: string indexing — "hello"[0] → "h"
      // v0.9.1: a negative index counts from the end (`xs[-1]` is the last element), which
      // saves the `xs[len(xs) - 1]` dance — 7 tokens — and is the form every model reaches for.
      if (typeof obj === 'string') {
        let i = Math.floor(idx);
        if (i < 0) i += obj.length;
        if (i < 0 || i >= obj.length) return null;
        return obj[i];
      }
      if (Array.isArray(obj)) {
        let i = typeof idx === 'number' ? Math.floor(idx) : idx;
        if (typeof i === 'number' && i < 0) i += obj.length;
        const v = obj[i];
        return v === undefined ? null : v;
      }
      if (obj instanceof BVec) return obj.get(idx);
      if (obj instanceof BMap) return obj.get(idx);
      if (obj instanceof Map) return obj.get(idx);
      // v0.8.11: dict indexing — {a: 1}["a"] → 1, {a: 1}.a → 1 (via memberOf)
      if (obj && typeof obj === 'object' && !Array.isArray(obj) && !(obj instanceof Vec2) && !(obj instanceof Vec3) && !(obj instanceof Quat) && !(obj instanceof EntityInstance)) return obj[idx];
      if (obj && typeof obj === 'object' && obj[idx] !== undefined) return obj[idx];
      if (obj && typeof obj[idx] !== 'undefined') return obj[idx];
      throw new AxiomError(`cannot index ${typeNameOf(obj)} with [${stringifyKey(idx)}] — check the value is an array, string, or dict first (is_null / type)`, 'AX-RUNTIME-INDEX');
    }
    case 'Unary': {
      const v = evalExpr(node.expr, ctx);
      if (node.op === '!') return !truthy(v);
      if (node.op === '-') return v instanceof Vec3 ? v.mulScalar(-1) : v instanceof Vec2 ? v.mulScalar(-1) : -v;
      throw new Error(`unknown unary '${node.op}'`);
    }
    case 'Binary': {
      // v0.8.9: short-circuit evaluation for && and ||.
      if (node.op === '&&') {
        const l = evalExpr(node.left, ctx);
        return truthy(l) ? evalExpr(node.right, ctx) : l;
      }
      if (node.op === '||') {
        const l = evalExpr(node.left, ctx);
        return truthy(l) ? l : evalExpr(node.right, ctx);
      }
      const l = evalExpr(node.left, ctx), r = evalExpr(node.right, ctx);
      if (node.op === '..') return new Range(l, r);
      if (node.op === '?>') {
        // v0.6: real raycast. Syntax: `origin ?> dir * maxDist` — left=origin, right=dir*maxDist.
        // The right side is a direction scaled by max distance. We normalize dir and use maxDist
        // as the search radius. Returns the closest hit point as a Vec3, or null if nothing is hit.
        if (!(l instanceof Vec3)) return null;
        let dir, maxDist;
        if (r instanceof Vec3) {
          maxDist = r.mag;
          dir = r.norm;
        } else if (typeof r === 'number') {
          // Right was a scalar — degenerate form; no direction. Skip.
          return null;
        } else {
          return null;
        }
        return raycastWorld(ctx.world, l, dir, maxDist);
      }
      return binaryOp(node.op, l, r);
    }
    case 'Ternary': {
      const c = evalExpr(node.cond, ctx);
      return truthy(c) ? evalExpr(node.then, ctx) : evalExpr(node.else, ctx);
    }
    // v0.8.8: null-coalescing — `a ?? b`. Returns `a` if it's not null/undefined, else `b`.
    // Note: 0, "", false, NaN are all "not null" — they pass through. Only null/undefined
    // trigger the right-hand side. This matches JS ?? semantics (and Python's `or` does NOT —
    // Python's `or` returns the right side for any falsy value, which is wrong for null-guarding
    // a 0 result).
    case 'NullCoalesce': {
      const l = evalExpr(node.left, ctx);
      if (l !== null && l !== undefined) return l;
      return evalExpr(node.right, ctx);
    }
    case 'InferExpr': {
      const dist = evalExpr(node.dist, ctx);
      if (!(dist instanceof Distribution)) throw new Error(`'~> ${node.op}' used on a non-distribution value`);
      if (node.op === 'argmax') return dist.argmax();
      if (node.op === 'sample') return dist.sample();
      throw new Error(`unknown infer op '${node.op}' (supported: argmax, sample)`);
    }
    case 'MethodCall': {
      const obj = evalExpr(node.obj, ctx);
      return callMethod(obj, node.method, node.args, ctx);
    }
    case 'Call': {
      return callFunction(node.callee, node.args, ctx);
    }
    // v0.9.0: a lambda evaluates to a Closure capturing the current scope/entity.
    case 'Lambda': {
      return new Closure({
        params: node.params, body: node.body, isExpr: true,
        scope: ctx.scope || ctx.world.globalScope, entity: ctx.entity, name: '<lambda>',
      });
    }
    // v0.9.0: `x |> f` — pipe the left value into the right-hand callable as its FIRST argument.
    //   xs |> sum                 → sum(xs)
    //   xs |> map(\v: v * 2)      → map(xs, \v: v * 2)
    //   n  |> \v: v + 1           → (\v: v + 1)(n)
    // Piping into a call inserts the value ahead of the written arguments, which is what makes
    // a chain read in execution order instead of inside-out.
    case 'Pipe': {
      const val = evalExpr(node.left, ctx);
      const r = node.right;
      if (r.type === 'Call') {
        const args = [{ value: { type: '__Value', value: val } }, ...r.args];
        return callFunction(r.callee, args, ctx);
      }
      if (r.type === 'MethodCall') {
        // `x |> obj.m(a)` → obj.m(x, a)
        const obj = evalExpr(r.obj, ctx);
        return callMethod(obj, r.method, [{ value: { type: '__Value', value: val } }, ...r.args], ctx);
      }
      const fn = evalExpr(r, ctx);
      return callValue(fn, [val], ctx, r.name);
    }
    // v0.9.0: calling the result of an expression — `fns[i](x)`, `(\x: x * 2)(4)`.
    case 'CallValue': {
      const fn = evalExpr(node.callee, ctx);
      const argVals = node.args.map(a => evalExpr(a.value, ctx));
      return callValue(fn, argVals, ctx);
    }
    // An already-evaluated value spliced into an argument list (used by `|>`). Never written
    // in source; it exists so piping reuses the ordinary call paths without re-evaluating.
    case '__Value': return node.value;
    case 'ArrayLit': {
      return node.elements.map(e => evalExpr(e, ctx));
    }
    // v0.8.7: array comprehension — [expr for var in iter (if cond)?]
    // Evaluates `iterable`, iterates each element, binds it (or destructures for multi-var),
    // optionally filters via `cond`, and collects `expr` results into a plain JS array.
    // Multi-var destructuring assumes each element is an array; element[0]→vars[0], etc.
    case 'Comprehension': {
      const iterable = evalExpr(node.iterable, ctx);
      // Normalize iterables: arrays, BVec, Range, and string (iterate chars) are supported.
      let seq;
      if (Array.isArray(iterable)) seq = iterable;
      else if (iterable instanceof BVec) seq = iterable.data;
      else if (iterable instanceof Range) {
        seq = Array.from(iterable); // Range implements Symbol.iterator
      }
      else if (typeof iterable === 'string') seq = iterable.split('');
      else if (iterable && typeof iterable[Symbol.iterator] === 'function') seq = Array.from(iterable);
      else seq = [];
      const items = [];
      // v0.9.0: comprehension variables live in a real scope frame, so they shadow entity
      // fields and outer locals the same way a loop variable does, and never leak outward.
      const compScope = new Scope(ctx.scope || (ctx.world && ctx.world.globalScope) || null);
      const childCtx = { ...ctx, scope: compScope };
      for (const item of seq) {
        if (node.vars.length === 1) {
          compScope.declare(node.vars[0], item);
        } else {
          for (let vi = 0; vi < node.vars.length; vi++) {
            compScope.declare(node.vars[vi], destructureElement(item, vi, node.vars[vi]));
          }
        }
        if (node.cond && !truthy(evalExpr(node.cond, childCtx))) continue;
        items.push(evalExpr(node.expr, childCtx));
      }
      return items;
    }
    case 'DictLit': {
      const d = {};
      // v0.9.0: `keyExpr` is the computed-key form `{[expr]: v}`; `key` is the literal form.
      for (const p of node.pairs) {
        const k = p.keyExpr !== undefined ? stringifyKey(evalExpr(p.keyExpr, ctx)) : p.key;
        d[k] = evalExpr(p.value, ctx);
      }
      return d;
    }
    default:
      throw new Error(`cannot evaluate node type '${node.type}'`);
  }
}

function memberOf(obj, prop) {
  if (obj instanceof Vec2) {
    if (prop === 'mag') return obj.mag;
    if (prop === 'norm') return obj.norm;
    if (prop === 'angle') return obj.angle; // v0.8.10
    if (prop === 'x' || prop === 'y') return obj[prop];
  }
  if (obj instanceof Vec3) {
    if (prop === 'mag') return obj.mag;
    if (prop === 'norm') return obj.norm;
    if (prop === 'xy') return obj.xy;
    if (prop === 'xz') return obj.xz; // v0.8.10
    if (prop === 'yz') return obj.yz; // v0.8.10
    if (prop === 'x' || prop === 'y' || prop === 'z') return obj[prop];
  }
  if (obj instanceof Quat) {
    if (prop === 'euler') return obj.euler;
    if (prop === 'conj') return obj.conj;
    if (prop === 'normalized') return obj.normalized;
  }
  if (obj instanceof Mat4) {
    if (prop === 'inv') return obj.inv;
    if (prop === 'T') return obj.T;
  }
  if (obj instanceof Transform) {
    if (prop === 'pos' || prop === 'rot' || prop === 'scl' || prop === 'vel') return obj[prop];
  }
  if (obj instanceof EntityInstance) {
    if (obj.fields.has(prop)) return obj.fields.get(prop);
    if (obj.locals.has(prop)) return obj.locals.get(prop);
    // v0.5: shorthand #Entity.pos -> pose.pos, .vel -> pose.vel, .wpose -> worldPose.
    // v0.8.4 (Bug #8 fix): added .scl -> pose.scl for symmetry with the EntityInstance.get
    // method and the new setter. Previously `#Tag.scl` reads returned undefined here, even
    // though `pose.scl` worked — an asymmetric corner of the cross-entity read path.
    const pose = obj.locals.get('pose');
    if (pose) {
      if (prop === 'pos') return pose.pos;
      if (prop === 'vel') return pose.vel;
      if (prop === 'rot') return pose.rot;
      if (prop === 'scl') return pose.scl;
      if (prop === 'wpose') return pose.worldPose(obj.world);
    }
  }
  if (obj instanceof BVec) {
    if (prop === 'len') return obj.len;
  }
  if (obj instanceof BMap) {
    if (prop === 'len') return obj.len;
  }
  // v0.8.8: plain Array — expose `.length` and `.len` (alias matching BVec convention).
  if (Array.isArray(obj)) {
    if (prop === 'length' || prop === 'len') return obj.length;
  }
  // v0.8.11: string .length / .len
  if (typeof obj === 'string') {
    if (prop === 'length' || prop === 'len') return obj.length;
    if (prop === 'chars') return obj.split(''); // v0.8.12: char array for string processing
  }
  // v0.8.11: plain dict/object — expose `.len`/`.size` for key count
  if (obj && typeof obj === 'object' && !Array.isArray(obj) && !(obj instanceof Vec2) && !(obj instanceof Vec3) && !(obj instanceof Quat) && !(obj instanceof EntityInstance) && !(obj instanceof BVec) && !(obj instanceof BMap) && !(obj instanceof Transform) && !(obj instanceof Mat4)) {
    if (prop === 'len' || prop === 'size') return Object.keys(obj).length;
  }
  if (obj && typeof obj === 'object') return obj[prop];
  throw new Error(`no member '${prop}' on ${obj}`);
}

function binaryOp(op, l, r) {
  // v0.9.2: equality, membership and string concatenation mean the same thing for every type.
  // Before this, `v == v3(0, 0, 0)`, `pos == null`, `v in path` and `"at " + pos` all raised
  // "operator not defined for vectors", and `"a" + [1, 2]` gave "a1,2".
  if (op === '==') return equalsVal(l, r);
  if (op === '!=') return !equalsVal(l, r);
  if (op === '+' && (typeof l === 'string' || typeof r === 'string')) {
    const show = (v) => typeof v === 'string' ? v : (v && v.__timer) ? String(v.remaining) : stringifyFStringVal(v);
    return show(l) + show(r);
  }
  if (op === 'in') return memberIn(l, r);
  if (l instanceof Vec3 || r instanceof Vec3) {
    // v0.5: auto-promote Vec2 to Vec3(x, y, 0) in 3D context
    if (l instanceof Vec2) l = new Vec3(l.x, 0, l.y);
    if (r instanceof Vec2) r = new Vec3(r.x, 0, r.y);
    switch (op) {
      case '+': return l.add(r);
      case '-': return l.sub(r);
      case '*':
        // v0.8.10: Vec3 * Vec3 = component-wise multiply (Hadamard product). Common for
        // vel * friction, color * tint, etc. Use · for dot, × for cross.
        if (l instanceof Vec3 && r instanceof Vec3) return new Vec3(l.x * r.x, l.y * r.y, l.z * r.z);
        if (l instanceof Vec3 && typeof r === 'number') return l.mulScalar(r);
        if (r instanceof Vec3 && typeof l === 'number') return r.mulScalar(l);
        if (r instanceof Quat) return r.rotateVec(l); // v3 * quat = rotation
        break;
      case '/':
        if (l instanceof Vec3 && typeof r === 'number') return l.divScalar(r);
        throw new Error(`'/' requires a vector divided by a scalar`);
      case '\u00B7': return l.dot(r);
      case '\u00D7': return l.cross(r);
      default: throw new Error(`operator '${op}' not defined for vectors`);
    }
  }
  if (l instanceof Vec2 || r instanceof Vec2) {
    switch (op) {
      case '+': return l.add(r);
      case '-': return l.sub(r);
      case '*':
        // v0.8.10: Vec2 * Vec2 = component-wise multiply
        if (l instanceof Vec2 && r instanceof Vec2) return new Vec2(l.x * r.x, l.y * r.y);
        if (l instanceof Vec2 && typeof r === 'number') return l.mulScalar(r);
        if (r instanceof Vec2 && typeof l === 'number') return r.mulScalar(l);
        throw new Error(`'*' between a Vec2 and non-vector/non-number is undefined`);
      case '/':
        if (l instanceof Vec2 && typeof r === 'number') return l.divScalar(r);
        throw new Error(`'/' requires a vector divided by a scalar`);
      case '\u00B7': return l.dot(r);
      case '\u00D7': return l.cross(r);
      default: throw new Error(`operator '${op}' not defined for vectors`);
    }
  }
  if (l instanceof Quat || r instanceof Quat) {
    switch (op) {
      case '*':
        if (l instanceof Quat && r instanceof Quat) return l.mulQuat(r);
        if (l instanceof Quat && r instanceof Vec3) return l.rotateVec(r);
        break;
      default: throw new Error(`operator '${op}' not defined for quaternions`);
    }
  }
  if (l instanceof Mat4 || r instanceof Mat4) {
    switch (op) {
      case '*':
        if (l instanceof Mat4 && r instanceof Mat4) return l.mulMat4(r);
        if (l instanceof Mat4 && r instanceof Vec3) return l.mulVec3(r);
        break;
      default: throw new Error(`operator '${op}' not defined for matrices`);
    }
  }
  // v0.9.0: membership. One operator across every container the language has, because an LLM
  // should not have to remember whether the value in hand is an array, a dict or a string.
  return binaryOpRest(op, l, r);
}

function memberIn(l, r) {
  {
    if (r == null) return false;
    if (typeof r === 'string') return r.indexOf(String(l)) !== -1;
    if (Array.isArray(r)) return r.some(v => equalsVal(v, l));
    if (r instanceof Range) { const n = Number(l); return r.step > 0 ? (n >= r.lo && n < r.hi) : (n <= r.lo && n > r.hi); }
    if (r instanceof BVec) return r.data.slice(0, r.len).some(v => equalsVal(v, l));
    if (r instanceof BMap) return r.has(l);
    if (r instanceof Map || r instanceof Set) return r.has(l);
    if (typeof r === 'object') return Object.prototype.hasOwnProperty.call(r, stringifyKey(l));
    return false;
  }
}

function binaryOpRest(op, l, r) {
  // v0.9.1: array + array concatenates. JavaScript would stringify both sides ("12"), which is
  // never what the program meant.
  if (Array.isArray(l) && Array.isArray(r) && op === '+') return l.concat(r);
  switch (op) {
    case '+': return l + r; case '-': return l - r; case '*': return l * r; case '/': return l / r;
    case '%': return l % r;  // v0.8.8: modulo (numbers only — Vec3 has no modulo)
    case '**': return Math.pow(l, r);  // v0.8.11: exponentiation
    case '>': case '<': case '>=': case '<=': case '==': case '!=': return compareOp(op, l, r);
    default: throw new Error(`unknown operator '${op}'`);
  }
}

// v0.9.0: shared helpers for the higher-order library.
//
// `keySelector` turns whatever was passed — a lambda, a ^fn name, a closure, a field name, or
// nothing — into a JS function. Accepting a field name (`xs.sort_by("hp")`) matters: it is the
// shortest possible spelling of the most common callback, and it cannot be mistyped into
// silence the way a missing lambda could.
function keySelector(sel, ctx) {
  if (sel === undefined || sel === null) return (v) => v;
  if (typeof sel === 'string') return (v) => (v == null ? null : v[sel]);
  if (isCallable(sel, ctx.world)) return (v, i) => callValue(sel, [v, i], ctx);
  if (sel instanceof Atom) return (v) => (v == null ? null : v[sel.name]);
  return () => sel;
}

// Natural ordering: numeric for numbers, lexicographic otherwise. JS's default array sort
// compares stringified values, which puts 10 before 9 — a silent wrong answer.
function defaultCompare(a, b) {
  if (typeof a === 'number' && typeof b === 'number') return a - b;
  if (typeof a === 'boolean' || typeof b === 'boolean') return (a ? 1 : 0) - (b ? 1 : 0);
  const sa = stringifyKey(a), sb = stringifyKey(b);
  return sa < sb ? -1 : sa > sb ? 1 : 0;
}

// Declared parameter count of any callable — used to tell a comparator (2 params) from a key
// function (1 param) in `.sort(...)`.
function arityOf(fn) {
  if (fn instanceof Closure) return fn.params.length;
  if (fn && Array.isArray(fn.params)) return fn.params.length;
  if (typeof fn === 'function') return fn.length;
  return 0;
}

function callMethod(obj, method, argNodes, ctx) {
  // v0.8.11: string methods — split, replace, trim, upper, lower, startsWith, etc.
  if (typeof obj === 'string') {
    const args = argNodes.map(a => a.value ? evalExpr(a.value, ctx) : undefined);
    switch (method) {
      case 'split': return obj.split(args[0]);
      case 'replace': return obj.replace(args[0], String(args[1] ?? ''));
      case 'trim': return obj.trim();
      case 'upper': return obj.toUpperCase();
      case 'lower': return obj.toLowerCase();
      case 'startsWith': return obj.startsWith(args[0]);
      case 'endsWith': return obj.endsWith(args[0]);
      case 'includes': return obj.includes(args[0]);
      case 'contains': return obj.includes(args[0]); // v0.8.12: alias for Python familiarity
      case 'indexOf': return obj.indexOf(args[0]);
      case 'repeat': return obj.repeat(args[0] || 0);
      case 'slice': return args[1] !== undefined ? obj.slice(args[0], args[1]) : obj.slice(args[0]);
      case 'padStart': return obj.padStart(args[0] || 0, args[1] || ' ');
      case 'padEnd': return obj.padEnd(args[0] || 0, args[1] || ' ');
      case 'charAt': return obj.charAt(args[0]);
      case 'charCodeAt': return obj.charCodeAt(args[0]);
      case 'match': { const m = obj.match(args[0]); if (!m) return null; if (args[0] instanceof RegExp && args[0].global) return { matches: Array.from(m), count: m.length }; return { match: m[0], index: m.index, groups: Array.from(m.slice(1)) }; }
      // v0.9.0: the string methods a text-processing program actually reaches for. `lines`,
      // `words` and `chars` are the three shapes almost every parsing task starts from.
      case 'lines': return obj.split(/\r?\n/);
      case 'words': return obj.trim().split(/\s+/).filter(Boolean);
      case 'chars': return obj.split('');
      case 'len': return obj.length;
      case 'reverse': return obj.split('').reverse().join('');
      case 'replace_all': return obj.split(String(args[0])).join(String(args[1] ?? ''));
      case 'capitalize': return obj ? obj[0].toUpperCase() + obj.slice(1) : obj;
      case 'count': { const needle = String(args[0]); if (!needle) return 0; return obj.split(needle).length - 1; }
      case 'to_int': { const n = parseInt(obj.trim(), args[0] || 10); return Number.isNaN(n) ? null : n; }
      case 'to_num': { const n = Number(obj.trim()); return Number.isNaN(n) ? null : n; }
      case 'trim_start': return obj.replace(/^\s+/, '');
      case 'trim_end': return obj.replace(/\s+$/, '');
      case 'is_empty': return obj.length === 0;
    }
  }
  // v0.8.11: dict/object methods — keys, values, entries, has, delete, get, set
  if (obj && typeof obj === 'object' && !Array.isArray(obj) && !(obj instanceof Vec2) && !(obj instanceof Vec3) && !(obj instanceof Quat) && !(obj instanceof EntityInstance) && !(obj instanceof BVec) && !(obj instanceof BMap) && !(obj instanceof Transform) && !(obj instanceof Mat4) && !(obj instanceof Pool) && !(obj instanceof ColliderShape)) {
    const args = argNodes.map(a => a.value ? evalExpr(a.value, ctx) : undefined);
    switch (method) {
      case 'keys': return Object.keys(obj);
      case 'values': return Object.values(obj);
      case 'entries': return Object.entries(obj);
      case 'has': return Object.prototype.hasOwnProperty.call(obj, args[0]);
      case 'delete': delete obj[args[0]]; return undefined;
      case 'get': return (args[0] in obj) ? obj[args[0]] : (args[1] === undefined ? null : args[1]);
      case 'set': obj[args[0]] = args[1]; return args[1];
      // v0.9.0: dicts get the same higher-order surface as arrays, so a record or a lookup
      // table can be transformed without converting it to pairs and back.
      case 'items': return Object.keys(obj).filter(k => k !== '__type').map(k => [k, obj[k]]);
      case 'len': return Object.keys(obj).filter(k => k !== '__type').length;
      case 'map_values': { const f = keySelector(args[0], ctx); const out = {}; for (const k of Object.keys(obj)) { if (k === '__type') continue; out[k] = f(obj[k], k); } return out; }
      case 'filter': { const f = keySelector(args[0], ctx); const out = {}; for (const k of Object.keys(obj)) { if (k === '__type') continue; if (truthy(f(obj[k], k))) out[k] = obj[k]; } return out; }
      case 'each': case 'forEach': { const f = keySelector(args[0], ctx); for (const k of Object.keys(obj)) { if (k === '__type') continue; f(obj[k], k); } return null; }
      case 'merge': return Object.assign({}, obj, ...args.filter(a => a && typeof a === 'object'));
      case 'clone': { try { return JSON.parse(JSON.stringify(obj)); } catch (e) { return Object.assign({}, obj); } }
      case 'is_empty': return Object.keys(obj).filter(k => k !== '__type').length === 0;
    }
  }
  // v0.8.8: plain Array methods. LLMs naturally write `items.push(x)`, `.filter(fn)`, etc.
  // on plain arrays (from `[1,2,3]` literals or comprehensions). Previously these threw
  // "no method '.push(...)' on Array". Now we handle the common Array methods here, BEFORE
  // the type-specific dispatch (Vec2, Vec3, etc.) — Array.isArray() is a reliable guard that
  // won't match any of the other types.
  //
  // Note: BVec already has its own .push/.pop/.get/.set/.clear/.map/.filter handlers below;
  // those take precedence for BVec instances (which are NOT plain Arrays — they're a class
  // with a .data property). This branch only fires for plain JS Arrays.
  if (Array.isArray(obj)) {
    const args = argNodes.map(a => a.value ? evalExpr(a.value, ctx) : undefined);
    switch (method) {
      case 'push': {
        // Mutate + return the pushed value (matches JS Array.push return = new length, but
        // returning the value is more useful in AxiomScript where the result is often logged
        // or stored). For backward compat with JS expectations, return the new length.
        for (const v of args) obj.push(v);
        return obj.length;
      }
      case 'pop': return obj.pop();
      case 'shift': return obj.shift();
      case 'unshift': {
        for (const v of args) obj.unshift(v);
        return obj.length;
      }
      case 'includes': return obj.includes(args[0]);
      case 'contains': return obj.includes(args[0]); // v0.8.12: alias for Python familiarity
      case 'indexOf': return obj.indexOf(args[0]); // v0.8.12: was missing from array dispatch
      case 'lastIndexOf': return obj.lastIndexOf(args[0]);
      case 'slice': {
        const start = args[0]; const end = args[1];
        return end !== undefined ? obj.slice(start, end) : obj.slice(start);
      }
      case 'splice': {
        const start = args[0]; const deleteCount = args[1];
        const items = args.slice(2);
        return obj.splice(start, deleteCount, ...items);
      }
      case 'concat': {
        const out = obj.slice();
        for (const v of args) {
          if (Array.isArray(v)) out.push(...v);
          else out.push(v);
        }
        return out;
      }
      case 'join': {
        const sep = args[0] !== undefined ? String(args[0]) : ',';
        return obj.map(v => (v === null || v === undefined) ? '' : String(v)).join(sep);
      }
      case 'sort': {
        // v0.9.0: the comparator can be a lambda, a ^fn name, a closure in a variable, or a
        // field name (`items.sort("hp")` sorts by that field). Passing nothing sorts naturally
        // (numeric for numbers, lexicographic otherwise) instead of JS's string-only order.
        const sel = args[0];
        if (sel != null) {
          if (isCallable(sel, ctx.world) && arityOf(sel) >= 2) {
            obj.sort((a, b) => { const r = callValue(sel, [a, b], ctx); return typeof r === 'number' ? r : 0; });
            return obj;
          }
          const key = keySelector(sel, ctx);
          obj.sort((a, b) => defaultCompare(key(a), key(b)));
          return obj;
        }
        if (obj.every(v => typeof v === 'number')) obj.sort((a, b) => a - b);
        else obj.sort(defaultCompare);
        return obj;
      }
      case 'reverse': {
        obj.reverse();
        return obj;
      }
      // v0.9.0: the higher-order methods take ANY callable — a lambda, a ^fn by name, a
      // closure held in a variable or a dict, or a field-name string. Before v0.9.0 only a
      // bare ^fn identifier worked and everything else silently returned a copy of the array,
      // which is the worst possible failure mode: no error, wrong answer.
      case 'map': { const f = keySelector(args[0], ctx); return obj.map((v, i) => f(v, i)); }
      case 'filter': { const f = keySelector(args[0], ctx); return obj.filter((v, i) => truthy(f(v, i))); }
      case 'reject': { const f = keySelector(args[0], ctx); return obj.filter((v, i) => !truthy(f(v, i))); }
      case 'reduce': {
        const fn = args[0];
        let acc = args[1];
        let start = 0;
        if (acc === undefined) { acc = obj.length ? obj[0] : null; start = 1; }
        for (let i = start; i < obj.length; i++) acc = callValue(fn, [acc, obj[i], i], ctx);
        return acc;
      }
      case 'find': { const f = keySelector(args[0], ctx); for (let i = 0; i < obj.length; i++) if (truthy(f(obj[i], i))) return obj[i]; return null; }
      case 'find_index': { const f = keySelector(args[0], ctx); for (let i = 0; i < obj.length; i++) if (truthy(f(obj[i], i))) return i; return -1; }
      case 'forEach': case 'each': { const f = keySelector(args[0], ctx); for (let i = 0; i < obj.length; i++) f(obj[i], i); return null; }
      case 'every': case 'all': { const f = keySelector(args[0], ctx); for (let i = 0; i < obj.length; i++) if (!truthy(f(obj[i], i))) return false; return true; }
      case 'some': case 'any': { const f = keySelector(args[0], ctx); for (let i = 0; i < obj.length; i++) if (truthy(f(obj[i], i))) return true; return false; }
      case 'flat_map': case 'flatMap': { const f = keySelector(args[0], ctx); const out = []; for (let i = 0; i < obj.length; i++) { const r = f(obj[i], i); if (Array.isArray(r)) out.push(...r); else out.push(r); } return out; }
      case 'count': {
        if (args[0] === undefined) return obj.length;
        if (isCallable(args[0], ctx.world)) { const f = keySelector(args[0], ctx); let n = 0; for (let i = 0; i < obj.length; i++) if (truthy(f(obj[i], i))) n++; return n; }
        let n = 0; for (const v of obj) if (equalsVal(v, args[0])) n++; return n;
      }
      case 'sum': { const f = keySelector(args[0], ctx); let t = 0; for (let i = 0; i < obj.length; i++) { const v = args[0] === undefined ? obj[i] : f(obj[i], i); t += typeof v === 'number' ? v : Number(v) || 0; } return t; }
      case 'min': { if (!obj.length) return null; const f = keySelector(args[0], ctx); let best = obj[0], bk = args[0] === undefined ? obj[0] : f(obj[0], 0); for (let i = 1; i < obj.length; i++) { const k = args[0] === undefined ? obj[i] : f(obj[i], i); if (defaultCompare(k, bk) < 0) { bk = k; best = obj[i]; } } return best; }
      case 'max': { if (!obj.length) return null; const f = keySelector(args[0], ctx); let best = obj[0], bk = args[0] === undefined ? obj[0] : f(obj[0], 0); for (let i = 1; i < obj.length; i++) { const k = args[0] === undefined ? obj[i] : f(obj[i], i); if (defaultCompare(k, bk) > 0) { bk = k; best = obj[i]; } } return best; }
      case 'sort_by': { const f = keySelector(args[0], ctx); return obj.slice().sort((a, b) => defaultCompare(f(a), f(b))); }
      case 'group_by': { const f = keySelector(args[0], ctx); const out = {}; for (let i = 0; i < obj.length; i++) { const g = stringifyKey(f(obj[i], i)); (out[g] = out[g] || []).push(obj[i]); } return out; }
      case 'uniq': { const f = keySelector(args[0], ctx); const seen = new Set(); const out = []; for (let i = 0; i < obj.length; i++) { const k = stringifyKey(args[0] === undefined ? obj[i] : f(obj[i], i)); if (seen.has(k)) continue; seen.add(k); out.push(obj[i]); } return out; }
      case 'len': return obj.length;
      case 'first': return obj.length ? obj[0] : null;
      case 'last': return obj.length ? obj[obj.length - 1] : null;
      case 'clear': { obj.length = 0; return obj; }
      case 'flat': {
        const depth = args[0] !== undefined ? args[0] : 1;
        return obj.flat(depth);
      }
      case 'fill': {
        const value = args[0];
        const start = args[1] || 0;
        const end = args[2] !== undefined ? args[2] : obj.length;
        obj.fill(value, start, end);
        return obj;
      }
      case 'keys': return obj.keys();
      case 'values': return obj.values();
      case 'entries': return obj.entries();
      // v0.8.11: additional array methods
      case 'some': {
        const fnNode = argNodes[0] && argNodes[0].value;
        if (fnNode && fnNode.type === 'Ident') {
          const fnDecl = ctx.world.fns.get(fnNode.name) || ctx.world.procs.get(fnNode.name);
          if (fnDecl) return obj.some(item => truthy(callUserFn(fnDecl, [item], ctx)));
        }
        return false;
      }
      case 'every': {
        const fnNode = argNodes[0] && argNodes[0].value;
        if (fnNode && fnNode.type === 'Ident') {
          const fnDecl = ctx.world.fns.get(fnNode.name) || ctx.world.procs.get(fnNode.name);
          if (fnDecl) return obj.every(item => truthy(callUserFn(fnDecl, [item], ctx)));
        }
        return true;
      }
      case 'findIndex': {
        const fnNode = argNodes[0] && argNodes[0].value;
        if (fnNode && fnNode.type === 'Ident') {
          const fnDecl = ctx.world.fns.get(fnNode.name) || ctx.world.procs.get(fnNode.name);
          if (fnDecl) return obj.findIndex(item => truthy(callUserFn(fnDecl, [item], ctx)));
        }
        return -1;
      }
      case 'flatMap': {
        const fnNode = argNodes[0] && argNodes[0].value;
        if (fnNode && fnNode.type === 'Ident') {
          const fnDecl = ctx.world.fns.get(fnNode.name) || ctx.world.procs.get(fnNode.name);
          if (fnDecl) return obj.flatMap(item => { const r = callUserFn(fnDecl, [item], ctx); return Array.isArray(r) ? r : [r]; });
        }
        return obj;
      }
    }
    // Fall through to the error below if no method matched.
  }
  if (obj instanceof Vec2) {
    if (method === 'to') {
      const target = evalExpr(argNodes[0].value, ctx);
      const maxDelta = evalExpr(argNodes[1].value, ctx);
      return obj.to(target, maxDelta);
    }
    if (method === 'lerp') { const v = evalExpr(argNodes[0].value, ctx); const t = evalExpr(argNodes[1].value, ctx); return obj.lerp(v, t); }
    if (method === 'mag') return obj.mag;
    if (method === 'norm') return obj.norm;
    if (method === 'clamp') {
      const lo = evalExpr(argNodes[0].value, ctx), hi = evalExpr(argNodes[1].value, ctx);
      return new Vec2(clampNum(obj.x, lo, hi), clampNum(obj.y, lo, hi));
    }
    if (method === 'reflect') { const n = evalExpr(argNodes[0].value, ctx); return obj.reflect(n); }
    if (method === 'rotate') { const a = evalExpr(argNodes[0].value, ctx); return obj.rotate(a); } // v0.8.10
  }
  if (obj instanceof Vec3) {
    if (method === 'to') {
      const target = evalExpr(argNodes[0].value, ctx);
      const maxDelta = evalExpr(argNodes[1].value, ctx);
      return obj.to(target, maxDelta);
    }
    if (method === 'lerp') { const v = evalExpr(argNodes[0].value, ctx); const t = evalExpr(argNodes[1].value, ctx); return obj.lerp(v, t); }
    if (method === 'reflect') { const n = evalExpr(argNodes[0].value, ctx); return obj.reflect(n); }
    if (method === 'rotate') { const q = evalExpr(argNodes[0].value, ctx); return obj.rotate(q); }
    if (method === 'mag') return obj.mag;
    if (method === 'norm') return obj.norm;
    if (method === 'clamp') {
      const lo = evalExpr(argNodes[0].value, ctx), hi = evalExpr(argNodes[1].value, ctx);
      return new Vec3(clampNum(obj.x, lo, hi), clampNum(obj.y, lo, hi), clampNum(obj.z, lo, hi));
    }
  }
  if (obj instanceof Quat) {
    if (method === 'slerp') { const q = evalExpr(argNodes[0].value, ctx); const t = evalExpr(argNodes[1].value, ctx); return obj.slerp(q, t); }
    if (method === 'normalized') return obj.normalized;
  }
  if (obj instanceof Transform) {
    if (method === 'toMat4') return obj.toMat4();
  }
  if (typeof obj === 'number') {
    if (method === 'clamp') {
      const lo = evalExpr(argNodes[0].value, ctx), hi = evalExpr(argNodes[1].value, ctx);
      return clampNum(obj, lo, hi);
    }
  }
  if (obj instanceof Distribution) {
    if (method === 'any') {
      const a = argNodes[0];
      const op = a.predicateOp || '==';
      const value = evalExpr(a.value, ctx);
      return obj.any(op, value);
    }
  }
  if (obj instanceof BVec) {
    // v0.5.1: added missing get/set handlers (were silently throwing "no method '.get(...)' on BVec")
    if (method === 'get') return obj.get(evalExpr(argNodes[0].value, ctx));
    if (method === 'set') { obj.set(evalExpr(argNodes[0].value, ctx), evalExpr(argNodes[1].value, ctx)); return null; }
    if (method === 'push') { return obj.push(evalExpr(argNodes[0].value, ctx)); }
    if (method === 'pop') return obj.pop();
    if (method === 'clear') { obj.clear(); return null; }
    if (method === 'map') {
      const fnNode = argNodes[0].value;
      if (fnNode.type === 'Ident' && fnNode.name) {
        const fnName = fnNode.name;
        return obj.data.map(item => {
          const fnDecl = ctx.world.fns.get(fnName);
          if (fnDecl) return callUserFn(fnDecl, [item], ctx);
          return item;
        });
      }
      return obj.data;
    }
    if (method === 'filter') {
      const fnNode = argNodes[0].value;
      if (fnNode.type === 'Ident' && fnNode.name) {
        const fnName = fnNode.name;
        return obj.data.filter(item => {
          const fnDecl = ctx.world.fns.get(fnName);
          if (fnDecl) return truthy(callUserFn(fnDecl, [item], ctx));
          return true;
        });
      }
      return obj.data;
    }
  }
  if (obj instanceof BMap) {
    if (method === 'get') return obj.get(evalExpr(argNodes[0].value, ctx));
    if (method === 'set') { return obj.set(evalExpr(argNodes[0].value, ctx), evalExpr(argNodes[1].value, ctx)); }
    if (method === 'inc') return obj.inc(evalExpr(argNodes[0].value, ctx), evalExpr(argNodes[1] ? argNodes[1].value : { type: 'NumberLit', value: 1 }, ctx));
    if (method === 'has') return obj.has(evalExpr(argNodes[0].value, ctx));
    if (method === 'clear') { obj.clear(); return null; }
  }
  // v0.9.0: a field holding a function IS a method. This is how objects are written without
  // adding a class construct: `counter = {n: 0, bump: \d: ...}` then `counter.bump(1)`. It
  // also covers a record whose ^type field holds a callback, and an entity field holding one.
  if (obj != null) {
    let member;
    if (obj instanceof EntityInstance) member = obj.get(method);
    else if (typeof obj === 'object') member = obj[method];
    if (member !== undefined && isCallable(member, ctx.world)) {
      return callValue(member, argNodes.map(a => evalExpr(a.value, ctx)), ctx, method);
    }
  }
  throw new AxiomError(`no method '.${method}(...)' on ${typeNameOf(obj)}`, 'AX-RUNTIME-METHOD');
}

function resolveObserveArg(valueNode, ctx) {
  const path = flattenPath(valueNode);
  if (path) {
    const dotted = path.join('.');
    if (ctx.world.channels.latest.has(dotted)) return ctx.world.channels.get(dotted);
  }
  return evalExpr(valueNode, ctx);
}

function callQuery(entity, name, argNodes, ctx) {
  // v0.8.2: global queries — `?nearest(#Prefab, radius?)` works on ANY entity (no subsystem
  // required). Returns the closest entity whose declaration name matches #Prefab, within the
  // optional radius (default: Infinity). Returns null if no match found. The "closest" is
  // measured by 3D Euclidean distance from the calling entity's pose.pos.
  // v0.8.3: `?exists(#Tag)` is also a global query — returns true if the tag resolves to a
  // live entity, false otherwise.
  if (name === 'nearest' || name === 'exists' || name === 'dist') {
    return callGlobalQuery(name, argNodes, entity, ctx);
  }
  // v0.8.3: NavMesh queries (?path, ?raycast, ?block_cell, ?unblock_cell, ?is_blocked) can be
  // called from ANY entity if the first argument is a #Nav tag — e.g. `?path(#Nav, from, to)`.
  // This removes the awkward cross-entity relay pattern where the NavMesh entity had to compute
  // the path in its own &tick and cross-write the result onto the chasing entity's field. Now
  // any &Body3D enemy can call `?path(#Nav, pose.pos, #Player.pos)` directly.
  if ((name === 'path' || name === 'raycast' || name === 'block_cell' || name === 'unblock_cell' || name === 'is_blocked')
      && argNodes[0] && argNodes[0].value && argNodes[0].value.type === 'TagRef') {
    const navEntity = ctx.world.resolveTag(argNodes[0].value.path[0]);
    if (navEntity && navEntity.decl.base === 'NavMesh3D') {
      // Strip the #Nav first arg and dispatch to the NavMesh3D queryImpl with the remaining args.
      const remainingArgs = argNodes.slice(1).map(a => evalExpr(a.value, ctx));
      const subsystem = NATIVE_SUBSYSTEMS['NavMesh3D'];
      return subsystem.queryImpl(name, remainingArgs, navEntity, ctx.world);
    }
  }
  const subsystem = NATIVE_SUBSYSTEMS[entity.decl.base];
  if (!subsystem || !subsystem.queries.includes(name)) {
    throw new Error(`'?${name}(...)' is not a declared query on '&${entity.decl.base}'`);
  }
  const args = argNodes.map(a => evalExpr(a.value, ctx));
  return subsystem.queryImpl(name, args, entity, ctx.world);
}

// v0.8.2: global queries that don't require a subsystem base type. Currently just `nearest`.
function callGlobalQuery(name, argNodes, entity, ctx) {
  if (name === 'nearest') {
    // ?nearest(#Prefab, radius?) → returns the closest EntityInstance, or null.
    const prefabArg = argNodes[0] && argNodes[0].value;
    if (!prefabArg || prefabArg.type !== 'TagRef') {
      throw new Error(`'?nearest(...)' requires a #Prefab tag as the first argument`);
    }
    const prefabName = prefabArg.path[0];
    const radiusArg = argNodes[1] ? evalExpr(argNodes[1].value, ctx) : Infinity;
    const radius = typeof radiusArg === 'number' ? radiusArg : Infinity;
    const myPose = entity.locals.get('pose');
    const myPos = myPose ? myPose.pos : new Vec3(0, 0, 0);
    let best = null, bestDist = Infinity;
    for (const e of ctx.world.entities) {
      if (e === entity) continue; // don't return self
      // Match by declaration name (the @Decl name, not the runtime tag name — spawned instances
      // get unique tag names like 'Enemy_12345' but their decl.name is still 'Enemy').
      // v0.9.2: a spawned copy is named `Enemy_3` but is still an #Enemy — match on the
      // declaration it was spawned from, so `?nearest(#Enemy)` finds copies, not just the original.
      if ((e.decl.prefab || e.decl.name) !== prefabName) continue;
      if (e._pendingRemove) continue; // v0.8.10: skip despawned entities
      const pose = e.locals.get('pose');
      if (!pose) continue;
      const d = Math.hypot(pose.pos.x - myPos.x, pose.pos.y - myPos.y, pose.pos.z - myPos.z);
      if (d < bestDist && d <= radius) { bestDist = d; best = e; }
    }
    return best;
  }
  // v0.8.3: ?exists(#Tag) → returns true if the tag resolves to a live entity, false otherwise.
  // This is the existence check that was missing — previously the only workaround was abusing
  // ?nearest(#Tag, radius) as a guard. Now: `?exists(#Boss) ? "alive" : "dead"`.
  if (name === 'exists') {
    const tagArg = argNodes[0] && argNodes[0].value;
    if (!tagArg || tagArg.type !== 'TagRef') {
      // v0.9.0: `exists(path)` on a string is the filesystem check from the standard library.
      // The two readings never overlap — one takes a #Tag, the other a string — so accepting
      // both here spares the LLM from having to remember which spelling the language wanted.
      const v = tagArg ? evalExpr(tagArg, ctx) : null;
      if (typeof v === 'string') return ctx.world.intrinsics.file_exists(v);
      throw new AxiomError(`'?exists(...)' takes a #Tag (entity check) or a string (file check)`, 'AX-QUERY-001');
    }
    return ctx.world.resolveTag(tagArg.path[0]) !== undefined;
  }
  // v0.8.10: ?dist(#Tag) — 3D Euclidean distance from self to target entity.
  // Saves 3 tokens vs `dist(pose.pos, #Target.pos)`. Returns Infinity if target not found.
  if (name === 'dist') {
    const tagArg = argNodes[0] && argNodes[0].value;
    if (!tagArg || tagArg.type !== 'TagRef') {
      throw new Error(`'?dist(...)' requires a #Tag argument`);
    }
    const target = ctx.world.resolveTag(tagArg.path[0]);
    if (!target) return Infinity;
    const targetPose = target.locals.get('pose');
    if (!targetPose || !targetPose.pos) return Infinity;
    const myPose = entity.locals.get('pose');
    if (!myPose || !myPose.pos) return Infinity;
    return Math.hypot(targetPose.pos.x - myPose.pos.x, targetPose.pos.y - myPose.pos.y, targetPose.pos.z - myPose.pos.z);
  }
  throw new Error(`'?${name}(...)' is not a known global query`);
}

// v0.4: call a user-defined function. Returns the value of the last expression (implicit return).
// v0.9.0: every call gets a fresh scope frame, which is what makes recursion correct. The
// frame's parent is the world's root scope (globals), NOT the caller's frame — AxiomScript is
// lexically scoped, so a function cannot see its caller's locals.
function callUserFn(fnDecl, argVals, ctx) {
  const world = ctx.world;
  if ((world._callDepth = (world._callDepth || 0) + 1) > MAX_CALL_DEPTH) {
    world._callDepth = 0;
    throw new AxiomError(`call depth exceeded ${MAX_CALL_DEPTH} in '${fnDecl.name}' — infinite recursion?`, 'AX-DEPTH-001');
  }
  try {
    const scope = new Scope(world.globalScope, true);
    const defaults = fnDecl.defaults || {};
    const defCtx = { ...ctx, scope, fnParams: null, inFn: true, hot: false };
    for (let i = 0; i < fnDecl.params.length; i++) {
      const pname = fnDecl.params[i];
      let v = i < argVals.length ? argVals[i] : undefined;
      // v0.9.0: a missing (or explicitly null) argument takes the parameter's default.
      if ((v === undefined || v === null) && defaults[pname]) v = evalExpr(defaults[pname], defCtx);
      scope.declare(pname, v === undefined ? null : v);
    }
    scope.declare('args', argVals.slice());
    const fnCtx = { ...ctx, scope, fnParams: null, inFn: true, hot: false, blockName: ctx.blockName };
    let lastVal = null;
    for (const stmt of fnDecl.body) {
      const result = execStmtInner(stmt, fnCtx);
      if (result instanceof ReturnSignal) return result.value;
      if (result !== undefined && !(result instanceof BreakSignal) && !(result instanceof ContinueSignal)) {
        lastVal = result;
      }
    }
    return lastVal;
  } catch (err) {
    throw asDepthError(err);
  } finally {
    world._callDepth--;
  }
}

// The interpreter uses several JS frames per AxiomScript call, so a deep recursion can exhaust
// the host stack before MAX_CALL_DEPTH is reached. Either way the program gets one clear,
// catchable error naming the real cause instead of a host-level RangeError.
function asDepthError(err) {
  // Deliberately no regex here: this runs with the stack already exhausted, where compiling a
  // pattern can fail outright. A plain indexOf costs nothing and cannot.
  if (err instanceof RangeError && String(err.message || '').indexOf('call stack') !== -1) {
    const e = new AxiomError('recursion depth exhausted the host stack — add a base case, rewrite the recursion as a loop, or raise the limit with `node --stack-size=N`', 'AX-DEPTH-001');
    return e;
  }
  return err;
}

function callFunction(name, argNodes, ctx) {
  if (name === 'observe') {
    const obs = {};
    for (const a of argNodes) { if (a.name) obs[a.name] = resolveObserveArg(a.value, ctx); }
    return { __observe: true, obs };
  }
  // v0.8.13: print() as a callable function (2 tokens vs !log's 3). Works in ^fn/^proc bodies.
  if (name === 'print') {
    // v0.9.1: evaluate every argument first, then render. Rendering as we went made the output
    // depend on side effects between arguments (`print(xs, xs.pop())` showed the pre-pop array),
    // which no reader would predict and the native runtime does not reproduce.
    const values = argNodes.map(a => (a && a.value) ? evalExpr(a.value, ctx) : '');
    const parts = values.map(v => (typeof v === 'string') ? v : stringifyFStringVal(v));
    const msg = parts.join(' ');
    ctx.world.log.push({ type: 'log', msg, level: 'info', entity: ctx.entity ? ctx.entity.decl.name : '<fn>' });
    if (!ctx.world._suppressConsole) console.log(msg);
    return null;
  }
  // v0.8.13: len() — universal length for arrays, strings, dicts, BVec, BMap, Pool
  if (name === 'len') {
    const v = argNodes[0] ? evalExpr(argNodes[0].value, ctx) : null;
    if (typeof v === 'string') return v.length;
    if (Array.isArray(v)) return v.length;
    if (v instanceof BVec || v instanceof BMap) return v.len;
    if (v instanceof Pool) return v.liveCount;
    // v0.9.0: a Range knows its own length, and a record's `__type` tag is not a field.
    if (v instanceof Range) return Math.max(0, Math.round(v.hi) - Math.round(v.lo));
    if (v && typeof v === 'object') return Object.keys(v).filter(k => k !== '__type').length;
    return 0;
  }
  // v0.8.13: range() — Python-style range(n) and range(lo, hi)
  if (name === 'range') {
    const args = argNodes.map(a => evalExpr(a.value, ctx));
    if (args.length === 1) return new Range(0, args[0]);
    // v0.9.0: `range(lo, hi, step)`, including a negative step for a countdown. Without a
    // step, counting down needed a manual while loop (≈12 tokens) for no reason.
    if (args.length >= 3) return new Range(args[0], args[1], args[2]);
    return new Range(args[0], args[1]);
  }
  // v0.9.0: innermost binding wins. A local or parameter holding a callable — `f = \x: x + 1`
  // then `f(2)`, or a callback passed in by name — shadows a same-named function or intrinsic,
  // which is what lexical scope means and what makes a parameter called `map` or `sum` safe.
  if (ctx.scope) {
    const v = ctx.scope.lookup(name);
    if (v !== NOT_BOUND && isCallable(v, ctx.world)) {
      return callValue(v, argNodes.map(a => evalExpr(a.value, ctx)), ctx, name);
    }
  }
  // v0.4: user-defined functions AND procs
  let fnDecl = ctx.world.fns.get(name);
  if (!fnDecl) fnDecl = ctx.world.procs.get(name);
  if (fnDecl) {
    const argVals = argNodes.map(a => evalExpr(a.value, ctx));
    return callUserFn(fnDecl, argVals, ctx);
  }
  const intr = ctx.world.intrinsics[name];
  if (intr) {
    const args = argNodes.map(a => evalExpr(a.value, ctx));
    return invokeIntrinsic(intr, args, ctx);
  }
  // v0.9.0: `^type` names are constructors — `^type P: x, y` then `P(1, 2)` or `P(x: 1, y: 2)`
  // builds {x: 1, y: 2, __type: "P"}. Records were declarable but not constructible before.
  const typeDecl = ctx.world.typeDecls.get(name);
  if (typeDecl) return constructRecord(typeDecl, argNodes, ctx);
  if (name === 'patrol_point') return patrolPoint(ctx.entity);
  // v0.9.0: distinguish "no such function" from "that name holds something that is not a
  // function" — the fix for the two is completely different.
  if (ctx.scope) {
    const bound = ctx.scope.lookup(name);
    if (bound !== NOT_BOUND) {
      throw new AxiomError(`'${name}' holds ${typeNameOf(bound)}, which is not callable`, 'AX-CALL-001');
    }
  }
  if (ctx.entity && ctx.entity.get(name) !== undefined) {
    // v0.9.2: a field holding a function is callable by name, like a local holding one.
    // Before this the call failed with "field 'f' holds function, which is not callable".
    const fv = ctx.entity.get(name);
    if (isCallable(fv, ctx.world)) return callValue(fv, argNodes.map(a => evalExpr(a.value, ctx)), ctx, name);
    throw new AxiomError(`field '${name}' holds ${typeNameOf(fv)}, which is not callable`, 'AX-CALL-001');
  }
  throw new AxiomError(`unknown function '${name}(...)'`, 'AX-RUNTIME-FUNC');
}

// v0.9.0: build a record from a ^type declaration. Positional args follow declaration order;
// named args (`P(y: 2)`) may be mixed in; omitted fields are null. The `__type` tag lets
// `type(v)` report the record's name and lets a match arm dispatch on shape.
function constructRecord(typeDecl, argNodes, ctx) {
  const out = { __type: typeDecl.name };
  for (const f of typeDecl.fields) out[f.name] = null;
  let pos = 0;
  for (const a of argNodes) {
    if (!a) continue;
    const v = a.value ? evalExpr(a.value, ctx) : null;
    if (a.name) { out[a.name] = v; continue; }
    const f = typeDecl.fields[pos++];
    if (f) out[f.name] = v;
  }
  return out;
}

// ==========================================================================================
// SECTION 10: Statement Execution
// ==========================================================================================

// v0.9.0: read a bare name for a compound assignment (`x += 1`). Same resolution order as
// resolveIdent, minus the atom fallback (a missing name reads as null, so `n += 1` on an
// undeclared name starts from null rather than from the atom `n`).
// v0.9.0: a match arm pattern. A bare lowercase identifier that is not bound anywhere matches
// by atom equality (`idle`, `running`), which is how state machines are written; everything
// else is an ordinary expression compared by value. A record pattern `P` (a ^type name) matches
// any record carrying that `__type`, so `?* node:` can dispatch on shape.
// v0.9.1: PATTERN MATCHING WITH BINDINGS.
//
// A match arm can now take a value apart and name the pieces in one step:
//
//   ?* node:
//     Add(l, r): ^return ev(l) + ev(r)      // record pattern — binds l and r
//     Num(v) if v > 0: ^return v            // guard, sees the bindings
//     [x, y]: ^return x + y                 // array pattern
//     {name, age}: ^return f"{name}:{age}"  // dict pattern — binds by key
//     _: ^throw "?"
//
// Every tree-shaped program — an interpreter, a JSON walker, a state machine carrying data —
// used to spend a conditional plus two or three field reads per case. This is the single
// largest token saving left in the language, and it removes the class of bug where the test
// and the field access disagree about which variant is in hand.
//
// The binding rule, stated once:
//   * INSIDE a pattern (a constructor's arguments, an array's elements, a dict's values), a
//     bare lowercase name BINDS whatever is in that position, and `_` matches without binding.
//   * At the TOP level of an arm, a bare name still compares — `idle:` tests the atom `idle`,
//     as it always has, because atom-valued state machines are the language's oldest idiom.
//     To capture the whole subject, use `_ if cond:` or match on the type name.
//   * Literals always compare. A capitalised name that is a declared ^type matches by type.
//
// `bindings` is a Map filled as the match proceeds; on a successful arm it becomes the arm's
// scope frame, so a failed arm leaves nothing behind.
function matchPatternInner(subject, patNode, ctx, bindings) {
  switch (patNode.type) {
    case 'Ident': {
      if (patNode.name === '_') return true;                      // wildcard, binds nothing
      if (ctx.world && ctx.world.typeDecls.has(patNode.name)) {    // bare type name: match by type
        return !!(subject && typeof subject === 'object' && subject.__type === patNode.name);
      }
      bindings.set(patNode.name, subject);
      return true;
    }
    case 'Call': {
      // Constructor pattern: `Point(x, y)`. Positional arguments follow the declared field
      // order; named arguments (`Point(y: b)`) pick fields out by name.
      const typeDecl = ctx.world && ctx.world.typeDecls.get(patNode.callee);
      if (!typeDecl) return equalsVal(subject, evalExpr(patNode, ctx));
      if (!subject || typeof subject !== 'object') return false;
      if (subject.__type !== undefined && subject.__type !== typeDecl.name) return false;
      let pos = 0;
      for (const arg of patNode.args) {
        if (!arg || !arg.value) continue;
        const fieldName = arg.name ? arg.name : (typeDecl.fields[pos++] || {}).name;
        if (fieldName === undefined) return false;
        if (!matchPatternInner(subject[fieldName], arg.value, ctx, bindings)) return false;
      }
      return true;
    }
    case 'ArrayLit': {
      if (!Array.isArray(subject)) return false;
      if (subject.length !== patNode.elements.length) return false;
      for (let i = 0; i < patNode.elements.length; i++) {
        if (!matchPatternInner(subject[i], patNode.elements[i], ctx, bindings)) return false;
      }
      return true;
    }
    case 'DictLit': {
      if (!subject || typeof subject !== 'object' || Array.isArray(subject)) return false;
      for (const pair of patNode.pairs) {
        const key = pair.keyExpr !== undefined ? stringifyKey(evalExpr(pair.keyExpr, ctx)) : pair.key;
        if (!Object.prototype.hasOwnProperty.call(subject, key)) return false;
        if (!matchPatternInner(subject[key], pair.value, ctx, bindings)) return false;
      }
      return true;   // a dict pattern matches on the keys it names; extra keys are ignored
    }
    default:
      return equalsVal(subject, evalExpr(patNode, ctx));
  }
}

// Top level of an arm: a bare identifier compares as an atom rather than binding (see above).
function matchesPattern(subject, patNode, ctx, bindings) {
  if (patNode.type === 'Ident') {
    if (patNode.name === '_') return true;
    if (ctx.world && ctx.world.typeDecls.has(patNode.name)) {
      return !!(subject && typeof subject === 'object' && subject.__type === patNode.name);
    }
    return equalsVal(subject, evalExpr(patNode, ctx));
  }
  return matchPatternInner(subject, patNode, ctx, bindings || new Map());
}

function readVar(name, ctx) {
  if (ctx.scope) { const hit = ctx.scope.lookup(name); if (hit !== NOT_BOUND) return hit === undefined ? null : hit; }
  if (ctx.entity) {
    const v = ctx.entity.get(name);
    if (v !== undefined) return v;
  }
  return null;
}

// v0.9.0: the assignment rule, in one place.
//   1. a name already bound in an enclosing scope frame  → write there (params, locals)
//   2. an existing field of the running entity           → write the field (unchanged v0.8
//                                                          behavior for game code)
//   3. otherwise                                         → declare in the current function
//                                                          frame, or on the entity when the
//                                                          statement runs in an entity block
// `declareLocal` (the `~x: v` form) forces case 3's scope branch, which is how a script
// shadows an outer name on purpose.
function writeVar(name, value, ctx, declareLocal) {
  // Inside a function body (`ctx.inFn`), `~x: v` declares/updates a local explicitly.
  if (declareLocal && ctx.inFn && ctx.scope) {
    if (!ctx.scope.setExisting(name, value)) ctx.scope.fnFrame().declare(name, value);
    return;
  }
  if (ctx.scope && ctx.scope.setExisting(name, value)) return;
  // An entity block keeps v0.8 semantics exactly: a bare assignment lands on the entity, so a
  // name used as frame-to-frame scratch state still persists. Only function bodies get true
  // locals for undeclared names — which is precisely the change recursion needed.
  if (ctx.entity && (!ctx.inFn || ctx.entity.get(name) !== undefined)) { ctx.entity.set(name, value); return; }
  if (ctx.scope) { ctx.scope.fnFrame().declare(name, value); return; }
  if (ctx.entity) { ctx.entity.set(name, value); return; }
  throw new AxiomError(`cannot assign '${name}' — no scope and no entity in this context`, 'AX-SCOPE-001');
}

// Run a statement list in a child scope frame (loop bodies, match arms, try blocks). Returns
// a control signal (break/continue/return) or undefined.
function execBody(body, ctx, scope) {
  const bodyCtx = scope ? { ...ctx, scope } : ctx;
  for (const st of body) {
    const r = execStmtInner(st, bodyCtx);
    if (r instanceof BreakSignal || r instanceof ContinueSignal || r instanceof ReturnSignal) return r;
  }
  return undefined;
}

// v0.9.0: does this statement list create a closure? Only an inline lambda can capture a loop
// iteration's scope frame (a declared ^fn closes over the globals, never over its caller), so
// when a body contains none, one scope frame can be reused across iterations instead of one
// per iteration. That keeps frame blocks close to their pre-0.9 allocation profile — the
// zero-allocation contract is the reason &physics exists.
function bodyCreatesClosure(stmt) {
  if (stmt.__capt !== undefined) return stmt.__capt;
  let found = false;
  const seen = new Set();
  const walk = (node) => {
    if (found || !node || typeof node !== 'object') return;
    if (Array.isArray(node)) { for (const n of node) walk(n); return; }
    if (seen.has(node)) return;
    seen.add(node);
    if (node.type === 'Lambda') { found = true; return; }
    for (const k in node) {
      if (k === 'type' || k === 'line' || k === 'col' || k === '__capt') continue;
      const v = node[k];
      if (v && typeof v === 'object') walk(v);
    }
  };
  walk(stmt.body);
  stmt.__capt = found;
  return found;
}

// v0.9.0: loop budget. Inside a hot entity block a runaway loop must not freeze the frame, so
// it is capped; inside a ^fn/^proc/^main there is no cap — a long-running computation is the
// point of a general-purpose language, and the old blanket 10 000-iteration limit made even a
// million-step sum impossible.
function loopLimitFor(ctx) {
  return ctx.hot ? HOT_LOOP_LIMIT : Infinity;
}

function execStmtInner(stmt, ctx) {
  switch (stmt.type) {
    // v0.9.0: `?* subject:` — multiway match on value equality, `_` arm is the default.
    case 'Match': {
      const subject = evalExpr(stmt.subject, ctx);
      const parentScope = ctx.scope || ctx.world.globalScope;
      for (const arm of stmt.arms) {
        let hit = false;
        let bindings = null;
        if (arm.patterns === null) hit = true;   // `_` catch-all (possibly guarded)
        else {
          for (const pat of arm.patterns) {
            const b = new Map();
            // A GUARDED arm whose pattern is a bare name captures the subject instead of
            // comparing against an atom of that name: `n if n > 10:` reads as "call it n, and
            // only take this arm when n > 10", which is the only reading that makes sense —
            // comparing a subject to an atom and then testing that atom is never useful.
            if (arm.guard && pat.type === 'Ident' && pat.name !== '_'
                && !(ctx.world && ctx.world.typeDecls.has(pat.name))) {
              b.set(pat.name, subject);
              hit = true; bindings = b; break;
            }
            if (matchesPattern(subject, pat, ctx, b)) { hit = true; bindings = b; break; }
          }
        }
        if (!hit) continue;
        // The arm's bindings are its scope frame, so the guard and the body see the same names
        // and a failed guard discards them cleanly.
        const armScope = new Scope(parentScope);
        if (bindings) for (const [k, v] of bindings) armScope.declare(k, v);
        if (arm.guard && !truthy(evalExpr(arm.guard, { ...ctx, scope: armScope }))) continue;
        return execBody(arm.body, ctx, armScope);
      }
      return;
    }
    // v0.9.0: `^throw expr` — raise. Any value can be thrown; a dict is the idiomatic form
    // for structured errors (`^throw {code: "E_IO", msg: "no file"}`).
    case 'Throw': {
      const v = stmt.expr ? evalExpr(stmt.expr, ctx) : null;
      throw new AxiomError(v, (v && typeof v === 'object' && v.code) ? v.code : 'AX-THROW');
    }
    // v0.9.0: `^try: ... ^catch e: ... ^fin: ...`. Catches both program throws and engine
    // faults (bad index, unknown method, division by a missing value), so a program can
    // recover instead of losing the rest of the block.
    case 'Try': {
      let signal;
      try {
        signal = execBody(stmt.body, ctx, new Scope(ctx.scope || ctx.world.globalScope));
      } catch (err) {
        if (err instanceof BreakSignal || err instanceof ContinueSignal || err instanceof ReturnSignal) throw err;
        if (!stmt.catchBody) {
          if (stmt.finallyBody) execBody(stmt.finallyBody, ctx, new Scope(ctx.scope || ctx.world.globalScope));
          throw err;
        }
        const catchScope = new Scope(ctx.scope || ctx.world.globalScope);
        if (stmt.catchVar) catchScope.declare(stmt.catchVar, errorToValue(err));
        try {
          signal = execBody(stmt.catchBody, ctx, catchScope);
        } finally {
          if (stmt.finallyBody) execBody(stmt.finallyBody, ctx, new Scope(ctx.scope || ctx.world.globalScope));
        }
        return signal;
      }
      if (stmt.finallyBody) {
        const fsig = execBody(stmt.finallyBody, ctx, new Scope(ctx.scope || ctx.world.globalScope));
        if (fsig) return fsig;
      }
      return signal;
    }
    // v0.5 fix: deep member assignment (pose.vel.y = 0)
    case 'DeepAssign': {
      // v0.8.2: if stmt.tag is set, this is a cross-entity deep assign (#Tag.field.path = expr).
      // Resolve the target entity, then walk the field path on it.
      let obj;
      if (stmt.tag) {
        obj = ctx.world.resolveTag(stmt.tag);
        if (!obj) throw new Error(`'#${stmt.tag}' not found for cross-entity write`);
        // v0.8.9 fix: use memberOf for non-entity objects (Transform, Vec3, etc.) — .get() only exists on EntityInstance.
        for (let i = 0; i < stmt.path.length - 1; i++) {
          if (obj instanceof EntityInstance) {
            obj = obj.get(stmt.path[i]);
          } else {
            obj = memberOf(obj, stmt.path[i]);
          }
        }
      } else {
        obj = resolveIdent(stmt.path[0], ctx);
        for (let i = 1; i < stmt.path.length - 1; i++) {
          obj = memberOf(obj, stmt.path[i]);
        }
      }
      const lastProp = stmt.path[stmt.path.length - 1];
      // v0.8.8: compound assignment on deep path — `pose.vel.x += 1` reads current value,
      // applies the op, writes back. Falls through to plain assign when compoundOp is null.
      // v0.8.17: `??=` (compoundOp '??') short-circuits when current is non-null.
      let v = evalExpr(stmt.value, ctx);
      if (stmt.compoundOp) {
        const current = (obj instanceof EntityInstance) ? obj.get(lastProp) : obj[lastProp];
        if (stmt.compoundOp === '??') {
          // null-coalescing: write only if current is null/undefined.
          if (current !== null && current !== undefined) return;
        } else {
          v = binaryOp(stmt.compoundOp, current, v);
        }
      }
      if (obj instanceof EntityInstance) obj.set(lastProp, v);
      else if (obj && typeof obj === 'object') obj[lastProp] = v;
      return;
    }
    // v0.9.0: `a, b = expr` — unpack an array, a key/value pair, or a record by field name.
    case 'DestructureAssign': {
      const v = evalExpr(stmt.value, ctx);
      for (let i = 0; i < stmt.names.length; i++) {
        writeVar(stmt.names[i], destructureElement(v, i, stmt.names[i]), ctx, false);
      }
      return;
    }
    case 'Assign': {
      if (stmt.op === '~=') {
        const dist = resolveIdent(stmt.target, ctx);
        if (!(dist instanceof Distribution)) throw new Error(`'~=' target '${stmt.target}' is not a distribution`);
        if (stmt.value.type !== 'Call' || stmt.value.callee !== 'observe') {
          throw new Error(`'~=' currently only supports 'observe(...)' on the right-hand side`);
        }
        const result = callFunction('observe', stmt.value.args, ctx);
        const pos = (ctx.entity.locals.get('pose') || new Transform()).pos;
        dist.update(result.obs, pos);
      } else {
        // v0.8.8: compound assignment on plain variable — `x += val` reads current value of x,
        // applies the op, writes back. The op is one of `+ - * / %`. Works on numbers, strings
        // (for `+=` only — concat), Vec3 (for `+=`/`-=`), etc. — whatever binaryOp supports.
        // v0.8.17: `??=` (compoundOp '??') writes only when current is null/undefined.
        let v = evalExpr(stmt.value, ctx);
        if (stmt.compoundOp) {
          const current = readVar(stmt.target, ctx);
          if (stmt.compoundOp === '??') {
            if (current !== null && current !== undefined) return;
          } else {
            v = binaryOp(stmt.compoundOp, current, v);
          }
        }
        writeVar(stmt.target, v, ctx, stmt.sigil === '~');
      }
      return;
    }
    // v0.8.8: array element assignment — `arr[i] = val` or `arr[i] += val`. Resolves the array
    // (must be a plain Array, BVec, or BMap), evaluates the index, applies the compound op if
    // present (reads current value), and writes. For BVec/BMap, uses .set()/.get() methods.
    case 'IndexAssign': {
      const idx = evalExpr(stmt.index, ctx);
      let v = evalExpr(stmt.value, ctx);
      const arr = resolveIdent(stmt.obj, ctx);
      if (stmt.compoundOp) {
        let current;
        if (Array.isArray(arr)) current = arr[idx];
        else if (arr instanceof BVec) current = arr.get(idx);
        else if (arr instanceof BMap) current = arr.get(idx);
        else if (arr && typeof arr === 'object') current = arr[idx];
        else throw new AxiomError(`cannot assign into ${typeNameOf(arr)} by index — '${stmt.obj}' is not an array or dict`, 'AX-RUNTIME-INDEX');
        // v0.8.17: ??= short-circuits on non-null current.
        if (stmt.compoundOp === '??') {
          if (current !== null && current !== undefined) return;
        } else {
          v = binaryOp(stmt.compoundOp, current, v);
        }
      }
      if (Array.isArray(arr)) arr[idx] = v;
      else if (arr instanceof BVec) arr.set(idx, v);
      else if (arr instanceof BMap) arr.set(idx, v);
      else if (arr && typeof arr === 'object') arr[idx] = v;
      else throw new AxiomError(`cannot assign into ${typeNameOf(arr)} by index — '${stmt.obj}' is not an array or dict`, 'AX-RUNTIME-INDEX');
      return;
    }
    // v0.4 fix: member assignment (e.hp = val)
    case 'MemberAssign': {
      // v0.8.2: if stmt.tag is set, this is a cross-entity member assign (#Tag.field = expr).
      // Resolve the target entity and write to its field directly.
      let v = evalExpr(stmt.value, ctx);
      let obj;
      if (stmt.tag) {
        obj = ctx.world.resolveTag(stmt.tag);
        if (!obj) throw new Error(`'#${stmt.tag}' not found for cross-entity write`);
      } else {
        obj = resolveIdent(stmt.obj, ctx);
      }
      // v0.8.8: compound assignment on member — `obj.field += val` reads current value, applies op.
      // v0.8.17: `??=` (compoundOp '??') short-circuits when current is non-null.
      if (stmt.compoundOp) {
        let current;
        if (obj instanceof EntityInstance) current = obj.get(stmt.prop);
        else if (obj && typeof obj === 'object') current = obj[stmt.prop];
        if (stmt.compoundOp === '??') {
          if (current !== null && current !== undefined) return;
        } else {
          v = binaryOp(stmt.compoundOp, current, v);
        }
      }
      if (obj instanceof EntityInstance) {
        obj.set(stmt.prop, v);
      } else if (obj && typeof obj === 'object') {
        obj[stmt.prop] = v;
      }
      return;
    }
    case 'Action': {
      execAction(stmt.name, stmt.args, ctx);
      return;
    }
    case 'Emit': {
      const path = stmt.path.join('.');
      const vals = stmt.args.map(a => evalExpr(a.value, ctx));
      ctx.world.channels.publish(path, vals.length === 1 ? vals[0] : vals, ctx.entity);
      return;
    }
    case 'TransitionChain': {
      const subject = stmt.subject;
      for (const clause of stmt.clauses) {
        if (clause.guard === null || truthy(evalExpr(clause.guard, ctx))) {
          if (clause.target === subject || clause.target === 'self') return;
          const argVals = clause.args.map(a => evalExpr(a.value, ctx));
          if (argVals.length) ctx.entity.locals.set('_arg_' + subject, argVals[0]);
          ctx.entity.set(subject, atom(clause.target));
          return;
        }
      }
      return;
    }
    case 'Broadcast': {
      const payload = {};
      for (const a of stmt.args) { if (a.name) payload[a.name] = evalExpr(a.value, ctx); }
      // v0.8.1: if the event schema has a `source` field and the broadcast args don't include
      // it, default `source` to `self` (the broadcasting entity). This lets `^Hit(damage: 25)
      // within(8)` instead of `^Hit(damage: 25, source: self) within(8)` — saves 3 tokens per
      // broadcast.
      //
      // v0.8.4: the auto-default only applies to REQUIRED fields. Optional fields (declared with
      // `field:: type?`) MUST resolve to `null` when omitted — that's the entire point of marking
      // a field optional. Before this fix, `^event Damage: amount:: number, source:: #Entity?`
      // followed by `^Damage(amount: 5)` would silently populate `source` with the broadcasting
      // entity, defeating the optional contract (test_v03.js #3 "omitted optional field resolves
      // to null in the handler"). The auto-default-for-`source` convenience is preserved for the
      // common required-`source` case (`source:: #Entity`).
      const schema = ctx.world.eventSchemas.get(stmt.name);
      if (schema) {
        for (const f of schema.fields) {
          if (!(f.name in payload)) {
            if (f.optional) {
              payload[f.name] = null;
            } else if (f.name === 'source') {
              payload[f.name] = ctx.entity;
            } else {
              payload[f.name] = null;
            }
          }
        }
      }
      const addressing = resolveAddressing(stmt.addressing, ctx);
      if (ctx.blockName === 'tick') {
        ctx.world._pendingSKernelEvents.push({ name: stmt.name, payload, fromEntity: ctx.entity, addressing });
      } else {
        ctx.world.deliverBroadcast(stmt.name, payload, ctx.entity, addressing);
      }
      return;
    }
    case 'ExprStmt': {
      return evalExpr(stmt.expr, ctx);
    }
    // v0.4: control flow
    case 'CondBlock': {
      let condMet = truthy(evalExpr(stmt.cond, ctx));
      // v0.5: guard is ANDed with condition
      if (condMet && stmt.guard) condMet = truthy(evalExpr(stmt.guard, ctx));
      if (condMet) {
        for (const s of stmt.ifBody) { const r = execStmtInner(s, ctx); if (r instanceof BreakSignal || r instanceof ContinueSignal || r instanceof ReturnSignal) return r; }
      } else if (stmt.elseBody) {
        for (const s of stmt.elseBody) { const r = execStmtInner(s, ctx); if (r instanceof BreakSignal || r instanceof ContinueSignal || r instanceof ReturnSignal) return r; }
      }
      return;
    }
    case 'WhileLoop': {
      let iters = 0;
      const limit = loopLimitFor(ctx);
      const outer = ctx.scope || (ctx.world ? ctx.world.globalScope : null);
      const fresh = bodyCreatesClosure(stmt);
      let scope = new Scope(outer);
      let bodyCtx = { ...ctx, scope };
      while (truthy(evalExpr(stmt.cond, ctx))) {
        if (++iters > limit) throw new AxiomError(`while loop exceeded ${limit} iterations in a hot block`, 'AX-LOOP-002');
        if (fresh) { scope = new Scope(outer); bodyCtx = { ...ctx, scope }; }
        else if (scope.vars.size) scope.vars.clear();
        let r;
        for (const st of stmt.body) {
          r = execStmtInner(st, bodyCtx);
          if (r instanceof BreakSignal || r instanceof ContinueSignal || r instanceof ReturnSignal) break;
          r = undefined;
        }
        if (r instanceof BreakSignal) return;
        if (r instanceof ReturnSignal) return r;
        // ContinueSignal: next iteration.
      }
      return;
    }
    case 'ForLoop': {
      const iterable = evalExpr(stmt.iterable, ctx);
      // v0.9.0: the loop variable lives in a scope frame instead of the entity's locals, so a
      // loop works with no entity present (scripts), nests without clobbering, and does not
      // leave a stray field behind. Multi-variable loops (`*k, v in ...`) destructure here.
      const loopVars = stmt.vars && stmt.vars.length > 1 ? stmt.vars : null;
      const outerScope = ctx.scope || (ctx.world ? ctx.world.globalScope : null);
      const bindLoop = (scope, item) => {
        if (loopVars) {
          for (let vi = 0; vi < loopVars.length; vi++) scope.declare(loopVars[vi], destructureElement(item, vi, loopVars[vi]));
        } else {
          scope.declare(stmt.varName, item);
        }
      };
      const seqLimit = loopLimitFor(ctx);
      let seqIters = 0;
      const freshFrame = bodyCreatesClosure(stmt);
      let loopScope = new Scope(outerScope);
      let loopCtx = { ...ctx, scope: loopScope };
      const runIteration = (item) => {
        if (++seqIters > seqLimit) throw new AxiomError(`for loop exceeded ${seqLimit} iterations in a hot block`, 'AX-LOOP-002');
        if (freshFrame) { loopScope = new Scope(outerScope); loopCtx = { ...ctx, scope: loopScope }; }
        else if (loopScope.vars.size) loopScope.vars.clear();
        bindLoop(loopScope, item);
        for (const st of stmt.body) {
          const r = execStmtInner(st, loopCtx);
          if (r instanceof BreakSignal || r instanceof ContinueSignal || r instanceof ReturnSignal) return r;
        }
        return undefined;
      };
      // Dict / record iteration yields KEYS (v0.8 behavior); `*k, v in items(d):` yields pairs.
      if (iterable && typeof iterable === 'object' && !Array.isArray(iterable) && !(iterable instanceof Range) && !(iterable instanceof BVec) && !(iterable instanceof BMap) && !(iterable instanceof Pool) && !(iterable instanceof Vec2) && !(iterable instanceof Vec3) && !(iterable instanceof Quat) && !(iterable instanceof EntityInstance) && typeof iterable[Symbol.iterator] !== 'function') {
        for (const k of Object.keys(iterable)) {
          const r = loopVars ? runIteration([k, iterable[k]]) : runIteration(k);
          if (r instanceof BreakSignal) return;
          if (r instanceof ReturnSignal) return r;
        }
        return;
      }
      if (iterable instanceof Range || Array.isArray(iterable) || iterable instanceof BVec || typeof iterable === 'string' || (iterable && typeof iterable[Symbol.iterator] === 'function')) {
        const seq = typeof iterable === 'string' ? iterable.split('') : (iterable instanceof BVec ? iterable.data.slice(0, iterable.len) : iterable);
        for (const item of seq) {
          const r = runIteration(item);
          if (r instanceof BreakSignal) return;
          if (r instanceof ReturnSignal) return r;
        }
        return;
      }
      if (iterable instanceof Pool) {
        for (const slot of iterable.slots) {
          if (!slot.used) continue;
          const r = runIteration(slot.data);
          if (r instanceof BreakSignal) return;
          if (r instanceof ReturnSignal) return r;
        }
        return;
      }
      if (iterable == null) return;
      throw new AxiomError(`cannot iterate a ${typeof iterable}`, 'AX-ITER-001');
    }
    case 'Return': {
      return new ReturnSignal(evalExpr(stmt.expr, ctx));
    }
    case 'Break': { return new BreakSignal(); }
    case 'Continue': { return new ContinueSignal(); }
    case 'Assert': {
      const v = evalExpr(stmt.expr, ctx);
      if (!truthy(v)) {
        ctx.world.log.push({ type: 'assert', entity: ctx.entity.decl.name, block: ctx.blockName, line: stmt.line, expr: stmt.expr });
      }
      return;
    }
    default:
      throw new Error(`cannot execute statement type '${stmt.type}'`);
  }
}

function resolveAddressing(node, ctx) {
  if (node.mode === 'to') return { mode: 'to', target: node.target };
  if (node.mode === 'within') {
    const radius = evalExpr(node.radius, ctx);
    const origin = node.origin ? evalExpr(node.origin, ctx) : (ctx.entity.locals.get('pose') || new Transform()).pos;
    return { mode: 'within', radius, origin };
  }
  return { mode: 'global' };
}

// v0.6: Audio playback helpers — best-effort. Never throws. Uses ffplay/aplay/paplay if
// available; falls back to log only. Generates a short sine beep for missing files so the
// runtime produces audible feedback in the sandbox (no real .wav files exist for the demo).
//
// v0.8.1 additions:
//   - Per-channel subprocess tracking: every spawned player is recorded in `_audioChannels`
//     keyed by a "channel" tag (default = the sound name). When a new !play() arrives on the
//     same channel, the previous subprocess is killed (SIGTERM) so overlapping calls don't
//     leak processes. This is a "poor man's mixing" — real mixing would require a real audio
//     backend, which is intentionally out of scope.
//   - Distance-based volume falloff: when the calling entity has a `pose.pos` AND there is
//     an active `&Camera` or `&Listener` entity in the world, the effective volume is
//     `vol * (1 - dist/range)` clamped to [0, 1]. The reference listener is the first
//     `&Listener` entity if any, otherwise the first `&Camera` entity. Falloff is linear
//     with a `range` of 20 units (matches the default Light `range`). Entities with no
//     pose (or no listener) play at full vol — preserves backward compat.
//   - Volume is applied uniformly via the existing volClamped multiplier; verified that all
//     three players (ffplay/aplay/paplay) receive the same scaled buffer.
//
// Remaining limits (documented in AXIOM_REF.md): no true mixing (overlapping sounds on
// different channels still play simultaneously via separate subprocesses — the OS scheduler
// does the "mixing"), no stereo panning, no DSP effects, no music streaming (!music is still
// log-only). The beep envelope is fixed at 100ms; real WAV playback would replace it.
let _audioPlayer = undefined; // cached 'ffplay' | 'aplay' | 'paplay' | null
let _audioLastSpawnAt = 0;
const _audioChannels = new Map(); // channel tag → { proc, startedAt, snd }

function detectAudioPlayer() {
  if (_audioPlayer !== undefined) return _audioPlayer;
  const { execSync } = require('child_process');
  for (const bin of ['ffplay', 'aplay', 'paplay']) {
    try { execSync(`command -v ${bin}`, { stdio: 'ignore' }); _audioPlayer = bin; return _audioPlayer; }
    catch (e) { /* not found */ }
  }
  _audioPlayer = null;
  return _audioPlayer;
}

// v0.8.1: find the active audio listener. Prefers the first &Listener entity (explicit audio
// listener marker); falls back to the first &Camera entity (camera-as-listener is the common
// convention for FPS-style games). Returns the listener's pose.pos as a Vec3, or null if no
// listener entity exists in the world (in which case distance falloff is skipped).
function findListenerPos(world) {
  if (!world || !world.entities) return null;
  let listenerPose = null, cameraPose = null;
  for (const e of world.entities) {
    if (e.decl.base === 'Listener' && !listenerPose) listenerPose = e.locals.get('pose');
    if (e.decl.base === 'Camera' && !cameraPose) cameraPose = e.locals.get('pose');
    if (listenerPose) break; // Listener wins, no need to keep looking
  }
  const pose = listenerPose || cameraPose;
  return pose && pose.pos ? pose.pos : null;
}

// v0.8.1: distance-based volume falloff. `srcPos` is the entity's pose.pos (or null if the
// entity has no pose, e.g. a UI sound). `listenerPos` is from findListenerPos. `range` is
// the distance at which volume reaches 0 (default 20, matches Light range). Returns the
// scaled volume in [0, 1]. If either position is missing, returns `vol` unchanged.
function distanceAttenuatedVolume(vol, srcPos, listenerPos, range = 20) {
  if (!srcPos || !listenerPos) return vol;
  const dx = srcPos.x - listenerPos.x;
  const dy = srcPos.y - listenerPos.y;
  const dz = srcPos.z - listenerPos.z;
  const d = Math.hypot(dx, dy, dz);
  if (d >= range) return 0;
  return Math.max(0, Math.min(1, vol * (1 - d / range)));
}

// v0.8.1: kill any prior subprocess on the given channel. Best-effort — never throws.
// Returns the killed process's PID (or null if none was running on this channel).
function killAudioChannel(channel) {
  const entry = _audioChannels.get(channel);
  if (!entry || !entry.proc) return null;
  try {
    if (typeof entry.proc.kill === 'function') entry.proc.kill('SIGTERM');
    else if (typeof entry.proc.destroy === 'function') entry.proc.destroy();
  } catch (e) { /* best-effort */ }
  _audioChannels.delete(channel);
  return entry.pid || null;
}

// v0.8.1: reap finished channels. Called after each spawn to keep _audioChannels from
// accumulating dead entries. A channel is "done" when its proc emitted 'exit' or 'close'.
function _reapAudioChannels() {
  for (const [ch, entry] of _audioChannels) {
    if (!entry || !entry.proc) { _audioChannels.delete(ch); continue; }
    if (entry._done) _audioChannels.delete(ch);
  }
}

// Exposed for tests: returns a snapshot of currently-active channels (snd → startedAt).
function activeAudioChannels() {
  _reapAudioChannels();
  const out = {};
  for (const [ch, entry] of _audioChannels) out[ch] = { startedAt: entry.startedAt, snd: entry.snd, pid: entry.pid };
  return out;
}

function playSoundBestEffort(snd, vol, world, srcPos) {
  const player = detectAudioPlayer();
  if (!player) return; // sandbox without any player — log already recorded
  // Throttle: don't spawn more than one sound per 50ms (avoid fork-bombing on rapid-fire !play).
  // Note: this is a global throttle; per-channel throttling is handled by killAudioChannel below.
  const now = Date.now();
  if (now - _audioLastSpawnAt < 50) return;
  _audioLastSpawnAt = now;
  // v0.8.1: distance-based volume falloff (only if both src and listener positions are known).
  const listenerPos = findListenerPos(world);
  const effVol = distanceAttenuatedVolume(vol, srcPos, listenerPos);
  if (effVol <= 0.001) return; // fully attenuated — skip spawn entirely
  // Hash the sound name → a stable pitch so different sounds are distinguishable.
  const pitch = 220 + ((snd && snd.toString ? snd.toString() : String(snd)).split('').reduce((a, c) => a + c.charCodeAt(0), 0) % 660);
  // Generate a 100ms sine beep as a 16-bit PCM WAV in memory.
  const sampleRate = 8000;
  const duration = 0.1;
  const n = Math.floor(sampleRate * duration);
  const buf = Buffer.alloc(44 + n * 2);
  buf.write('RIFF', 0);
  buf.writeUInt32LE(36 + n * 2, 4);
  buf.write('WAVE', 8);
  buf.write('fmt ', 12);
  buf.writeUInt32LE(16, 16);  // PCM chunk size
  buf.writeUInt16LE(1, 20);   // PCM format
  buf.writeUInt16LE(1, 22);   // mono
  buf.writeUInt32LE(sampleRate, 24);
  buf.writeUInt32LE(sampleRate * 2, 28);  // byte rate
  buf.writeUInt16LE(2, 32);   // block align
  buf.writeUInt16LE(16, 34);  // bits per sample
  buf.write('data', 36);
  buf.writeUInt32LE(n * 2, 40);
  // v0.8.1: volClamped uses the (attenuated) effVol, applied uniformly across all 3 players.
  const volClamped = Math.max(0, Math.min(1, effVol || 1)) * 0.5;
  for (let i = 0; i < n; i++) {
    const t = i / sampleRate;
    const sample = Math.sin(2 * Math.PI * pitch * t) * volClamped * (1 - t / duration); // decay
    buf.writeInt16LE(Math.max(-32768, Math.min(32767, sample * 32767)), 44 + i * 2);
  }
  const { spawn } = require('child_process');
  const args = player === 'ffplay' ? ['-nodisp', '-autoexit', '-loglevel', 'quiet', '-i', 'pipe:0'] : [];
  // v0.8.1: channel key = sound name (so two !play("shoot.wav") calls overlap-kill each other,
  // but !play("shoot.wav") and !play("hit.wav") play simultaneously on different channels).
  const channel = (snd && snd.toString) ? snd.toString() : String(snd);
  killAudioChannel(channel); // overlap-kill: prior instance of the same sound is terminated
  try {
    const p = spawn(player, args, { stdio: ['pipe', 'ignore', 'ignore'] });
    p.on('error', () => {}); // never propagate audio errors
    // Reap on exit so _audioChannels doesn't grow unbounded.
    p.on('exit', () => { const e = _audioChannels.get(channel); if (e) e._done = true; });
    p.on('close', () => { const e = _audioChannels.get(channel); if (e) e._done = true; });
    p.stdin.write(buf);
    p.stdin.end();
    _audioChannels.set(channel, { proc: p, startedAt: now, snd, pid: p.pid });
    _reapAudioChannels();
  } catch (e) { /* swallow */ }
}

// v0.8.1: stop all audio. Called on world shutdown (if main.js hooks it). Best-effort.
function stopAllAudio() {
  for (const ch of [..._audioChannels.keys()]) killAudioChannel(ch);
}

function playMusicBestEffort(snd, world) {
  // Music is long-form audio. We do NOT spawn a beep loop (that would be obnoxious). Just
  // log the queue entry — the production path would loop the file via SDL_mixer or similar.
  void snd; void world;
}

// v0.8.1: Save schema helpers — compute a fingerprint of the world's entity/field structure
// at save time, and compare it on load to detect schema drift (fields added/removed/renamed
// between when the save was written and when it's loaded). A full migration system is out of
// scope (per the gap spec); detection + a clear diagnostic is the bar.

// Compute the schema fingerprint for the current world. Returns:
//   { axiomVersion: string, entities: { entityName: [sortedFieldNames], ... } }
// Only non-nosave entities are included (matching !save's filter). `decl` is the entity that
// triggered the save (used to look up the program version via decl.world — but we don't have
// a back-reference, so we read the version from the world's loaded program if available).
function computeSaveSchema(world, triggeringDecl) {
  const entities = {};
  for (const e of world.entities) {
    if (e._nosave || e._pendingRemove) continue; // v0.8.12: skip despawned in schema
    const name = e._tagName || e.decl.name;
    // Sort field names so the fingerprint is order-independent (Map iteration order is
    // insertion order, which can vary if fields are declared in a different order).
    entities[name] = [...e.fields.keys()].sort();
  }
  // axiomVersion: read from the world's loaded program if available; otherwise 'unknown'.
  // The program version comes from the `axiom X.Y` header pragma.
  let axiomVersion = 'unknown';
  if (world._program && world._program.version) axiomVersion = world._program.version;
  return { axiomVersion, entities };
}

// Compare a saved schema stamp to the current world's schema. Returns null if they match
// (safe to load), or a mismatch descriptor with `kind` and `detail` fields:
//   { kind: 'version', savedVersion, currentVersion }
//   { kind: 'entity_added', entity }
//   { kind: 'entity_removed', entity }
//   { kind: 'field_added', entity, field }
//   { kind: 'field_removed', entity, field }
//   { kind: 'legacy', detail }  // for saves with no schema stamp at all
// The comparison is conservative: ANY difference in entity set or field-name set is a mismatch.
// Field VALUE types are not checked (too fragile — a number↔string change is usually intentional).
function compareSaveSchemas(saved, current) {
  if (saved.axiomVersion !== current.axiomVersion) {
    return { kind: 'version', savedVersion: saved.axiomVersion, currentVersion: current.axiomVersion };
  }
  const savedEntities = Object.keys(saved.entities || {}).sort();
  const currentEntities = Object.keys(current.entities || {}).sort();
  // Check for added/removed entities.
  for (const name of savedEntities) {
    if (!current.entities[name]) return { kind: 'entity_removed', entity: name };
  }
  for (const name of currentEntities) {
    if (!saved.entities[name]) return { kind: 'entity_added', entity: name };
  }
  // Check for added/removed fields within each shared entity.
  for (const name of savedEntities) {
    const savedFields = saved.entities[name];
    const currentFields = current.entities[name];
    for (const f of savedFields) {
      if (!currentFields.includes(f)) return { kind: 'field_removed', entity: name, field: f };
    }
    for (const f of currentFields) {
      if (!savedFields.includes(f)) return { kind: 'field_added', entity: name, field: f };
    }
  }
  return null; // schemas match — safe to load
}

// Build a runtime fault diagnostic for a schema mismatch, using the standard error shape from
// AXIOM_REF.md (error_code, severity, location, violated_rule, context_snippet, message_for_human,
// message_for_agent, suggested_fix, auto_fixable). Severity is 'fatal' for actual mismatches
// (load is blocked) and 'advisory' for legacy saves (load proceeds but with a warning).
function makeSchemaMismatchFault(entity, blockName, slotName, mismatch, savedSchema) {
  let human, agent, fix;
  if (mismatch.kind === 'legacy') {
    human = `!load("${slotName}"): ${mismatch.detail}`;
    agent = `Save file predates v0.8.1 schema stamping. Re-save with !save("${slotName}") to add the stamp; future loads will then be verified.`;
    fix = `Re-save the slot to upgrade it to the v0.8.1 format.`;
  } else if (mismatch.kind === 'version') {
    human = `!load("${slotName}"): save was written with axiom version '${mismatch.savedVersion}' but the current program is version '${mismatch.currentVersion}'.`;
    agent = `Schema versions differ. The save may have fields the current program doesn't expect, or vice versa. Bump the program version or migrate the save manually.`;
    fix = `Either revert the program to version '${mismatch.savedVersion}' or re-save under the current version after verifying the entity schema is compatible.`;
  } else if (mismatch.kind === 'entity_removed') {
    human = `!load("${slotName}"): save references entity '${mismatch.entity}' which no longer exists in the current program.`;
    agent = `The entity was removed between save and load. Its saved state will be lost.`;
    fix = `Re-add the @${mismatch.entity} entity declaration, or accept the loss and re-save to drop it.`;
  } else if (mismatch.kind === 'entity_added') {
    human = `!load("${slotName}"): current program has entity '${mismatch.entity}' which is not in the save (will keep its default field values).`;
    agent = `The entity was added between save and load. Its fields will retain their declared defaults — no saved state to restore.`;
    fix = `This is usually safe. Re-save to include the new entity's current state.`;
  } else if (mismatch.kind === 'field_removed') {
    human = `!load("${slotName}"): save has field '~${mismatch.field}' on entity '${mismatch.entity}' which no longer exists in the current program.`;
    agent = `The field was removed between save and load. Its saved value will be lost.`;
    fix = `Re-add the ~${mismatch.field} field declaration, or accept the loss and re-save to drop it.`;
  } else if (mismatch.kind === 'field_added') {
    human = `!load("${slotName}"): entity '${mismatch.entity}' has field '~${mismatch.field}' in the current program which is not in the save (will keep its default value).`;
    agent = `The field was added between save and load. It will retain its declared default — no saved value to restore.`;
    fix = `This is usually safe. Re-save to include the new field's current value.`;
  } else {
    human = `!load("${slotName}"): schema mismatch (unknown kind '${mismatch.kind}').`;
    agent = `Unrecognized mismatch kind. See the raw mismatch object for details.`;
    fix = `Re-save to refresh the schema stamp.`;
  }
  return {
    error_code: 'AX-SAVE-001',
    severity: mismatch.kind === 'legacy' || mismatch.kind === 'entity_added' || mismatch.kind === 'field_added' ? 'advisory' : 'fatal',
    location: { entity: entity.decl.name, block: blockName, line: null, col: null },
    violated_rule: { section: 'runtime', title: 'Save/Load Schema Mismatch' },
    context_snippet: null,
    message_for_human: human,
    message_for_agent: agent,
    suggested_fix: fix,
    auto_fixable: false,
    __axiomRuntimeFault: true,
  };
}

function execAction(name, argNodes, ctx) {
  if (name === 'move') {
    // v0.8.8: !move(delta) — adds delta to entity's pose.pos. Equivalent to `pose.pos = pose.pos + delta`.
    // Accepts Vec3 (full 3D move) or Vec2 (treated as XY-plane move, z=0). The legacy `position`
    // Vec2 local is also updated for backward compat with v0.6 code that reads `position` directly.
    const delta = evalExpr(argNodes[0].value, ctx);
    const d3 = delta instanceof Vec3 ? delta
             : delta instanceof Vec2 ? new Vec3(delta.x, 0, delta.y)
             : (typeof delta === 'number') ? new Vec3(delta, 0, 0)
             : new Vec3(0, 0, 0);
    const pose = ctx.entity.locals.get('pose');
    if (pose) pose.pos = pose.pos.add(d3);
    // Legacy: also update the `position` Vec2 local if it exists (v0.6 convention).
    const pos2 = ctx.entity.locals.get('position');
    if (pos2 instanceof Vec2) {
      ctx.entity.locals.set('position', new Vec2(pos2.x + d3.x, pos2.y + d3.z));
    } else {
      // Create it if missing — some code reads `position` directly.
      ctx.entity.locals.set('position', new Vec2(d3.x, d3.z));
    }
    return;
  }
  if (name === 'spawn') {
    const firstArg = argNodes[0] && argNodes[0].value;
    // v0.4: !spawn(#Entity, at: v3(...)) — runtime entity instantiation
    if (firstArg && firstArg.type === 'TagRef') {
      const declName = firstArg.path[0];
      const decl = ctx.world.entityDecls.get(declName);
      if (!decl) throw new Error(`!spawn(#${declName}): no entity declaration '${declName}' found`);
      // v0.8.10: use per-world counter instead of Date.now() to avoid name collisions
      // when two !spawn calls happen within the same millisecond.
      if (!ctx.world._spawnCounter) ctx.world._spawnCounter = 0;
      ctx.world._spawnCounter++;
      const inst = ctx.world.addEntity({ ...decl, name: declName + '_' + ctx.world._spawnCounter, prefab: declName, mixins: [] });
      // Apply overrides from named args
      for (let i = 1; i < argNodes.length; i++) {
        const a = argNodes[i];
        if (a.name === 'at') {
          const val = evalExpr(a.value, ctx);
          if (val instanceof Vec3) { inst.locals.get('pose').pos = val; inst.locals.set('position', val.xy); }
        } else if (a.name) {
          inst.set(a.name, evalExpr(a.value, ctx));
        }
      }
      return inst;
    }
    // Original pool spawn
    const pool = evalExpr(firstArg, ctx);
    if (!(pool instanceof Pool)) throw new Error(`'!spawn(...)': first argument must be a '&Pool' field or a #TagRef`);
    const data = spawnInit(argNodes[1].value, ctx);
    return pool.spawn(data);
  }
  if (name === 'release') {
    const pool = evalExpr(argNodes[0].value, ctx);
    if (!(pool instanceof Pool)) throw new Error(`'!release(...)': first argument must be a '&Pool' field`);
    const handle = evalExpr(argNodes[1].value, ctx);
    pool.release(handle);
    return;
  }
  // v0.8.2: !despawn(self) or !despawn(#Tag) — remove an entity from the world.
  // `!despawn(self)` removes the calling entity (the one whose &physics/&tick block is running).
  // `!despawn(#Tag)` removes a specific tagged entity. The entity stops receiving all updates
  // immediately. This is the counterpart to !spawn — without it, dead entities accumulate
  // forever (the only workaround was a ~dead flag + teleport-below-floor, which wastes a
  // physics/tick/render pass on every "dead" entity for the rest of the session).
  if (name === 'despawn') {
    const arg = argNodes[0] && argNodes[0].value;
    if (!arg) throw new Error(`'!despawn(...)' requires an argument: self or #Tag`);
    let target = null;
    if (arg.type === 'TagRef') {
      // !despawn(#Tag) — resolve the tagged entity.
      target = ctx.world.resolveTag(arg.path[0]);
      if (!target) throw new Error(`'!despawn(#${arg.path[0]})': entity not found`);
    } else if (arg.type === 'Ident' && arg.name === 'self') {
      // !despawn(self) — remove the calling entity.
      target = ctx.entity;
    } else {
      // Try evaluating as an expression (might be a variable holding an entity).
      const v = evalExpr(arg, ctx);
      if (v instanceof EntityInstance) target = v;
      else throw new Error(`'!despawn(...)': argument must be 'self' or #Tag (got ${typeof v})`);
    }
    ctx.world.removeEntity(target);
    ctx.world.log.push({ type: 'despawn', entity: target._tagName || target.decl.name });
    return;
  }
  // v0.4: tween
  if (name === 'tween') {
    const targetExpr = argNodes[0] && argNodes[0].value;
    const endValExpr = argNodes[1] && argNodes[1].value;
    const durExpr = argNodes[2] && argNodes[2].value;
    const easeExpr = argNodes[3] && argNodes[3].value;
    if (!targetExpr || !endValExpr || !durExpr) throw new Error('!tween requires at least 3 args: target, endValue, duration');
    // Resolve target as a property path
    const path = flattenPath(targetExpr);
    if (!path || path.length < 1) throw new Error('!tween target must be a dotted property path');
    const rootObj = path.length === 1 ? ctx.entity : resolveIdent(path[0], ctx);
    const propPath = path.length === 1 ? [path[0]] : path.slice(1);
    const endVal = evalExpr(endValExpr, ctx);
    let duration = evalExpr(durExpr, ctx);
    // Handle unit suffixes
    if (durExpr.unit === 's') duration = duration;
    else if (durExpr.unit === 'ms') duration = duration / 1000;
    const easeName = easeExpr ? (evalExpr(easeExpr, ctx).name || evalExpr(easeExpr, ctx)) : 'out';
    ctx.world.tweens.push(new Tween(rootObj, propPath, endVal, duration, String(easeName)));
    return;
  }
  // v0.4: save/load
  if (name === 'save') {
    const slotName = argNodes[0] ? evalExpr(argNodes[0].value, ctx) : 'default';
    const state = {};
    for (const e of ctx.world.entities) {
      if (e._nosave || e._pendingRemove) continue; // v0.8.12: skip despawned entities in save
      const entityState = { fields: {}, locals: {} };
      for (const [k, v] of e.fields) {
        if (v instanceof Distribution) entityState.fields[k] = { __dist: true, w: v.w, h: v.h, weights: v.weights, particles: v.particles, mode: v.mode };
        else if (v && v.__timer) entityState.fields[k] = v;
        else entityState.fields[k] = v;
      }
      for (const [k, v] of e.locals) {
        if (v instanceof Transform) entityState.locals[k] = { __transform: true, pos: [v.pos.x, v.pos.y, v.pos.z], rot: [v.rot.x, v.rot.y, v.rot.z, v.rot.w], scl: [v.scl.x, v.scl.y, v.scl.z], vel: [v.vel.x, v.vel.y, v.vel.z] };
        else if (v instanceof Vec2) entityState.locals[k] = { __v2: true, x: v.x, y: v.y };
        else if (v instanceof Vec3) entityState.locals[k] = { __v3: true, x: v.x, y: v.y, z: v.z };
        else if (v instanceof Atom) entityState.locals[k] = { __atom: true, name: v.name };
        else entityState.locals[k] = v;
      }
      state[e._tagName || e.decl.name] = entityState;
    }
    // v0.8.1: stamp a schema fingerprint on the save so !load can detect incompatible saves.
    // The fingerprint is the sorted list of {entityName: [sortedFieldNames]} for all non-nosave
    // entities. If the entity schema changes between save and load (fields added/removed/renamed),
    // the fingerprint differs and !load surfaces a clear diagnostic instead of silently loading
    // stale data. axiomVersion comes from the program's version pragma (or 'unknown' if absent).
    const schema = computeSaveSchema(ctx.world, ctx.entity.decl);
    const wrapped = { __axiomSchema: schema, entities: state };
    ctx.world.saveSlots.set(String(slotName), wrapped);
    // v0.6: persist to disk at `saves/${slotName}.json`. Best-effort: silent on failure (no
    // permissions, no fs access in sandbox, etc.). The in-memory saveSlots map is the source of
    // truth for !load; the disk file is the durable copy. v0.8.1: writes the WRAPPED state
    // (with __axiomSchema) so cross-process loads can detect schema drift.
    // v0.8.7: in sandbox mode, SKIP the disk write entirely — saves stay in-memory only.
    // This prevents untrusted .ax files from writing arbitrary files to the host filesystem.
    if (ctx.world.sandbox) {
      // In-memory only; no fs access. The save is still usable via !load in the same session.
      return;
    }
    try {
      const fs = require('fs');
      const path = require('path');
      const dir = path.resolve('saves');
      if (!fs.existsSync(dir)) fs.mkdirSync(dir, { recursive: true });
      const filePath = path.join(dir, String(slotName) + '.json');
      fs.writeFileSync(filePath, JSON.stringify(wrapped, null, 2));
    } catch (e) { /* best-effort */ }
    return;
  }
  if (name === 'load') {
    const slotName = argNodes[0] ? evalExpr(argNodes[0].value, ctx) : 'default';
    // v0.6: prefer the in-memory slot (set by a prior !save in this session); fall back to disk
    // if the in-memory slot is missing. This lets !load("slot") recover state across process
    // restarts (production) while keeping the round-trip semantics for tests.
    // v0.8.7: in sandbox mode, SKIP the disk read entirely — only in-memory slots are visible.
    let wrapped = ctx.world.saveSlots.get(String(slotName));
    if (!wrapped && !ctx.world.sandbox) {
      try {
        const fs = require('fs');
        const path = require('path');
        const filePath = path.resolve('saves', String(slotName) + '.json');
        if (fs.existsSync(filePath)) {
          wrapped = JSON.parse(fs.readFileSync(filePath, 'utf8'));
          ctx.world.saveSlots.set(String(slotName), wrapped);
        }
      } catch (e) { /* best-effort */ }
    }
    if (!wrapped) return;
    // v0.8.1: schema-version check. If the save has a __axiomSchema stamp, compare its fingerprint
    // to the current world's entity/field names. On mismatch, emit a runtime diagnostic (using
    // the standard error shape from AXIOM_REF.md) and SKIP the load — silently loading stale
    // data with missing/renamed fields would produce a partially-populated world with no warning.
    // Legacy v0.6 saves (no __axiomSchema) are loaded with an advisory diagnostic.
    let state;
    if (wrapped.__axiomSchema) {
      const currentSchema = computeSaveSchema(ctx.world, ctx.entity.decl);
      const mismatch = compareSaveSchemas(wrapped.__axiomSchema, currentSchema);
      if (mismatch) {
        const fault = makeSchemaMismatchFault(ctx.entity, ctx.blockName, slotName, mismatch, wrapped.__axiomSchema);
        ctx.world.runtimeDiagnostics.push(fault);
        // v0.8.1: advisory mismatches (entity_added, field_added, legacy) don't block the load —
        // the new entity/field just keeps its declared defaults, which is safe. Fatal mismatches
        // (version, entity_removed, field_removed) DO block the load to prevent silent corruption.
        if (fault.severity === 'fatal') {
          return; // do NOT load — surfaces the problem instead of silently corrupting state
        }
        // Advisory: fall through and load, but the diagnostic is already recorded.
      }
      state = wrapped.entities;
    } else {
      // Legacy v0.6 save (no schema stamp). Load it but warn — there's no way to verify compat.
      ctx.world.runtimeDiagnostics.push(makeSchemaMismatchFault(ctx.entity, ctx.blockName, slotName,
        { kind: 'legacy', detail: 'Save file has no schema stamp (pre-v0.8.1 format). Loading without verification.' },
        null));
      state = wrapped; // legacy saves store the entity map directly at the top level
    }
    for (const e of ctx.world.entities) {
      const key = e._tagName || e.decl.name;
      const s = state[key];
      if (!s) continue;
      for (const [k, v] of Object.entries(s.fields || {})) {
        if (v && v.__dist) {
          const d = e.fields.get(k);
          if (d instanceof Distribution) { d.weights = v.weights; d.particles = v.particles; d._massCache = null; }
        } else if (v && v.__timer) {
          e.fields.set(k, v);
        } else {
          e.fields.set(k, v);
        }
      }
      for (const [k, v] of Object.entries(s.locals || {})) {
        if (v && v.__transform) {
          const t = new Transform();
          t.pos = new Vec3(...v.pos); t.rot = new Quat(...v.rot); t.scl = new Vec3(...v.scl); t.vel = new Vec3(...v.vel);
          t._entity = e;
          e.locals.set(k, t);
        } else if (v && v.__v2) e.locals.set(k, new Vec2(v.x, v.y));
        else if (v && v.__v3) e.locals.set(k, new Vec3(v.x, v.y, v.z));
        else if (v && v.__atom) e.locals.set(k, new Atom(v.name));
        else e.locals.set(k, v);
      }
    }
    return;
  }
  // v0.4: audio
  if (name === 'play') {
    // v0.6: best-effort audio playback. The reference interpreter has no real WAV loader, so for
    // missing sound files we generate a short beep (sine wave at 440Hz * 1/vol for variety) and
    // pipe it to whatever audio player is available (ffplay / aplay / paplay, in that order). If
    // no player is found, we fall back to log only — the runtime never throws on audio.
    // v0.8.1: pass the entity's pose.pos through to playSoundBestEffort so it can apply distance-
    // based volume falloff relative to the active listener (&Listener or &Camera).
    // v0.8.7: in sandbox mode, STUB the subprocess call — log only, no ffplay/aplay/paplay spawn.
    // This prevents untrusted .ax files from spawning arbitrary subprocesses on the host.
    const snd = argNodes[0] ? evalExpr(argNodes[0].value, ctx) : null;
    const vol = argNodes[1] ? evalExpr(argNodes[1].value, ctx) : 1;
    const pose = ctx.entity.locals.get('pose');
    const srcPos = pose && pose.pos ? pose.pos : null;
    ctx.world.log.push({ type: 'play', snd, vol, entity: ctx.entity.decl.name, pos: srcPos ? [srcPos.x, srcPos.y, srcPos.z] : null, sandbox: !!ctx.world.sandbox });
    if (!ctx.world.sandbox) {
      playSoundBestEffort(snd, vol, ctx.world, srcPos);
    }
    return;
  }
  if (name === 'music') {
    // v0.6: queue a music track. Like !play, this is best-effort — if no player is available, the
    // track is recorded in world.musicQueue (for tests + native code to inspect) but not played.
    const snd = argNodes[0] ? evalExpr(argNodes[0].value, ctx) : null;
    ctx.world.log.push({ type: 'music', snd, entity: ctx.entity.decl.name });
    ctx.world.musicQueue = ctx.world.musicQueue || [];
    ctx.world.musicQueue.push({ snd, entity: ctx.entity.decl.name, t: ctx.world._simTime });
    playMusicBestEffort(snd, ctx.world);
    return;
  }
  // v0.8.1: !play_anim(#Mesh, clip_name, loop?) — drive joint matrices from a glTF animation clip.
  // The mesh must have been loaded from a .glb file with `animations` data; the clip_name selects
  // which animation to play (falls back to the first clip if name not found). The animation time
  // advances automatically each render step (see World.stepRender). Loop defaults to false.
  if (name === 'play_anim') {
    const meshRef = argNodes[0] ? evalExpr(argNodes[0].value, ctx) : null;
    const clipName = argNodes[1] ? evalExpr(argNodes[1].value, ctx) : null;
    const loop = argNodes[2] ? !!evalExpr(argNodes[2].value, ctx) : false;
    let clip = null;
    if (meshRef && Array.isArray(meshRef.animations) && meshRef.animations.length > 0) {
      if (clipName) {
        clip = meshRef.animations.find(a => a.name === String(clipName)) || meshRef.animations[0];
      } else {
        clip = meshRef.animations[0];
      }
    }
    if (clip) {
      ctx.entity._activeAnim = { clip, time: 0, loop, meshRef };
    } else {
      // No clip found — clear any active anim so the entity returns to rest pose.
      ctx.entity._activeAnim = null;
    }
    ctx.world.log.push({ type: 'play_anim', clip: clip ? clip.name : null, loop, entity: ctx.entity.decl.name, found: !!clip });
    return;
  }
  // v0.4: mesh draw
  // v0.8.13: !stop_anim — stop active animation on entity
  if (name === 'stop_anim') {
    ctx.entity._activeAnim = null;
    ctx.entity._jointMatrices = null;
    return;
  }
  if (name === 'mesh') {
    const meshRef = argNodes[0] ? evalExpr(argNodes[0].value, ctx) : null;
    const kwargs = {};
    for (let i = 1; i < argNodes.length; i++) {
      if (argNodes[i].name) kwargs[argNodes[i].name] = evalExpr(argNodes[i].value, ctx);
    }
    ctx.world.drawList.push({ entity: ctx.entity.decl.name, mesh: meshRef, ...kwargs });
    // v0.8.1: record the most recent skinned mesh on the entity so stepRender can build
    // _jointMatrices from manual ~joint_pos/~joint_rot/~joint_scl arrays (without needing
    // !play_anim). Only set if this mesh actually has a skin; otherwise leave any prior value.
    if (meshRef && meshRef.skin) {
      ctx.entity._skinningMesh = meshRef;
    }
    return;
  }
  // v0.4: debug
  if (name === 'dbg_line') {
    const a = evalExpr(argNodes[0].value, ctx);
    const b = evalExpr(argNodes[1].value, ctx);
    const color = argNodes[2] ? evalExpr(argNodes[2].value, ctx) : 'red';
    ctx.world.log.push({ type: 'dbg_line', a, b, color, entity: ctx.entity.decl.name });
    return;
  }
  // v0.8.12: !print is an alias for !log (saves 1 token for non-game scripting).
  if (name === 'print' || name === 'log') {
    // v0.8.11: variadic !log — all args are stringified and joined with space.
    // !log("x=", x, " y=", y) → "x=5 y=10"
    const parts = argNodes.map(a => {
      if (!a || !a.value) return '';
      const v = evalExpr(a.value, ctx);
      return (typeof v === 'string') ? v : stringifyFStringVal(v);
    });
    const msg = parts.join(' ');
    ctx.world.log.push({ type: 'log', msg, level: 'info', entity: ctx.entity.decl.name });
    // v0.8.13: also output to console for interactive/eval use — but only if not in JSON mode
    if (!ctx.world._suppressConsole) console.log(msg);
    return;
  }
  // v0.8.7: `!d(...)` — token-efficient debug log for hot blocks (&physics/&render/&on).
  // The 2-token `!d` replaces the workaround of moving logs to a separate `&tick(1hz):` block
  // (which costs 15-20 tokens of boilerplate). The checker whitelists `!d` in zero-alloc blocks
  // (no AX-ALLOC-003 for string concat in its args).
  //
  // Runtime: args are evaluated and pushed to a fixed-size ring buffer (no per-call heap
  // allocation beyond the arg values themselves, which already exist). The buffer is flushed
  // to world.log at the end of each frame (stepRender / update). This keeps the hot path
  // allocation-free in the sense that matters: no growing data structures, no GC pressure
  // from log entry objects during physics/render steps.
  if (name === 'd') {
    // Evaluate args lazily — only when the buffer is flushed, not now. But eval requires ctx,
    // which is only valid during the block's execution. So we eval NOW and store the values.
    // The "no allocation in hot path" claim is about not creating log-entry objects or growing
    // arrays — the arg values themselves are user data that already exists. We push a compact
    // record to a pre-allocated ring buffer.
    if (!ctx.world._debugRing) {
      ctx.world._debugRing = { buf: new Array(256), head: 0, count: 0 };
    }
    const args = argNodes.map(a => evalExpr(a.value, ctx));
    const entry = {
      entity: ctx.entity.decl.name,
      block: ctx.blockName,
      args,
      t: ctx.world._simTime,
    };
    const ring = ctx.world._debugRing;
    ring.buf[ring.head] = entry;
    ring.head = (ring.head + 1) % ring.buf.length;
    if (ring.count < ring.buf.length) ring.count++;
    return;
  }
  // Native subsystem actions
  const subsystem = ctx.entity ? NATIVE_SUBSYSTEMS[ctx.entity.decl.base] : null;
  if (subsystem && subsystem.actions.includes(name)) {
    const args = argNodes.map(a => evalExpr(a.value, ctx));
    return subsystem.actionImpl(name, args, ctx.entity, ctx.world);
  }
  // v0.9.0: `!name(...)` falls back to a user ^proc/^fn, to a callable held in a local, and
  // finally to an intrinsic. Statement position is where an LLM naturally writes a call whose
  // result it does not use, and before this a `!draw_row(y)` calling a declared ^proc failed at
  // runtime with "undefined action" — a confusing error for correct-looking code.
  const userDecl = ctx.world.procs.get(name) || ctx.world.fns.get(name);
  if (userDecl) {
    const args = argNodes.map(a => evalExpr(a.value, ctx));
    return callUserFn(userDecl, args, ctx);
  }
  if (ctx.scope && ctx.scope.has(name)) {
    const v = ctx.scope.get(name);
    if (isCallable(v, ctx.world)) return callValue(v, argNodes.map(a => evalExpr(a.value, ctx)), ctx, name);
  }
  const intrinsic = ctx.world.intrinsics[name];
  if (typeof intrinsic === 'function') {
    return invokeIntrinsic(intrinsic, argNodes.map(a => evalExpr(a.value, ctx)), ctx);
  }
  throw new Error(`unknown action '!${name}(...)'`);
}

function spawnInit(node, ctx) {
  if (node.type === 'Call') {
    // v0.5.1: spawn data is now a NAMED object so `*b in pool: ... b.vel.x` works.
    // - Named args (`Bullet(pos: v3(...), vel: v3(...))`) → obj.pos, obj.vel
    // - Positional args with a declared ^type → mapped to type field names by position
    // - Positional args without a ^type → fall back to legacy `args` array
    const obj = { __type: node.callee };
    const typeDecl = ctx.world.typeDecls.get(node.callee);
    const fields = typeDecl ? typeDecl.fields : [];
    let posIdx = 0;
    for (const a of node.args) {
      const v = evalExpr(a.value, ctx);
      if (a.name) obj[a.name] = v;
      else if (fields[posIdx]) obj[fields[posIdx].name] = v;
      else (obj.args = obj.args || []).push(v);
      posIdx++;
    }
    return obj;
  }
  return evalExpr(node, ctx);
}

function classifyRuntimeError(err) {
  // v0.9.0: errors the new language features raise carry their own code already.
  if (err instanceof AxiomError && err.axiomCode) {
    const code = err.axiomCode;
    if (code === 'AX-DEPTH-001') return { code, human: 'Recursion went too deep.', hint: 'Add a base case, or convert the recursion to a loop. Use memo(fn) if the recursion is re-computing the same arguments.' };
    if (code === 'AX-LOOP-002') return { code, human: 'A loop in a hot block ran past its iteration budget.', hint: 'Frame blocks (&physics/&render/&tick/&on) are capped so one frame cannot hang the program. Move the long computation into a ^fn called from ^main, or bound the loop.' };
    if (code === 'AX-CALL-001') return { code, human: 'Tried to call something that is not a function.', hint: 'Check the value: type(v) reports "fn" for callables. A ^fn name used without () is a function value; a field holding a number is not.' };
    if (code === 'AX-SANDBOX-001') return { code, human: 'The sandbox denied this operation.', hint: 'Pass --allow-read/--allow-write for the path, or --allow-exec for commands. Sandbox mode denies all three by default.' };
    if (code === 'AX-RUNTIME-INDEX') return { code, human: err.message, hint: 'Guard the value first: `?is_null(v):` or `v ?? []`, and use len(v) to check the range. Reading past the end of an array gives null rather than an error.' };
    if (code === 'AX-RUNTIME-METHOD') return { code, human: err.message, hint: 'Check the receiver type with type(v). Array, dict, and string methods are listed in STDLIB.md; a dict field holding a function is callable as a method.' };
    if (code === 'AX-RUNTIME-FUNC') return { code, human: err.message, hint: 'Declare it with ^fn/^proc, import it with ^use, or check the spelling against STDLIB.md.' };
    if (code === 'AX-CHECK') return { code, human: err.message, hint: 'A check()/check_eq() assertion failed. Catch it with ^try:/^catch e: or fix the condition.' };
    if (code === 'AX-EXIT') return { code, human: err.message, hint: 'exit() ended the program; this is not a failure unless the code is non-zero.' };
    return { code, human: err.message, hint: 'Raised by ^throw or by the standard library. Wrap the call in ^try:/^catch e: to handle it.' };
  }
  const m = (err && err.message) || String(err);
  if (/^no method/.test(m)) return {
    code: 'AX-RUNTIME-METHOD', human: 'Called an undefined method.',
    hint: "Check the receiver's runtime type and the spelling of the method name.",
    fix: 'Common methods: Vec3.norm, Vec3.mag, Vec3.reflect(n), Vec3.rotate(q), Vec3.lerp(v,t). BVec.push(v), BVec.get(i), BMap.get(k), BMap.set(k,v). Pool has no methods — iterate with *x in pool.',
  };
  if (/^unknown function/.test(m)) return {
    code: 'AX-RUNTIME-FUNC', human: 'Called an undefined function.',
    hint: 'Check the name against the intrinsics table and user-defined functions.',
    fix: 'Available intrinsics: v2(x,y), v3(x,y,z), q(axis,angle), euler(p,y,r), lookat(eye,tgt,up), dist(a,b), clamp(x,lo,hi), sphere(r), box(v3), capsule(r,h), patrol_point(), bar(pos,w,h,fg). Declare your own with ^fn name(params): body.',
  };
  if (/^unknown action/.test(m)) return {
    code: 'AX-RUNTIME-ACTION', human: 'Called an undefined action.',
    hint: 'Only intrinsic actions and native subsystem declared actions exist.',
    fix: 'Available actions: !play(snd,vol), !music(snd), !play_anim(#Mesh,clip,loop?), !mesh(#Ref,mat/color:), !tween(target,dest,dur,ease), !save(slot), !load(slot), !dbg_line(a,b), !log(msg,level), !d(msg,...), !spawn(#Entity,at:), !despawn(self|#Tag), !move(delta). Body3D: !force(v), !impulse(v).',
  };
  if (/is not a distribution/.test(m)) return {
    code: 'AX-RUNTIME-KERNEL', human: 'Used a stochastic operator on a non-distribution value.',
    hint: "Check that the '~='/'~>' target is actually a '$'-declared field.",
    fix: 'Distributions are declared with $field: shape ~infer: strategy. The ~= and ~> operators only work on $-fields.',
  };
  if (/cannot index/.test(m)) return {
    code: 'AX-RUNTIME-INDEX', human: 'Indexed a value that does not support indexing.',
    hint: 'Only distributions, arrays, BVec, BMap, and dicts support indexing.',
    fix: 'Use path[0] for arrays, vec.get(i) for BVec, map.get(key) for BMap. For Vec3, use .x/.y/.z properties.',
  };
  if (/no entity.*tagged|no entity or resource tagged/.test(m)) return {
    code: 'AX-RUNTIME-TAG', human: 'Referenced an unresolved #tag.',
    hint: 'Check the tag is spelled correctly and that entity exists.',
    fix: 'Tag references like #Player.pos resolve to entity tags. Ensure the entity @Player is declared. For resources, use #Mesh3D Name: "path" and reference as #Name.',
  };
  if (/not a declared query/.test(m)) return {
    code: 'AX-RUNTIME-QUERY', human: 'Called an undeclared query on a subsystem entity.',
    hint: 'Queries are declared on the subsystem. Check the entity has the right base type.',
    fix: 'NavMesh3D queries: ?path(from,to), ?raycast(origin,dir,maxDist). No other subsystems have queries. Ensure the entity has &NavMesh3D as its base type.',
  };
  if (/no member/.test(m)) return {
    code: 'AX-RUNTIME-MEMBER', human: 'Accessed a non-existent member.',
    hint: 'The object does not have the requested property.',
    fix: 'Vec3: .x .y .z .mag .norm. Quat: .euler .conj .normalized. Mat4: .inv .T. Transform: .pos .rot .scl .vel. Entity: .hp .speed etc (your ~fields). Use pose.pos not self.pos (pose is implicit).',
  };
  return {
    code: 'AX-RUNTIME-000', human: 'An unclassified runtime fault occurred.',
    hint: 'See the raw message for details.',
    fix: 'Check the line for syntax errors, undefined references, or type mismatches.',
  };
}

function makeRuntimeFault(entity, block, stmt, err, world) {
  const cls = classifyRuntimeError(err);
  const line = (stmt && stmt.line) || null;
  const col = (stmt && stmt.col) || null;
  const snippet = (world.sourceText && line) ? (world.sourceText.split('\n')[line - 1] || '').trim() : null;
  return {
    error_code: cls.code,
    severity: 'fatal',
    // v0.9.0: entity may be null — ^main, a global initializer, and a ^fn called from the host
    // all run without one.
    location: { entity: entity ? entity.decl.name : null, block: block ? block.name : null, line, col },
    violated_rule: { section: 'runtime', title: 'Runtime Fault' },
    context_snippet: snippet,
    message_for_human: cls.human,
    message_for_agent: `${cls.human} Raw error: "${err.message}". ${cls.hint}`,
    suggested_fix: cls.fix || null,
    auto_fixable: false,
    __axiomRuntimeFault: true,
  };
}

const HOT_BLOCKS = new Set(['physics', 'render', 'tick', 'on']);

function runBlock(block, entity, world, dt, eventPayload) {
  // v0.9.0: each block execution gets its own scope frame (parented to the world globals), and
  // is marked `hot` when it runs on the frame budget so loop caps apply there and only there.
  const ctx = {
    entity, world, dt, blockName: block.name, eventPayload: eventPayload || null,
    scope: new Scope(world.globalScope), inFn: false, hot: HOT_BLOCKS.has(block.name),
  };
  for (const stmt of block.body) {
    try {
      execStmtInner(stmt, ctx);
    } catch (err) {
      if (err instanceof BreakSignal || err instanceof ContinueSignal || err instanceof ReturnSignal) throw err;
      world.runtimeDiagnostics.push(makeRuntimeFault(entity, block, stmt, err, world));
      break;
    }
  }
}

module.exports = {
  Vec2, Vec3, Quat, Mat4, Transform, Atom, atom, Distribution, Pool, BVec, BMap, Range, Tween,
  ColliderShape, ChannelBus, EntityInstance, World,
  NATIVE_SUBSYSTEMS, SUPPORTED_INFER_STRATEGIES,
  evalExpr, binaryOp, execStmt: execStmtInner, execStmtInner, execAction, runBlock, truthy, equalsVal, BreakSignal, ContinueSignal, ReturnSignal,
  // v0.6: collision + raycast exported for test access
  stepCollisions, stepGroundPlane, raycastWorld, raycastCollider, colliderWorld,
  testColliders, sphereSphereMTV, sphereBoxMTV, boxBoxMTV,
  // v0.8.1: capsule collision exported for test access
  capsuleSphereMTV, capsuleBoxMTV, capsuleCapsuleMTV,
  closestPointOnSegment, closestPointsSegmentSegment, refreshCapsuleAxis,
  // v0.6: NavMesh pathfinder exported for test access
  loadNavGrid, aStar, smoothPath, navRaycast,
  // v0.8.1: multi-layer navmesh helpers
  parseSingleLayerNav, parseMultiLayerNav, aStarMultiLayer,
  // v0.6.1: GLB loader exported for test access
  loadGLB, parseGLB, decodeAccessor,
  // v0.7: multi-primitive glb loader
  loadGLBMulti, parseGLBMulti, buildPrimitive,
  // v0.8.1: animation clip + skinning helpers
  parseGLBSkin, parseGLBAnimations, sampleAnimation, buildJointMatrices, trsToMat4, mat4FromFlat,
  // v0.8.1: audio engine helpers exported for test access
  detectAudioPlayer, findListenerPos, distanceAttenuatedVolume,
  killAudioChannel, activeAudioChannels, stopAllAudio,
  // v0.8.1: save/load schema versioning helpers
  computeSaveSchema, compareSaveSchemas, makeSchemaMismatchFault,
};
