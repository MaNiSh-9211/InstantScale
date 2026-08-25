# ============================================================================
#  GitHub CLI authentication via raw OAuth Device Flow (no TTY required)
#  Usage:  powershell -File gh_login.ps1 request    -> prints URL + one-time code
#          powershell -File gh_login.ps1 complete   -> polls token, feeds gh
# ============================================================================
param([Parameter(Mandatory = $true)][string]$Phase)

$client = "178c6fc778ccc68e1d6a"   # GitHub CLI's public OAuth client_id
$tmp    = "$env:TEMP\iscale_gh"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

if ($Phase -eq "request") {
    $r = Invoke-RestMethod -Method Post `
        -Uri "https://github.com/login/device/code" `
        -Headers @{ Accept = "application/json" } `
        -Body @{ client_id = $client; scope = "repo" }

    Set-Content (Join-Path $tmp "device_code") $r.device_code
    Set-Content (Join-Path $tmp "interval")    $r.interval

    Write-Host ""
    Write-Host "  1. Open :  $($r.verification_uri)" -ForegroundColor Cyan
    Write-Host "  2. Code :  $($r.user_code)" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "  (code expires in $($r.expires_in)s; approve, then run 'complete')"
}

if ($Phase -eq "complete") {
    $device = Get-Content (Join-Path $tmp "device_code") -ErrorAction Stop
    $interval = [int](Get-Content (Join-Path $tmp "interval"))
    if ($interval -lt 5) { $interval = 5 }

    $token = $null
    for ($i = 0; $i -lt 80; $i++) {
        Start-Sleep -Seconds $interval
        $r = Invoke-RestMethod -Method Post `
            -Uri "https://github.com/login/oauth/access_token" `
            -Headers @{ Accept = "application/json" } `
            -Body @{
                client_id     = $client
                device_code   = $device
                grant_type    = "urn:ietf:params:oauth:grant-type:device_code"
            }
        if ($r.access_token) { $token = $r.access_token; break }
        if ($r.error -eq "authorization_pending") { Write-Host "." -NoNewline; continue }
        if ($r.error -eq "slow_down")             { $interval += 5;      continue }
        Write-Host ""; Write-Host ("flow error: " + $r.error); exit 1
    }

    if (-not $token) { Write-Host "timed out waiting for approval"; exit 1 }
    $token | & "$env:ProgramFiles\GitHub CLI\gh.exe" auth login --with-token
    & "$env:ProgramFiles\GitHub CLI\gh.exe" auth status
}
