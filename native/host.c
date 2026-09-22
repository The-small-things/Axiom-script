// host.c — the engine's container and inference objects: &Pool, &Vec, &Map fields and `$`
// distributions, plus the NavMesh3D queries and !save/!load.
//
// Each follows its class in interpreter.js (Pool, BVec, BMap, Distribution, loadNavGrid, …).
// They are `AX_HOST` values: a kind tag and a payload, so the core value model does not need a
// type per container.

#include "axiom.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

enum { H_POOL = 0, H_BVEC = 1, H_BMAP = 2, H_DIST = 3 };

typedef struct { AxStr *elem; int capacity; bool *used; AxValue *data; } Pool;
typedef struct { AxStr *elem; int capacity; AxArr *data; } BVec;
typedef struct { AxStr *ktype, *vtype; int capacity; AxDict *data; } BMap;

static AxStr *S(const char *s) { return ax_internz(s); }
static AxValue strv(AxStr *s) { ax_retain(ax_strv(s)); return ax_strv(s); }

static AxValue host_new(int kind, void *data) {
  AxHost *h = calloc(1, sizeof(AxHost));
  h->hdr.rc = 1;
  h->hdr.type = AX_HOST;
  h->kind = kind;
  h->data = data;
  AxValue v; v.t = AX_HOST; v.o = (AxObj *)h;
  return v;
}

extern void ax_dist_free(void *d);

void ax_host_free(AxHost *h) {
  switch (h->kind) {
    case H_POOL: {
      Pool *p = h->data;
      for (int i = 0; i < p->capacity; i++) ax_release(p->data[i]);
      free(p->used); free(p->data); free(p);
      break;
    }
    case H_BVEC: { BVec *b = h->data; ax_release(ax_arrv(b->data)); free(b); break; }
    case H_BMAP: { BMap *m = h->data; ax_release(ax_dictv(m->data)); free(m); break; }
    case H_DIST: ax_dist_free(h->data); break;
  }
  free(h);
}

extern AxValue ax_dist_new(AxVM *vm, AxNode *field);

// A field's initial value when it is a container or a distribution.
AxValue ax_host_from_field(AxVM *vm, AxNode *field, AxEntity *e) {
  (void)e;
  if (field->op == 1) return ax_dist_new(vm, field);
  AxNode *t = field->a;
  int cap = (int)lround(t->num);
  if (cap < 0) cap = 0;
  if (!strcmp(t->str->data, "Pool")) {
    Pool *p = calloc(1, sizeof(Pool));
    p->elem = t->str2;
    p->capacity = cap;
    p->used = calloc(cap + 1, sizeof(bool));
    p->data = calloc(cap + 1, sizeof(AxValue));
    return host_new(H_POOL, p);
  }
  if (!strcmp(t->str->data, "Vec")) {
    BVec *b = calloc(1, sizeof(BVec));
    b->elem = t->str2;
    b->capacity = cap;
    b->data = ax_arr_new(8);
    return host_new(H_BVEC, b);
  }
  BMap *m = calloc(1, sizeof(BMap));
  m->ktype = t->str2;
  m->vtype = t->nnames ? t->names[0] : NULL;
  m->capacity = cap;
  m->data = ax_dict_new();
  return host_new(H_BMAP, m);
}

static AxHost *as_host(AxValue v, int kind) {
  if (v.t != AX_HOST) return NULL;
  AxHost *h = (AxHost *)v.o;
  return h->kind == kind ? h : NULL;
}

// ---- Pool ------------------------------------------------------------------------------------

static AxStr *K__type, *K__handle, *K_index, *K_args;
static void keys(void) {
  if (K__type) return;
  K__type = S("__type"); K__handle = S("__handle"); K_index = S("index"); K_args = S("args");
}

static int pool_live(Pool *p) { int n = 0; for (int i = 0; i < p->capacity; i++) n += p->used[i]; return n; }

