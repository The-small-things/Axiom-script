// sjlj.c — the runtime half of clang's WebAssembly setjmp/longjmp lowering (LLVM 18's ABI, the
// same as Emscripten's before 2024): each function that calls setjmp keeps a table of
// (setjmp id → label); longjmp throws a WebAssembly exception carrying the jmp_buf, and the
// catching function asks testSetjmp whether that id is one of its own.
#include <stdint.h>
#include <stdlib.h>

typedef struct { uintptr_t id; uint32_t label; } TableEntry;

static uint32_t temp_ret0;
uint32_t getTempRet0(void) { return temp_ret0; }
void setTempRet0(uint32_t v) { temp_ret0 = v; }

static uintptr_t setjmp_id;

TableEntry *saveSetjmp(uintptr_t *env, uint32_t label, TableEntry *table, uint32_t size) {
  setjmp_id++;
  *env = setjmp_id;
  for (;;) {
    for (uint32_t i = 0; i < size; i++) {
      if (table[i].id == 0) {
        table[i].id = setjmp_id;
        table[i].label = label;
        table[i + 1].id = 0;   // the table always has one slot more than `size`
        setTempRet0(size);
        return table;
      }
    }
    size *= 2;
    table = realloc(table, sizeof(TableEntry) * (size + 1));
  }
}

uint32_t testSetjmp(uintptr_t id, TableEntry *table, uint32_t size) {
  for (uint32_t i = 0; i < size; i++) {
    if (table[i].id == 0) break;
    if (table[i].id == id) return table[i].label;
  }
  return 0;
}

struct WasmLongjmpArgs { void *env; int val; };
static struct WasmLongjmpArgs longjmp_args;

void __wasm_longjmp(void *env, int val) {
  longjmp_args.env = env;
  longjmp_args.val = val == 0 ? 1 : val;
  __builtin_wasm_throw(1, &longjmp_args);   // tag 1: __c_longjmp
}
