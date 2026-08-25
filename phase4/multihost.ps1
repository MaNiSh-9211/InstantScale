# ============================================================================
#  HotPod Phase 4 - MULTI-HOST MIGRATION ORCHESTRATOR (Windows)
# ----------------------------------------------------------------------------
#  Two containers on a private docker bridge act as two hosts:
#    hotpod-src : boots warm app, checkpoints, serves pages on :46100
#    target     : lazy-resumes; its pages arrive across the REAL network
#
#  Usage:
#    powershell -ExecutionPolicy Bypass -File phase4\multihost.ps1
#      [-HeapMB 32] [-InitMs 1200] [-Mode lazy|eager|both]
#
#  Style: no subexpressions inside strings, no line continuations.
# ============================================================================

param(
    [int]$HeapMB = 32,
    [int]$InitMs = 1200,
    [ValidateSet("lazy","eager","both")][string]$Mode = "lazy"
)

$ErrorActionPreference = "Continue"
$root  = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$img   = "hotpod-devel"
$net   = "hotpod-net"
$src   = "hotpod-src"
$port  = "46100"

function Info([string]$m) { Write-Host ("[mh] " + $m) -ForegroundColor Cyan }
function Good ([string]$m) { Write-Host ("[mh] " + $m) -ForegroundColor Green }
function Bad ([string]$m) { Write-Host ("[mh] " + $m) -ForegroundColor Red }
function Sum ([string]$m) { Write-Host ("[mh] " + $m) -ForegroundColor Yellow }

# --- engine -----------------------------------------------------------------
docker info *> $null
if ($LASTEXITCODE -ne 0) {
    Info "starting Docker Desktop..."
    Start-Process "C:\Program Files\Docker\Docker\Docker Desktop.exe"
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep 5
        docker info *> $null
        if ($LASTEXITCODE -eq 0) { break }
    }
}

# --- image ------------------------------------------------------------------
docker image inspect $img *> $null
if ($LASTEXITCODE -ne 0) {
    docker build -t $img (Join-Path $root ".dev")
}

# --- network ----------------------------------------------------------------
docker network ls | Out-Null
docker network inspect $net *> $null
if ($LASTEXITCODE -ne 0) {
    docker network create $net | Out-Null
}

# --- artifacts --------------------------------------------------------------
$artDir = Join-Path $root "artifacts"
New-Item -ItemType Directory -Force -Path $artDir | Out-Null
Get-ChildItem $artDir -ErrorAction SilentlyContinue |
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue

# --- fresh source node ------------------------------------------------------
docker rm -f $src *> $null

Info ("booting source host '" + $src + "' heap=" + $HeapMB + "MB init=" + $InitMs + "ms")

$bind = $root + ":/src"
$srcArgs = @(
    "run", "-d",
    "--name", $src,
    "--network", $net,
    "-v", $bind,
    "-w", "/src",
    "-e", ("INIT_MS=" + $InitMs),
    "-e", ("HEAP_MB=" + $HeapMB),
    "-e", ("PORT=" + $port),
    $img, "bash", "/src/phase4/source_node.sh"
)
docker @srcArgs | Out-Null

# --- wait for checkpoint ----------------------------------------------------
$ready   = Join-Path $artDir "checkpoint.ready"
$imgFile = Join-Path $artDir "app.isim"
$ok = $false
for ($i = 0; $i -lt 240; $i++) {
    if ((Test-Path $ready) -and (Test-Path $imgFile)) { $ok = $true; break }
    Start-Sleep -Milliseconds 250
}
if (-not $ok) {
    Bad "TIMEOUT waiting for source checkpoint. Source logs:"
    docker logs $src | Select-Object -Last 12
    docker rm -f $src | Out-Null
    exit 1
}
$size = (Get-Item $imgFile).Length
Good ("checkpoint ready: " + $size + " bytes; page server on " + $net + ":" + $port)

# --- target runs ------------------------------------------------------------
$results = @()
$modes   = @("lazy")
if ($Mode -eq "both") { $modes = @("lazy", "eager") }

foreach ($m in $modes) {
    Info ("activating target host (mode=" + $m + ") across the network...")
    $tgtArgs = @(
        "run", "--rm", "--privileged",
        "--network", $net,
        "-v", $bind,
        "-w", "/src",
        $img, "bash", "/src/phase4/target_node.sh", $src, $port, $m
    )
    $out = docker @tgtArgs
    foreach ($line in $out) {
        if ($line -like "RESULT*") {
            Write-Host $line -ForegroundColor Yellow
            $results += $line
        } elseif ($line) {
            Write-Host ("    " + $line)
        }
    }
}

# --- summary ----------------------------------------------------------------
Write-Host ""
Sum "================ MULTI-HOST SUMMARY ================"

$preSeq = "?"
$preFile = Join-Path $artDir "pre_seq.txt"
if (Test-Path $preFile) { $preSeq = (Get-Content $preFile | Select-Object -First 1).Trim() }
Info ("source pre-migration seq : " + $preSeq)

$contOk = $true
foreach ($r in $results) {
    if ($r -match "post_seq=(\d+)") {
        if ([int]$Matches[1] -le [int]$preSeq) { $contOk = $false }
    } else {
        $contOk = $false
    }
    if ($r -notmatch "FINAL digest=") { $contOk = $false }
}

foreach ($r in $results) { Info $r }

if ($contOk) {
    Good "continuity + integrity : PASS (target resumed past source state)"
} else {
    Bad "continuity + integrity : FAIL"
}

docker rm -f $src | Out-Null
exit $(if ($contOk) { 0 } else { 1 })
