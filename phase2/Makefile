# InstantScale Phase 2 — split-process page service
CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wpedantic -std=gnu11

BINS := seeder pageserver restorer

all: $(BINS)

seeder: seeder.c common.h
	$(CC) $(CFLAGS) -o $@ $<

pageserver: pageserver.c common.h
	$(CC) $(CFLAGS) -pthread -o $@ $<

restorer: restorer.c common.h
	$(CC) $(CFLAGS) -pthread -o $@ $<

# Full story: seed -> serve -> lazy activation vs eager migration A/B
demo: all
	bash demo.sh

clean:
	rm -f $(BINS)

.PHONY: all demo clean