// spawnInit: `Bullet(pos: …, vel: …)` builds a named record; positional arguments follow the
// ^type's field order, and any beyond it collect in `args`.
static AxValue spawn_init(AxVM *vm, AxNode *node, AxScope *scope) {
  keys();
  if (node->kind != N_CALL) return ax_eval(vm, node, scope);
  AxDict *obj = ax_dict_new();
  obj->type_tag = node->str;
  ax_retain(ax_strv(node->str));
  AxValue fields;
  AxArr *fnames = NULL;
  if (ax_dict_get(vm->types, node->str, &fields)) fnames = (AxArr *)fields.o;
  int pos = 0;
  for (int i = 0; i < node->nlist; i++) {
    AxNode *a = node->list[i];
    AxValue v = ax_eval(vm, a->b, scope);
    if (a->str) ax_dict_set(obj, a->str, v);
    else if (fnames && pos < (int)fnames->len) ax_dict_set(obj, (AxStr *)fnames->items[pos].o, v);
    else {
      AxValue arr;
      if (!ax_dict_get(obj, K_args, &arr)) { arr = ax_arrv(ax_arr_new(4)); ax_dict_set(obj, K_args, ax_copy(arr)); }
      ax_arr_push((AxArr *)arr.o, v);
      ax_release(arr);
    }
    pos++;
  }
  if (fnames) ax_release(fields);
  return ax_dictv(obj);
}

AxValue ax_pool_spawn(AxVM *vm, AxNode *n, AxScope *scope) {
  keys();
  AxValue pv = n->nlist ? ax_eval(vm, n->list[0]->b, scope) : ax_null();
  AxHost *h = as_host(pv, H_POOL);
  if (!h) { ax_release(pv); ax_throw(vm, "AX-RUNTIME-000", "'!spawn(...)': first argument must be a '&Pool' field or a #TagRef"); }
  AxValue data = n->nlist > 1 ? spawn_init(vm, n->list[1]->b, scope) : ax_null();
  Pool *p = h->data;
  AxValue out = ax_null();
  for (int i = 0; i < p->capacity; i++) {
    if (p->used[i]) continue;
    p->used[i] = true;
    ax_release(p->data[i]);
    p->data[i] = data;
    AxDict *hd = ax_dict_new();
    ax_dict_set(hd, K__handle, ax_bool(true));
    ax_dict_set(hd, K_index, ax_num(i));
    out = ax_dictv(hd);
    data = ax_null();
    break;
  }
  ax_release(data);
  ax_release(pv);
  return out;
}

void ax_pool_release_action(AxVM *vm, AxNode *n, AxScope *scope) {
  keys();
  AxValue pv = n->nlist ? ax_eval(vm, n->list[0]->b, scope) : ax_null();
  AxHost *h = as_host(pv, H_POOL);
  if (!h) { ax_release(pv); ax_throw(vm, "AX-RUNTIME-000", "'!release(...)': first argument must be a '&Pool' field"); }
  AxValue hv = n->nlist > 1 ? ax_eval(vm, n->list[1]->b, scope) : ax_null();
  if (hv.t == AX_DICT) {
    AxValue flag, idx;
    if (ax_dict_get((AxDict *)hv.o, K__handle, &flag)) {
      if (ax_truthy(flag) && ax_dict_get((AxDict *)hv.o, K_index, &idx)) {
        Pool *p = h->data;
        int i = (int)ax_to_num(idx);
        if (i >= 0 && i < p->capacity) { p->used[i] = false; ax_release(p->data[i]); p->data[i] = ax_null(); }
        ax_release(idx);
      }
      ax_release(flag);
    }
  }
  ax_release(hv);
  ax_release(pv);
}

// ---- Shared surface ---------------------------------------------------------------------------

// Iteration order: a pool yields its live items, a bounded vector its items, a bounded map its
// keys.
AxArr *ax_host_seq(AxValue v) {
  AxHost *h = (AxHost *)v.o;
  AxArr *out = ax_arr_new(8);
  if (h->kind == H_POOL) {
    Pool *p = h->data;
    for (int i = 0; i < p->capacity; i++) if (p->used[i]) ax_arr_push(out, ax_copy(p->data[i]));
  } else if (h->kind == H_BVEC) {
    BVec *b = h->data;
    for (uint32_t i = 0; i < b->data->len; i++) ax_arr_push(out, ax_copy(b->data->items[i]));
  } else if (h->kind == H_BMAP) {
    BMap *m = h->data;
    for (uint32_t i = 0; i < m->data->len; i++) {
      if (m->data->entries[i].dead) continue;
      ax_arr_push(out, strv(m->data->entries[i].key));
    }
  }
  return out;
}

