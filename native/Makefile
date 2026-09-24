# AxiomScript native runtime.
#
# No dependencies beyond libc and libm: the point of the native build is a single binary that
# runs a program anywhere, with no toolchain, no package manager and no runtime to install.

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
LDFLAGS ?=
LDLIBS  ?= -lm

# The runtime; the command is main.c on top, an embedding host links libaxiom.a (axiom_api.h).
LIB_SRCS = value.c lexer.c parser.c interp.c stdlib.c engine.c host.c infer.c jsmath.c term.c kbd.c render.c fmt.c glb.c check.c repl.c imports.c api.c
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

clean:
	rm -f $(OBJS) $(BIN) $(LIB) tests/api/apitest

.PHONY: all clean test debug apitest
