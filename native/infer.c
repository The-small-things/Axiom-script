// infer.c — `$` distributions, NavMesh3D queries, and !save/!load (placeholder until ported).
#include "axiom.h"
#include <stdlib.h>
AxValue ax_dist_new(AxVM *vm, AxNode *field) { ax_throw(vm, "AX-RUNTIME-000", "distributions are not yet ported"); return ax_null(); }
void ax_dist_free(void *d) { free(d); }
AxValue ax_dist_infer(AxVM *vm, AxValue dist, AxStr *op) { ax_throw(vm, "AX-RUNTIME-000", "'~> %s' used on a non-distribution value", op->data); return ax_null(); }
void ax_dist_observe(AxVM *vm, AxNode *n, AxScope *scope) { ax_throw(vm, "AX-RUNTIME-KERNEL", "'~=' target '%s' is not a distribution", n->str->data); }
AxValue ax_dist_mass_at(AxVM *vm, AxHost *h, AxValue cell) { return ax_num(0); }
bool ax_dist_member(AxVM *vm, AxHost *h, AxStr *prop, AxValue *out) { *out = ax_null(); return true; }
bool ax_dist_method(AxVM *vm, AxHost *h, AxStr *name, AxValue *args, int argc, AxValue *out) { return false; }
void ax_dist_json(AxHost *h, void *jbp, int indent, int depth, bool sim) {}
AxValue ax_nav_query(AxVM *vm, AxEntity *nav, AxStr *name, AxValue *args, int argc) { return ax_null(); }
void ax_save_action(AxVM *vm, AxNode *n, AxScope *scope, bool load) {}
