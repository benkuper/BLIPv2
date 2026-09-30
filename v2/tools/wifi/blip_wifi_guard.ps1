# Dot-source this file before switching the computer to a BLIP access point.
# The watchdog runs in another process and can restore Wi-Fi if the caller dies.

function Get-BlipConnectedSsid {
    $match = netsh wlan show interfaces |
        Select-String -Pattern '^\s*SSID\s*:\s*(.+)$' |
        Select-Object -First 1
    if ($null -eq $match) { return "" }
    return $match.Matches[0].Groups[1].Value.Trim()
}

function Test-BlipInternet {
    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $connection = $client.BeginConnect("www.microsoft.com", 443, $null, $null)
        if (-not $connection.AsyncWaitHandle.WaitOne(5000)) { return $false }
        $client.EndConnect($connection)
        return $true
    } catch {
        return $false
    } finally {
        $client.Dispose()
    }
}

function Start-BlipWifiGuard {
    param(
        [Parameter(Mandatory = $true)] [string] $InternetProfile,
        [Parameter(Mandatory = $true)] [string] $DeviceSsid,
        [string] $Interface = "Wi-Fi",
        [int] $RecoveryDelaySeconds = 600
    )

    if ($RecoveryDelaySeconds -lt 30) { throw "recovery delay must be at least 30 seconds" }
    if ($InternetProfile -eq $DeviceSsid) { throw "Internet and device SSIDs must differ" }
    if ((Get-BlipConnectedSsid) -ne $InternetProfile) {
        throw "connect to Internet profile '$InternetProfile' before AP HIL"
    }
    if (-not (Test-BlipInternet)) { throw "Internet access must be working before AP HIL" }

    $stateDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("blip-wifi-guard-" + [guid]::NewGuid().ToString("N"))
    [System.IO.Directory]::CreateDirectory($stateDirectory) | Out-Null
    $state = @{
        internet_profile = $InternetProfile
        device_ssid = $DeviceSsid
        interface = $Interface
        delay_seconds = $RecoveryDelaySeconds
    }
    $state | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stateDirectory "state.json") -Encoding UTF8

    $watchdog = Join-Path $PSScriptRoot "blip_wifi_recovery.ps1"
    $arguments = @(
        "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
        "-File", ('"' + $watchdog + '"'),
        "-StateDirectory", ('"' + $stateDirectory + '"')
    )
    $process = Start-Process -FilePath "powershell.exe" -ArgumentList $arguments -WindowStyle Hidden -PassThru
    $armedFile = Join-Path $stateDirectory "armed"
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while (-not [System.IO.File]::Exists($armedFile) -and [DateTime]::UtcNow -lt $deadline) {
        $process.Refresh()
        if ($process.HasExited) { break }
        Start-Sleep -Milliseconds 100
    }
    $process.Refresh()
    if (-not [System.IO.File]::Exists($armedFile) -or $process.HasExited) {
        throw "independent Wi-Fi recovery did not arm; refusing to switch networks"
    }
    return [pscustomobject]@{ Process = $process; StateDirectory = $stateDirectory }
}

function Restore-BlipWifi {
    param(
        [Parameter(Mandatory = $true)] $Guard,
        [Parameter(Mandatory = $true)] [string] $InternetProfile,
        [string] $Interface = "Wi-Fi"
    )

    & netsh wlan connect "name=$InternetProfile" "interface=$Interface" | Out-Host
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ((Get-BlipConnectedSsid) -eq $InternetProfile -and (Test-BlipInternet)) {
            [System.IO.File]::WriteAllText((Join-Path $Guard.StateDirectory "cancel"), "Internet restored")
            return
        }
        Start-Sleep -Seconds 1
    }
    throw "Internet not restored; the independent Wi-Fi watchdog remains active"
}
