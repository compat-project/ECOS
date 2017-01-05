CFLAGS=-Wall -g -DDEBUG -std=gnu99 -I/home/maciej/Praca/Compat/local/include
LDFLAGS=-L/home/maciej/Praca/Compat/local/lib
LIBS=-lm -llikwid
TARGETS=papi perf likwid ecos

.DEFAULT_GOAL := ecos

all: ${TARGETS}

ecos: ecos.o
papi: papi.o
perf: perf.o
likwid: likwid.o

${TARGETS}:
	gcc $(LDFLAGS) -o $@ $< $(LIBS)

%.o: %.c
	gcc $(CFLAGS) -c $<

clean:
	rm -f ${TARGETS} $(addsuffix .o,${TARGETS})