double ax_host_len(AxValue v) {
  AxHost *h = (AxHost *)v.o;
  if (h->kind == H_POOL) return pool_live(h->data);
  if (h->kind == H_BVEC) return ((BVec *)h->data)->data->len;
  if (h->kind == H_BMAP) return ax_dict_count(((BMap *)h->data)->data);
  return 0;
}

bool ax_host_contains(AxVM *vm, AxValue hay, AxValue needle) {
  AxHost *h = (AxHost *)hay.o;
  if (h->kind == H_BVEC) {
    AxArr *a = ((BVec *)h->data)->data;
    for (uint32_t i = 0; i < a->len; i++) if (ax_equals(a->items[i], needle)) return true;
    return false;
  }
  if (h->kind == H_BMAP) {
    AxStr *k = ax_to_str(needle);
    bool r = ax_dict_has(((BMap *)h->data)->data, k);
    ax_release(ax_strv(k));
    return r;
  }
  return false;
}

static AxValue bvec_get(BVec *b, AxValue idx) {
  if (idx.t != AX_NUM) return ax_null();
  double d = idx.num;
  if (d != floor(d) || d < 0 || d >= b->data->len) return ax_null();
  return ax_copy(b->data->items[(uint32_t)d]);
}

static AxValue bmap_get(BMap *m, AxValue k) {
  AxStr *ks = ax_to_str(k);
  AxValue out;
  if (!ax_dict_get(m->data, ks, &out)) out = ax_null();
  ax_release(ax_strv(ks));
  return out;
}

static bool bmap_set(BMap *m, AxValue k, AxValue v) {   // takes v
  AxStr *ks = ax_to_str(k);
  bool ok = ax_dict_has(m->data, ks) || (int)ax_dict_count(m->data) < m->capacity;
  if (ok) ax_dict_set(m->data, ks, v); else ax_release(v);
  ax_release(ax_strv(ks));
  return ok;
}

AxValue ax_host_index(AxVM *vm, AxValue obj, AxValue idx) {
  AxHost *h = (AxHost *)obj.o;
  if (h->kind == H_BVEC) return bvec_get(h->data, idx);
  if (h->kind == H_BMAP) return bmap_get(h->data, idx);
  extern AxValue ax_dist_mass_at(AxVM *vm, AxHost *h, AxValue cell);
  if (h->kind == H_DIST) return ax_dist_mass_at(vm, h, idx);
  ax_throw(vm, "AX-RUNTIME-INDEX", "cannot index Pool with [%s]", "?");
  return ax_null();
}

int ax_engine_index_assign(AxVM *vm, AxValue target, AxValue idx, AxValue v, int op) {
  AxHost *h = (AxHost *)target.o;
  if (op != OP_NONE) {
    AxValue cur = ax_host_index(vm, target, idx);
    if (op == OP_COALESCE) {
      if (cur.t != AX_NULL) { ax_release(cur); ax_release(v); return AX_FLOW_NORMAL; }
      ax_release(cur);
    } else {
      AxValue c = ax_binary_op(vm, op, cur, v);
      ax_release(cur);
      ax_release(v);
      v = c;
    }
  }
  if (h->kind == H_BVEC) {
    BVec *b = h->data;
    double d = ax_to_num(idx);
    if (d >= 0 && d < b->data->len && d == floor(d)) ax_arr_set(b->data, (int64_t)d, v);
    else ax_release(v);
    return AX_FLOW_NORMAL;
  }
  if (h->kind == H_BMAP) { bmap_set(h->data, idx, v); return AX_FLOW_NORMAL; }
  ax_release(v);
  ax_throw(vm, "AX-RUNTIME-INDEX", "cannot assign into a Pool by index");
  return AX_FLOW_NORMAL;
}

