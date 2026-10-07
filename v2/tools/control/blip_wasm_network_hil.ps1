[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Python,
    [Parameter(Mandatory = $true)] [string] $Port,
    [Parameter(Mandatory = $true)] [string] $Board,
    [Parameter(Mandatory = $true)] [string] $BuildDirectory,
    [Parameter(Mandatory = $true)] [string] $Output,
    [Parameter(Mandatory = $true)] [string] $Ip,
    [string] $NetworkProfile = "Archi-wifi guest",
    [string] $InternetProfile = "Archi-Wifi",
    [int] $Pixels = 36,
    [ValidateRange(1, 100)] [int] $Cycles = 20,
    [switch] $CoexistenceOnly
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "../wifi/blip_wifi_guard.ps1")
$interface = "Wi-Fi"
$temporaryProfile = "BLIP-WASM-" + [guid]::NewGuid().ToString("N")
$profileDirectory = Join-Path ([IO.Path]::GetTempPath()) $temporaryProfile
[IO.Directory]::CreateDirectory($profileDirectory) | Out-Null
$guard = $null
try {
    # Export without key=clear. Never print profile XML or its credentials.
    & netsh wlan export profile "name=$NetworkProfile" "interface=$interface" "folder=$profileDirectory" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "cannot export the existing network profile" }
    $files = @(Get-ChildItem -LiteralPath $profileDirectory -Filter '*.xml')
    if ($files.Count -ne 1) { throw "expected one exported network profile" }
    [xml] $profile = [IO.File]::ReadAllText($files[0].FullName)
    $ssid = $profile.WLANProfile.SSIDConfig.SSID.name
    $profile.WLANProfile.name = $temporaryProfile
    $profile.WLANProfile.connectionMode = "manual"
    $profile.Save($files[0].FullName)
    $guard = Start-BlipWifiGuard -InternetProfile $InternetProfile -DeviceSsid $temporaryProfile `
        -Interface $interface -RecoveryDelaySeconds 45
    # The guard deletes only this temporary clone, preserving the saved profile.
    & netsh wlan add profile "filename=$($files[0].FullName)" "interface=$interface" user=current | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "cannot install the temporary network profile" }
    & netsh wlan connect "name=$temporaryProfile" "ssid=$ssid" "interface=$interface" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "cannot connect to the test network" }
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ((Get-BlipConnectedSsid) -ne $ssid -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 200 }
    if ((Get-BlipConnectedSsid) -ne $ssid) { throw "test network association timed out" }
    # Association alone does not establish DHCP or a usable IP route.
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    $reachable = $false
    do {
        $client = [Net.Sockets.TcpClient]::new()
        try {
            $connection = $client.BeginConnect($Ip, 80, $null, $null)
            if ($connection.AsyncWaitHandle.WaitOne(1000)) {
                $client.EndConnect($connection)
                $reachable = $true
            }
        } catch {} finally { $client.Dispose() }
        if (-not $reachable) { Start-Sleep -Milliseconds 500 }
    } while (-not $reachable -and [DateTime]::UtcNow -lt $deadline)
    if (-not $reachable) { throw "device HTTP port is unreachable on the test network" }
    $trialArguments = @('--port', $Port, '--board', $Board, '--build', $BuildDirectory,
        '--output', $Output, '--ip', $Ip, '--pixels', $Pixels, '--cycles', $Cycles)
    if ($CoexistenceOnly) { $trialArguments += '--coexistence-only' }
    & $Python (Join-Path $PSScriptRoot 'blip_wasm_hil.py') @trialArguments
    if ($LASTEXITCODE -ne 0) { throw "WASM network qualification failed" }
} finally {
    if ($null -ne $guard) {
        Restore-BlipWifi -Guard $guard -InternetProfile $InternetProfile -Interface $interface
        & netsh wlan delete profile "name=$temporaryProfile" "interface=$interface" | Out-Null
    }
    foreach ($file in @(Get-ChildItem -LiteralPath $profileDirectory -Filter '*.xml')) {
        Remove-Item -LiteralPath $file.FullName
    }
    Remove-Item -LiteralPath $profileDirectory
}
Write-Output "PASS WASM network HIL; original Wi-Fi and Internet restored"
$report = Get-Content -LiteralPath $Output -Raw | ConvertFrom-Json
$report | Add-Member -NotePropertyName host_network -NotePropertyValue @{
    test_ssid = $ssid
    original_profile = $InternetProfile
    independent_recovery_armed = $true
    recovery_delay_seconds = 45
    original_wifi_and_internet_restored = $true
    temporary_profile_removed = $true
}
[IO.File]::WriteAllText([IO.Path]::GetFullPath($Output), ($report | ConvertTo-Json -Depth 100) + "`n", [Text.UTF8Encoding]::new($false))
