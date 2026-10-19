// setjmp.h for the WebAssembly build (wasi-libc has none). Compiled with
// `-mllvm -wasm-enable-sjlj -mexception-handling`, clang lowers setjmp/longjmp onto WebAssembly
// exception handling and calls the helpers in sjlj.c. The first word of a jmp_buf holds the
// setjmp's id.
#ifndef AXIOM_WASM_SETJMP_H
#define AXIOM_WASM_SETJMP_H
#include <stdint.h>
typedef uintptr_t jmp_buf[4];
int setjmp(jmp_buf env);
_Noreturn void longjmp(jmp_buf env, int val);
#endif
