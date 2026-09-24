# AxiomScript native runtime.
#
# No dependencies beyond libc and libm: the point of the native build is a single binary that
# runs a program anywhere, with no toolchain, no package manager and no runtime to install.

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
LDFLAGS ?=
LDLIBS  ?= -lm

SRCS = value.c lexer.c parser.c interp.c stdlib.c engine.c host.c infer.c jsmath.c term.c kbd.c render.c fmt.c repl.c main.c
OBJS = $(SRCS:.c=.o)
BIN  = axiom

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c axiom.h jsmath.h term.h render.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Debug build with the sanitizers on: how the memory discipline is actually checked.
debug: CFLAGS = -std=c11 -g -O0 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -fsanitize=address,undefined
debug: LDFLAGS = -fsanitize=address,undefined
debug: clean $(BIN)

test: $(BIN)
	./difftest.sh

clean:
	rm -f $(OBJS) $(BIN)

.PHONY: all clean test debug
