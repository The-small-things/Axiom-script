# AxiomScript native runtime.
#
# No dependencies beyond libc and libm: the point of the native build is a single binary that
# runs a program anywhere, with no toolchain, no package manager and no runtime to install.

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
LDFLAGS ?=
LDLIBS  ?= -lm

# The runtime; the command is main.c on top, an embedding host links libaxiom.a (axiom_api.h).
LIB_SRCS = value.c lexer.c parser.c interp.c stdlib.c engine.c host.c infer.c jsmath.c term.c kbd.c render.c fmt.c glb.c check.c repl.c imports.c api.c bigint.c
SRCS = $(LIB_SRCS) main.c
OBJS = $(SRCS:.c=.o)
LIB_OBJS = $(LIB_SRCS:.c=.o)
BIN  = axiom
LIB  = libaxiom.a

all: $(BIN) $(LIB)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(LIB): $(LIB_OBJS)
	rm -f $@
	$(AR) rcs $@ $(LIB_OBJS)

%.o: %.c axiom.h axiom_api.h jsmath.h term.h render.h checknames.h
	$(CC) $(CFLAGS) -c -o $@ $<

# The embedding API's own test: a C host driving every entry point.
tests/api/apitest: tests/api/apitest.c $(LIB)
	$(CC) $(CFLAGS) -I. -o $@ $< $(LIB) $(LDLIBS)

apitest: tests/api/apitest
	./tests/api/apitest

# Debug build with the sanitizers on: how the memory discipline is actually checked.
debug: CFLAGS = -std=c11 -g -O0 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -fsanitize=address,undefined
debug: LDFLAGS = -fsanitize=address,undefined
debug: clean $(BIN)

test: $(BIN)
	./difftest.sh

# WebAssembly (wasm32-wasi; clang with the wasi-libc sysroot). setjmp/longjmp, which errors
# unwind with, become WebAssembly exceptions (wasm/sjlj.c is their runtime half).
#   wasm/axiom.wasm       the command — node wasm/axiom-wasi.js prog.ax [flags]
#   wasm/axiom-lib.wasm   the embedding API (axiom_api.h) — wasm/axiom.mjs wraps it for JS
WASI_CC     ?= clang
WASI_CFLAGS ?= --target=wasm32-wasi -O2 -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
               -mllvm -wasm-enable-sjlj -mexception-handling -Iwasm/include
WASI_LDFLAGS = --target=wasm32-wasi -mexception-handling -Wl,-z,stack-size=8388608
WASM_OBJS = $(patsubst %.c,wasm/obj/%.o,$(LIB_SRCS)) wasm/obj/sjlj.o
API_EXPORTS = axiom_version axiom_new axiom_free axiom_set_args axiom_sandbox axiom_allow_read \
              axiom_allow_write axiom_allow_exec axiom_host_error axiom_check axiom_load axiom_main \
              axiom_run axiom_result axiom_set_input axiom_step axiom_state axiom_exit_code \
              axiom_render axiom_eval axiom_error axiom_release axiom_js_new axiom_js_define \
              axiom_js_alloc axiom_js_eval_value
AXIOM_WASM_JS_EXPORTS = $(foreach f,$(API_EXPORTS),-Wl,--export=$(f))

wasm: wasm/axiom.wasm wasm/axiom-lib.wasm

wasm/obj/%.o: %.c axiom.h axiom_api.h jsmath.h term.h render.h checknames.h
	@mkdir -p wasm/obj
	$(WASI_CC) $(WASI_CFLAGS) -c -o $@ $<

wasm/obj/sjlj.o: wasm/sjlj.c
	@mkdir -p wasm/obj
	$(WASI_CC) $(WASI_CFLAGS) -c -o $@ $<

wasm/obj/api_js.o: wasm/api_js.c axiom_api.h
	@mkdir -p wasm/obj
	$(WASI_CC) $(WASI_CFLAGS) -I. -c -o $@ $<

wasm/axiom.wasm: $(WASM_OBJS) wasm/obj/main.o
	$(WASI_CC) $(WASI_LDFLAGS) -o $@ $(WASM_OBJS) wasm/obj/main.o -lm

wasm/axiom-lib.wasm: $(WASM_OBJS) wasm/obj/api_js.o
	$(WASI_CC) $(WASI_LDFLAGS) -mexec-model=reactor $(AXIOM_WASM_JS_EXPORTS) -o $@ $(WASM_OBJS) wasm/obj/api_js.o -lm

clean:
	rm -f $(OBJS) $(BIN) $(LIB) tests/api/apitest
	rm -rf wasm/obj wasm/axiom.wasm wasm/axiom-lib.wasm

.PHONY: all clean test debug apitest wasm
