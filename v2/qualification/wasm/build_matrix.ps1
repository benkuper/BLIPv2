param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('esp32','esp32s3','esp32c6')]
    [string]$Chip,
    [string]$IdfPath = 'D:\Projects\Dev\.tools\esp-idf-v6.0.2',
    [string]$BuildDirectory = ''
)
# Windows PowerShell 5 treats native stderr (including IDF's informational
# activation output) as errors. Check native exit codes explicitly.
$ErrorActionPreference = 'Continue'
$Repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$BuildPath = if ($BuildDirectory) { [IO.Path]::GetFullPath((Join-Path $Repo $BuildDirectory)) } else { Join-Path $Repo "build/m6-wasm-$Chip" }
& (Join-Path $IdfPath 'export.ps1') | Out-Null
Push-Location $Repo
try {
    python v2/qualification/wasm/prepare_dependencies.py
    if ($LASTEXITCODE -ne 0) { throw 'Dependency preparation failed' }
    foreach ($Engine in @('baseline','wamr','wasm3')) {
        $LogPath = Join-Path $Repo "build/m6-wasm-$Chip-$Engine.log"
        idf.py -C v2/qualification/wasm -B $BuildPath -D "IDF_TARGET=$Chip" -D "BLIP_WASM_ENGINE=$Engine" build *> $LogPath
        if ($LASTEXITCODE -ne 0) { throw "Build failed: $LogPath" }
        $ArchivePath = Join-Path $Repo "build/m6-wasm-images/$Chip/$Engine"
        New-Item -ItemType Directory -Path $ArchivePath -Force -ErrorAction Stop | Out-Null
        foreach ($Name in @('blip-wasm-benchmark.bin','blip-wasm-benchmark.elf','blip-wasm-benchmark.map','ota_data_initial.bin','sdkconfig','project_description.json')) {
            Copy-Item -LiteralPath (Join-Path $BuildPath $Name) -Destination (Join-Path $ArchivePath $Name) -Force -ErrorAction Stop
        }
        python -m esp_idf_size --format json2 (Join-Path $BuildPath 'blip-wasm-benchmark.map') > (Join-Path $ArchivePath 'size.json')
        if ($LASTEXITCODE -ne 0) { throw "Size collection failed: $Engine" }
        Write-Output "Archived $Chip $Engine to $ArchivePath"
    }
} finally {
    Pop-Location
}
