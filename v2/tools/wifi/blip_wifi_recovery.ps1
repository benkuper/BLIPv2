[CmdletBinding()]
param([Parameter(Mandatory = $true)] [string] $StateDirectory)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "blip_wifi_guard.ps1")

$state = Get-Content -LiteralPath (Join-Path $StateDirectory "state.json") -Raw | ConvertFrom-Json
$cancelFile = Join-Path $StateDirectory "cancel"
$armedFile = Join-Path $StateDirectory "armed"
$deadline = [DateTime]::UtcNow.AddSeconds([int] $state.delay_seconds)
[System.IO.File]::WriteAllText($armedFile, "armed")

while ([DateTime]::UtcNow -lt $deadline) {
    if ([System.IO.File]::Exists($cancelFile)) { exit 0 }
    Start-Sleep -Seconds 1
}

# The deadline is a hard limit for the AP test. Keep retrying if Windows or the
# access point is temporarily unavailable; the caller may no longer be alive.
& netsh wlan delete profile "name=$($state.device_ssid)" "interface=$($state.interface)" | Out-Null
while (-not [System.IO.File]::Exists($cancelFile)) {
    if ((Get-BlipConnectedSsid) -eq $state.internet_profile -and (Test-BlipInternet)) { exit 0 }
    & netsh wlan disconnect "interface=$($state.interface)" | Out-Null
    & netsh wlan connect "name=$($state.internet_profile)" "interface=$($state.interface)" | Out-Null
    Start-Sleep -Seconds 5
}