bool ax_host_member(AxVM *vm, AxHost *h, AxStr *prop, AxValue *out) {
  const char *p = prop->data;
  *out = ax_null();
  if (h->kind == H_POOL) {
    Pool *pl = h->data;
    if (!strcmp(p, "liveCount")) *out = ax_num(pool_live(pl));
    else if (!strcmp(p, "capacity")) *out = ax_num(pl->capacity);
    else if (!strcmp(p, "elementType")) *out = strv(pl->elem);
    return true;
  }
  if (h->kind == H_BVEC) {
    BVec *b = h->data;
    if (!strcmp(p, "len")) *out = ax_num(b->data->len);
    else if (!strcmp(p, "capacity")) *out = ax_num(b->capacity);
    else if (!strcmp(p, "data")) *out = ax_copy(ax_arrv(b->data));
    return true;
  }
  if (h->kind == H_BMAP) {
    BMap *m = h->data;
    if (!strcmp(p, "len")) *out = ax_num(ax_dict_count(m->data));
    else if (!strcmp(p, "capacity")) *out = ax_num(m->capacity);
    return true;
  }
  extern bool ax_dist_member(AxVM *vm, AxHost *h, AxStr *prop, AxValue *out);
  if (h->kind == H_DIST) return ax_dist_member(vm, h, prop, out);
  return true;
}

// BVec.map/filter take the NAME of a declared ^fn (the reference passes the argument node, and
// applies it only when it is a bare function name); anything else returns the items unchanged.
static bool named_fn(AxValue f) { return f.t == AX_FN && !((AxFn *)f.o)->native && !((AxFn *)f.o)->is_expr; }

bool ax_host_method(AxVM *vm, AxHost *h, AxStr *name, AxValue *args, int argc, AxValue *out) {
  const char *m = name->data;
  AxValue a0 = argc > 0 ? args[0] : ax_null(), a1 = argc > 1 ? args[1] : ax_null();
  if (h->kind == H_BVEC) {
    BVec *b = h->data;
    if (!strcmp(m, "get")) { *out = bvec_get(b, a0); return true; }
    if (!strcmp(m, "set")) {
      double d = ax_to_num(a0);
      if (d >= 0 && d < b->data->len && d == floor(d)) ax_arr_set(b->data, (int64_t)d, ax_copy(a1));
      *out = ax_null();
      return true;
    }
    if (!strcmp(m, "push")) {
      bool ok = (int)b->data->len < b->capacity;
      if (ok) ax_arr_push(b->data, ax_copy(a0));
      *out = ax_bool(ok);
      return true;
    }
    if (!strcmp(m, "pop")) {
      if (!b->data->len) { *out = ax_null(); return true; }
      *out = b->data->items[--b->data->len];
      return true;
    }
    if (!strcmp(m, "clear")) {
      for (uint32_t i = 0; i < b->data->len; i++) ax_release(b->data->items[i]);
      b->data->len = 0;
      *out = ax_null();
      return true;
    }
    if (!strcmp(m, "map") || !strcmp(m, "filter")) {
      if (!named_fn(a0)) { *out = ax_copy(ax_arrv(b->data)); return true; }
      AxArr *res = ax_arr_new(b->data->len);
      for (uint32_t i = 0; i < b->data->len; i++) {
        AxValue r = ax_call(vm, a0, &b->data->items[i], 1);
        if (m[0] == 'm') ax_arr_push(res, r);
        else { if (ax_truthy(r)) ax_arr_push(res, ax_copy(b->data->items[i])); ax_release(r); }
      }
      *out = ax_arrv(res);
      return true;
    }
    return false;
  }
  if (h->kind == H_BMAP) {
    BMap *mp = h->data;
    if (!strcmp(m, "get")) { *out = bmap_get(mp, a0); return true; }
    if (!strcmp(m, "set")) { *out = ax_bool(bmap_set(mp, a0, ax_copy(a1))); return true; }
    if (!strcmp(m, "has")) {
      AxStr *k = ax_to_str(a0);
      *out = ax_bool(ax_dict_has(mp->data, k));
      ax_release(ax_strv(k));
      return true;
    }
    if (!strcmp(m, "inc")) {
      AxStr *k = ax_to_str(a0);
      double by = argc > 1 ? ax_to_num(a1) : 1;
      if (!ax_dict_has(mp->data, k) && (int)ax_dict_count(mp->data) >= mp->capacity) { ax_release(ax_strv(k)); *out = ax_null(); return true; }
      AxValue cur;
      double c = 0;
      if (ax_dict_get(mp->data, k, &cur)) { c = ax_truthy(cur) ? ax_to_num(cur) : 0; ax_release(cur); }
      ax_dict_set(mp->data, k, ax_num(c + by));
      ax_release(ax_strv(k));
      *out = ax_num(c + by);
      return true;
    }
    if (!strcmp(m, "clear")) {
      ax_release(ax_dictv(mp->data));
      mp->data = ax_dict_new();
      *out = ax_null();
      return true;
    }
    return false;
  }
  extern bool ax_dist_method(AxVM *vm, AxHost *h, AxStr *name, AxValue *args, int argc, AxValue *out);
  if (h->kind == H_DIST) return ax_dist_method(vm, h, name, args, argc, out);
  return false;
}

