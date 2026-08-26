# HotPod kind A/B test — full pipeline with resilience
$ErrorActionPreference = "Continue"
$repo  = "C:\Users\at381\OneDrive\Desktop\InstantScale"
$build = "$env:TEMP\hotpod-build"
$kind  = "$env:USERPROFILE\tools\kind.exe"
$gbash = "C:\Program Files\Git\bin\bash.exe"

# 0. engine
docker version --format ok *> $null
if ($LASTEXITCODE -ne 0) {
    Get-Process "Docker Desktop","com.docker.backend" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep 8
    Start-Process "C:\Program Files\Docker\Docker\Docker Desktop.exe"
    foreach ($i in 1..50) { Start-Sleep 5; docker version --format ok *> $null; if ($LASTEXITCODE -eq 0) { break } }
}
Write-Host "[run] engine ready"

# 1. fresh build context (dodges OneDrive mount flake)
if (Test-Path $build) { Remove-Item $build -Recurse -Force }
robocopy $repo $build /E /XD .git artifacts /NFL /NDL /NJH /NJS | Out-Null

# 2. build image
Write-Host "[run] building hotpod:test ..."
docker build -f "$build\deploy\Dockerfile" -t hotpod:test $build | Select-Object -Last 2
if ($LASTEXITCODE -ne 0) { Write-Host "[run] BUILD FAILED" -ForegroundColor Red; exit 1 }

# 3. kind node up + image loaded
docker start hotpod-control-plane *> $null
& $kind get clusters
& $kind load docker-image hotpod:test --name hotpod
if ($LASTEXITCODE -ne 0) { Write-Host "[run] kind load failed" -ForegroundColor Red; exit 1 }
kubectl config use-context kind-hotpod | Out-Null
kubectl wait --for=condition=ready node/hotpod-control-plane --timeout=180s

# 4. the A/B test
Write-Host "[run] running A/B test..."
Set-Location $repo
& $gbash deploy/kind/test.sh
exit $LASTEXITCODE
