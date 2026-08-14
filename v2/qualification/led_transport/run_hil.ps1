param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("esp32", "esp32s3", "esp32c6")]
    [string]$Target,
    [Parameter(Mandatory = $true)]
    [string]$Port,
    [Parameter(Mandatory = $true)]
    [string]$BuildDir,
    [Parameter(Mandatory = $true)]
    [string]$Backup,
    [Parameter(Mandatory = $true)]
    [string]$Output,
    [int]$Seconds = 20
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$buildPath = (Resolve-Path (Join-Path $repo $BuildDir)).Path
$backupPath = (Resolve-Path (Join-Path $repo $Backup)).Path
$outputPath = Join-Path $repo $Output

if ((Get-Item -LiteralPath $backupPath).Length -lt 0x100000) {
    throw "Backup does not cover the qualification image overwrite range"
}

$qualificationPassed = $false
$restorePassed = $false
try {
    & idf.py -C (Join-Path $repo "v2\qualification\led_transport") `
        -B $buildPath -p $Port flash
    if ($LASTEXITCODE -ne 0) {
        throw "Qualification flash failed"
    }

    & python (Join-Path $repo "v2\tools\ota\blip_ota_serial_capture.py") `
        --port $Port --output $outputPath --seconds $Seconds --reset
    if ($LASTEXITCODE -ne 0) {
        throw "Qualification serial capture failed"
    }
    if (-not (Select-String -LiteralPath $outputPath -SimpleMatch "BLIP_LED_QUAL_DONE" -Quiet)) {
        throw "Qualification completion marker was not captured"
    }
    $qualificationPassed = $true
}
finally {
    & python -m esptool --chip $Target --port $Port write-flash 0 $backupPath
    if ($LASTEXITCODE -eq 0) {
        & python -m esptool --chip $Target --port $Port verify-flash 0 $backupPath
        $restorePassed = $LASTEXITCODE -eq 0
    }
}

if (-not $restorePassed) {
    throw "Original flash range was not restored and verified"
}
if (-not $qualificationPassed) {
    throw "Qualification did not complete"
}
Write-Output "PASS target=$Target port=$Port capture=$Output restore=verified"
