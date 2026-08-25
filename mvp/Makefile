# InstantScale Phase 1 MVP — build & test
CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wpedantic -std=gnu11
LDLIBS   = -pthread

BIN := uffd_selffault

all: $(BIN)

$(BIN): uffd_selffault.c
	$(CC) $(CFLAGS) $(LDLIBS) -o $@ $<

# Run the acceptance criteria demo (Linux host or WSL2)
run: $(BIN)
	./$(BIN)

# Same run with a simulated 500us network RTT per page (closer to reality)
run-sim: $(BIN)
	IS_SIM_LATENCY_US=500 ./$(BIN)

# Build + execute inside a privileged Ubuntu container (for Windows/macOS dev
# boxes). --privileged is needed because Docker's default seccomp profile
# blocks userfaultfd; on real Linux hosts prefer running ./uffd_selffault
# directly after 'sysctl vm.unprivileged_userfaultfd=1'.
docker-test:
	docker run --rm --privileged -v "$$PWD":/src -w /src gcc:14 \
		bash -c "make clean all && ./uffd_selffault"

clean:
	rm -f $(BIN)

.PHONY: all run run-sim docker-test clean
