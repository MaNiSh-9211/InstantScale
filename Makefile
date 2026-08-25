# HotPod â€” top-level build & test orchestration
#
# PRIMARY PATH ON WINDOWS:  .\hotpod.ps1   (wraps everything below)
# This Makefile remains for CI/Linux hosts.

P3_ENV := docker run --rm --privileged -v "$${PWD}:/src" -w /src hotpod-devel

.PHONY: all clean test-mvp test-p2 test-p3 docker-test

all:
	@echo "Linux hosts : make -C mvp && make -C phase2 && make -C phase3"
	@echo "Windows     : powershell -ExecutionPolicy Bypass -File hotpod.ps1 all"

test-mvp:
	docker run --rm --privileged -v "$${PWD}:/src" -w /src gcc:14 \
		bash -c 'make -C /src/mvp clean all && /src/mvp/uffd_selffault'

test-p2:
	$(P3_ENV) bash -c 'make -C /src/phase2 clean all && bash /src/phase2/demo.sh'

test-p3:
	$(P3_ENV) bash -c 'make -C /src/phase3 clean all && \
		make -C /src/phase2 pageserver && bash /src/phase3/battery.sh'

docker-test: test-mvp test-p2 test-p3

clean:
	-make -C mvp clean
	-make -C phase2 clean
	-make -C phase3 clean
