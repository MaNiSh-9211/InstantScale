# ============================================================================
#  HotPod - one-command Windows test runner
# ----------------------------------------------------------------------------
#  The project targets Linux kernels (userfaultfd), but EVERYTHING is tested
#  from your Windows desktop through Docker Desktop privileged containers.
#
#  Usage (from the repo folder, any shell):
#    powershell -ExecutionPolicy Bypass -File hotpod.ps1 all     # everything
#    powershell -ExecutionPolicy Bypass -File hotpod.ps1 mvp     # Phase 1
#    powershell -ExecutionPolicy Bypass -File hotpod.ps1 p2      # Phase 2 A/B
#    powershell -ExecutionPolicy Bypass -File hotpod.ps1 p3      # Phase 3 lifecycle
#    powershell -ExecutionPolicy Bypass -File hotpod.ps1 hammer  # stress p2
#    powershell -ExecutionPolicy Bypass -File hotpod.ps1 image   # build image only
# ============================================================================
param([Parameter(Position = 0)][string]$Cmd = "all")

# NOTE: PS 5.1 promotes native stderr noise (e.g. docker's blkio warning)
# into terminating errors under "Stop"; we validate via $LASTEXITCODE instead.
$ErrorActionPreference = "Continue"
$root = $PSScriptRoot
$img  = "hotpod-devel"

function Ensure-Docker {
    docker info *> $null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[hotpod] starting Docker Desktop..." -ForegroundColor Cyan
        $dd = "C:\Program Files\Docker\Docker\Docker Desktop.exe"
        if (-not (Test-Path $dd)) { throw "Docker Desktop not found at $dd" }
        Start-Process $dd
        for ($i = 0; $i -lt 60; $i++) {
            Start-Sleep -Seconds 5
            docker info *> $null
            if ($LASTEXITCODE -eq 0) { break }
        }
        docker info *> $null
        if ($LASTEXITCODE -ne 0) { throw "Docker engine did not come up" }
    }
}

function Ensure-Image {
    docker image inspect $img *> $null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[hotpod] building devel image (gcc + CRIU)..." -ForegroundColor Cyan
        docker build -t $img "$root\.dev"
    }
}

function Banner([string]$t) {
    Write-Host ""
    Write-Host ("=== " + $t + " ===") -ForegroundColor Yellow
}

function Lin([string]$c) {
    # privileged: Docker's default seccomp blocks userfaultfd/CRIU helpers
    docker run --rm --privileged -v "${root}:/src" -w /src $img bash -c $c
    if ($LASTEXITCODE -ne 0) { throw "container step failed (exit $LASTEXITCODE)" }
}

Ensure-Docker

switch ($Cmd.ToLower()) {
    "image" { Ensure-Image }
    "mvp" {
        Ensure-Image
        Banner "PHASE 1 : userfaultfd self-faulting prototype"
        Lin 'make -C /src/mvp clean all && /src/mvp/uffd_selffault'
    }
    "p2" {
        Ensure-Image
        Banner "PHASE 2 : split-process page service, LAZY vs EAGER"
        Lin 'make -C /src/phase2 clean all >/dev/null && bash /src/phase2/demo.sh'
    }
    "p3" {
        Ensure-Image
        Banner "PHASE 3 : instant scale-out lifecycle (cold / eager / lazy)"
        Lin 'make -C /src/phase3 clean all >/dev/null && make -C /src/phase2 pageserver >/dev/null && bash /src/phase3/battery.sh'
    }
    "hammer" {
        Ensure-Image
        Banner "STRESS  : phase2 deadlock hammer (10 runs)"
        Lin 'make -C /src/phase2 clean all >/dev/null && bash /src/phase2/hammer.sh 64 10'
    }
    "mh" {
        Ensure-Image
        Banner "PHASE 4 : MULTI-HOST migration (two containers, real network)"
        & powershell -ExecutionPolicy Bypass -File "$root\phase4\multihost.ps1" @args
    }
    "all" {
        Ensure-Image
        Banner "PHASE 1 : userfaultfd self-faulting prototype"
        Lin 'make -C /src/mvp clean all && /src/mvp/uffd_selffault'
        Banner "PHASE 2 : split-process page service, LAZY vs EAGER"
        Lin 'make -C /src/phase2 clean all >/dev/null && bash /src/phase2/demo.sh'
        Banner "PHASE 3 : instant scale-out lifecycle (cold / eager / lazy)"
        Lin 'make -C /src/phase3 clean all >/dev/null && make -C /src/phase2 pageserver >/dev/null && bash /src/phase3/battery.sh'
        Write-Host ""
        Write-Host "[hotpod] ALL PHASES GREEN" -ForegroundColor Green
    }
    default {
        Write-Host "usage: hotpod.ps1 [image|mvp|p2|p3|hammer|mh|all]"
        exit 2
    }
}
