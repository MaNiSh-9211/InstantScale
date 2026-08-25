# InstantScale Phase 3 â€” real CRIU integration
CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wpedantic -std=gnu11

BINS := demo_app analyze

all: $(BINS)

demo_app: demo_app.c
	$(CC) $(CFLAGS) -o $@ $<

analyze: analyze.c ../phase2/common.h
	$(CC) $(CFLAGS) -I../phase2 -o $@ $<

# Full migration story: cold vs eager vs lazy + continuity proofs
migrate: all
	bash hotpod.sh all

analyze-images: all
	bash hotpod.sh eager >/dev/null 2>&1 || true; \
	./analyze /tmp/hotpod.*/imgs_t 2>/dev/null || true

clean:
	rm -f $(BINS)

.PHONY: all migrate clean
