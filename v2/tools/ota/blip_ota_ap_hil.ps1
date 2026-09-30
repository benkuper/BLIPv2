[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("status", "upload", "interrupt")]
    [string] $Operation,

    [Parameter(Mandatory = $true)]
    [string] $DeviceSsid,

    [Parameter(Mandatory = $true)]
    [string] $InternetProfile,

    [string] $DeviceOrigin = "http://192.168.4.1",

    [string] $Image,

    [ValidateSet("esp32", "esp32s3", "esp32c6")]
    [string] $Target = "esp32c6",

    [string] $Profile = "minimal",

    [int] $InterruptAfter = 65536,

    [string] $Python = "python",

    [int] $RecoveryDelaySeconds = 600
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "../wifi/blip_wifi_guard.ps1")
$interface = "Wi-Fi"
$profileFile = Join-Path ([System.IO.Path]::GetTempPath()) "$DeviceSsid.xml"
$uploader = Join-Path $PSScriptRoot "blip_ota.py"

function Invoke-Netsh {
    param([string[]] $Arguments)
    & netsh @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "netsh failed: $($Arguments -join ' ')"
    }
}

function Get-ConnectedSsid {
    $match = netsh wlan show interfaces |
        Select-String -Pattern '^\s*SSID\s*:\s*(.+)$' |
        Select-Object -First 1
    if ($null -eq $match) {
        return ""
    }
    return $match.Matches[0].Groups[1].Value.Trim()
}

function Wait-ConnectedSsid {
    param([string] $Ssid, [int] $TimeoutSeconds = 30)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ((Get-ConnectedSsid) -eq $Ssid) {
            return
        }
        Start-Sleep -Milliseconds 500
    }
    throw "timed out connecting to Wi-Fi '$Ssid'"
}

function Wait-DeviceHttp {
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    while ([DateTime]::UtcNow -lt $deadline) {
        & curl.exe --connect-timeout 1 --max-time 2 --silent --fail `
            --output NUL "$DeviceOrigin/api/firmware"
        if ($LASTEXITCODE -eq 0) {
            return
        }
        Start-Sleep -Milliseconds 500
    }
    throw "device HTTP endpoint did not become ready at $DeviceOrigin"
}

if ($Operation -ne "status" -and [string]::IsNullOrWhiteSpace($Image)) {
    throw "-Image is required for '$Operation'"
}

$escapedSsid = [System.Security.SecurityElement]::Escape($DeviceSsid)
$profileXml = @"
<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
  <name>$escapedSsid</name>
  <SSIDConfig><SSID><name>$escapedSsid</name></SSID></SSIDConfig>
  <connectionType>ESS</connectionType>
  <connectionMode>manual</connectionMode>
  <MSM><security><authEncryption>
    <authentication>open</authentication>
    <encryption>none</encryption>
    <useOneX>false</useOneX>
  </authEncryption></security></MSM>
</WLANProfile>
"@

$offlineError = $null
$guard = $null
try {
    [System.IO.File]::WriteAllText($profileFile, $profileXml)
    Invoke-Netsh -Arguments @("wlan", "add", "profile", "filename=$profileFile", "interface=$interface", "user=current")
    $guard = Start-BlipWifiGuard -InternetProfile $InternetProfile -DeviceSsid $DeviceSsid `
        -Interface $interface -RecoveryDelaySeconds $RecoveryDelaySeconds
    Invoke-Netsh -Arguments @("wlan", "disconnect", "interface=$interface")
    Invoke-Netsh -Arguments @("wlan", "connect", "name=$DeviceSsid", "ssid=$DeviceSsid", "interface=$interface")
    Wait-ConnectedSsid -Ssid $DeviceSsid

    Wait-DeviceHttp

    if ($Operation -eq "status") {
        & curl.exe --connect-timeout 2 --max-time 5 --silent --show-error "$DeviceOrigin/api/firmware"
        if ($LASTEXITCODE -ne 0) { throw "firmware status request failed" }
        & curl.exe --connect-timeout 2 --max-time 5 --silent --show-error "$DeviceOrigin/api/web-assets"
        if ($LASTEXITCODE -ne 0) { throw "web asset status request failed" }
        & curl.exe --connect-timeout 2 --max-time 5 --silent --show-error `
            --output NUL --write-out "root_http=%{http_code} bytes=%{size_download}`n" `
            --header "Accept: text/html" "$DeviceOrigin/"
        if ($LASTEXITCODE -ne 0) { throw "root asset request failed" }
    } else {
        $arguments = @(
            $uploader, $Image, "--device", $DeviceOrigin, "--target", $Target,
            "--profile", $Profile
        )
        if ($Operation -eq "interrupt") {
            $arguments += @("--interrupt-after", $InterruptAfter)
        }
        & $Python @arguments
        if ($LASTEXITCODE -ne 0) { throw "OTA uploader failed" }
        if ($Operation -eq "interrupt") {
            Start-Sleep -Seconds 2
            & curl.exe --connect-timeout 2 --max-time 5 --silent --show-error "$DeviceOrigin/api/firmware"
            if ($LASTEXITCODE -ne 0) { throw "post-interruption status request failed" }
        }
    }
} catch {
    $offlineError = $_
} finally {
    if ($null -ne $guard) {
        try {
            Restore-BlipWifi -Guard $guard -InternetProfile $InternetProfile -Interface $interface
        } catch {
            if ($null -eq $offlineError) { $offlineError = $_ }
            else { Write-Warning $_ }
        }
    }
    & netsh wlan delete profile "name=$DeviceSsid" "interface=$interface" | Out-Host
    Remove-Item -LiteralPath $profileFile -Force -ErrorAction SilentlyContinue
}

if ($null -ne $offlineError) {
    throw $offlineError
}

Write-Output "PASS AP HIL operation=$Operation internet_restored=$InternetProfile"