// JSON of a container, as JSON.stringify sees the reference's class instance.
typedef struct { char *buf; size_t len, cap; } JB;
extern void ax_json_value_into(void *jb, AxValue v, int indent, int depth, bool sim);
static void jb(JB *b, const char *s) { ax_str_append(&b->buf, &b->len, &b->cap, s, strlen(s)); }
static void pad(JB *b, int indent, int depth) { if (!indent) return; jb(b, "\n"); for (int i = 0; i < indent * depth; i++) jb(b, " "); }
static void key(JB *b, const char *k, int indent) { jb(b, "\""); jb(b, k); jb(b, indent ? "\": " : "\":"); }

void ax_host_json(AxHost *h, void *jbp, int indent, int depth, bool sim) {
  JB *b = jbp;
  if (h->kind == H_POOL) {
    Pool *p = h->data;
    jb(b, "{");
    pad(b, indent, depth + 1); key(b, "elementType", indent); ax_json_value_into(b, ax_strv(p->elem), indent, depth + 1, sim); jb(b, ",");
    pad(b, indent, depth + 1); key(b, "capacity", indent); ax_json_value_into(b, ax_num(p->capacity), indent, depth + 1, sim); jb(b, ",");
    pad(b, indent, depth + 1); key(b, "slots", indent);
    if (!p->capacity) jb(b, "[]");
    else {
      jb(b, "[");
      for (int i = 0; i < p->capacity; i++) {
        if (i) jb(b, ",");
        pad(b, indent, depth + 2);
        jb(b, "{");
        pad(b, indent, depth + 3); key(b, "used", indent); jb(b, p->used[i] ? "true" : "false"); jb(b, ",");
        pad(b, indent, depth + 3); key(b, "data", indent); ax_json_value_into(b, p->data[i], indent, depth + 3, sim);
        pad(b, indent, depth + 2);
        jb(b, "}");
      }
      pad(b, indent, depth + 1);
      jb(b, "]");
    }
    pad(b, indent, depth);
    jb(b, "}");
    return;
  }
  if (h->kind == H_BVEC) {
    BVec *v = h->data;
    jb(b, "{");
    pad(b, indent, depth + 1); key(b, "elementType", indent); ax_json_value_into(b, ax_strv(v->elem), indent, depth + 1, sim); jb(b, ",");
    pad(b, indent, depth + 1); key(b, "capacity", indent); ax_json_value_into(b, ax_num(v->capacity), indent, depth + 1, sim); jb(b, ",");
    pad(b, indent, depth + 1); key(b, "data", indent); ax_json_value_into(b, ax_arrv(v->data), indent, depth + 1, sim);
    pad(b, indent, depth);
    jb(b, "}");
    return;
  }
  if (h->kind == H_BMAP) {
    BMap *m = h->data;
    jb(b, "{");
    pad(b, indent, depth + 1); key(b, "keyType", indent); ax_json_value_into(b, ax_strv(m->ktype), indent, depth + 1, sim); jb(b, ",");
    pad(b, indent, depth + 1); key(b, "valType", indent); ax_json_value_into(b, m->vtype ? ax_strv(m->vtype) : ax_null(), indent, depth + 1, sim); jb(b, ",");
    pad(b, indent, depth + 1); key(b, "capacity", indent); ax_json_value_into(b, ax_num(m->capacity), indent, depth + 1, sim); jb(b, ",");
    pad(b, indent, depth + 1); key(b, "data", indent); jb(b, "{}");   // a JS Map has no own enumerable keys
    pad(b, indent, depth);
    jb(b, "}");
    return;
  }
  extern void ax_dist_json(AxHost *h, void *jbp, int indent, int depth, bool sim);
  if (h->kind == H_DIST) { ax_dist_json(h, jbp, indent, depth, sim); return; }
  jb(b, "null");
}
