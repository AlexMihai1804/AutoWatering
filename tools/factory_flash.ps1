param(
    [string]$HwId,
    [switch]$NoBuild,
    [switch]$NoFlash,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptDir
$settingsPath = Join-Path $scriptDir 'factory.settings.json'
$examplePath = Join-Path $scriptDir 'factory.settings.json.example'
$pythonScript = Join-Path $scriptDir 'factory_provision_build_flash.py'

function Initialize-SettingsFile {
    if (Test-Path $settingsPath) {
        return
    }

    Write-Host "[Factory] Prima rulare: initializez config local..." -ForegroundColor Yellow

    if (Test-Path $examplePath) {
        Copy-Item $examplePath $settingsPath -Force
    }
    else {
        @{
            apiBaseUrl   = ""
            factoryToken = ""
            defaultHwId  = ""
            board        = "arduino_nano_33_ble"
            buildDir     = "build_factory"
            sysbuild     = $true
            pristine     = $true
            flashRunner  = "bossac"
            operator     = ""
            batch        = ""
        } | ConvertTo-Json | Set-Content -Path $settingsPath -Encoding UTF8
    }

    $local = Get-Content $settingsPath -Raw | ConvertFrom-Json

    $envApi = [Environment]::GetEnvironmentVariable('AUTOWATERING_FACTORY_API_BASE_URL')
    $envToken = [Environment]::GetEnvironmentVariable('AUTOWATERING_FACTORY_TOKEN')

    if ([string]::IsNullOrWhiteSpace($local.apiBaseUrl)) {
        if (-not [string]::IsNullOrWhiteSpace($envApi)) {
            $local.apiBaseUrl = $envApi
        }
        else {
            $local.apiBaseUrl = Read-Host "API Base URL (ex: https://<id>.execute-api.<region>.amazonaws.com)"
        }
    }

    if ([string]::IsNullOrWhiteSpace($local.factoryToken)) {
        if (-not [string]::IsNullOrWhiteSpace($envToken)) {
            $local.factoryToken = $envToken
        }
        else {
            $secureToken = Read-Host "Factory token" -AsSecureString
            $ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secureToken)
            try {
                $local.factoryToken = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr)
            }
            finally {
                [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr)
            }
        }
    }

    $local | ConvertTo-Json -Depth 8 | Set-Content -Path $settingsPath -Encoding UTF8
    Write-Host "[Factory] Config local salvat în $settingsPath" -ForegroundColor Green
}

function Get-HwIdFromNrfJprog {
    $nrfjprog = Get-Command nrfjprog -ErrorAction SilentlyContinue
    if (-not $nrfjprog) {
        return $null
    }

    try {
        $out = & nrfjprog --memrd 0x10000060 --w 32 --n 2 2>&1
        if ($LASTEXITCODE -ne 0) {
            return $null
        }

        $hexWords = @()
        foreach ($line in $out) {
            $matches = [regex]::Matches([string]$line, '0x([0-9A-Fa-f]{8})')
            foreach ($match in $matches) {
                $value = $match.Groups[1].Value.ToUpperInvariant()
                if ($value -ne '10000060') {
                    $hexWords += $value
                }
            }
        }

        if ($hexWords.Count -ge 2) {
            return ($hexWords[0] + $hexWords[1]).ToUpperInvariant()
        }
    }
    catch {
        return $null
    }

    return $null
}

Initialize-SettingsFile

$settings = Get-Content $settingsPath -Raw | ConvertFrom-Json

if ([string]::IsNullOrWhiteSpace($settings.apiBaseUrl) -or [string]::IsNullOrWhiteSpace($settings.factoryToken)) {
    Write-Host "`"apiBaseUrl`" și `"factoryToken`" sunt obligatorii în $settingsPath" -ForegroundColor Red
    exit 1
}

if ([string]::IsNullOrWhiteSpace($HwId)) {
    if (-not [string]::IsNullOrWhiteSpace($settings.defaultHwId)) {
        $HwId = $settings.defaultHwId
    }
    else {
        $detectedHwId = Get-HwIdFromNrfJprog
        if (-not [string]::IsNullOrWhiteSpace($detectedHwId)) {
            $HwId = $detectedHwId
            Write-Host "[Factory] HW ID detectat automat (nrfjprog): $HwId" -ForegroundColor Green
        }
        else {
            Write-Host "[Factory] Nu am putut detecta automat HW ID." -ForegroundColor Red
            Write-Host "[Factory] Scriptul rulează fără introducere manuală de HW ID." -ForegroundColor Red
            Write-Host "[Factory] Verifică:" -ForegroundColor Yellow
            Write-Host "  1) nrfjprog instalat și în PATH" -ForegroundColor Yellow
            Write-Host "  2) acces SWD/J-Link disponibil pentru citire FICR" -ForegroundColor Yellow
            Write-Host "  3) device conectat corect" -ForegroundColor Yellow
            exit 2
        }
    }
}

if ([string]::IsNullOrWhiteSpace($HwId)) {
    Write-Host "HW ID este obligatoriu." -ForegroundColor Red
    exit 1
}

$pythonCmd = Get-Command py -ErrorAction SilentlyContinue
if ($pythonCmd) {
    $pythonExe = 'py'
    $pythonPrefixArgs = @('-3')
}
else {
    $pythonExe = 'python'
    $pythonPrefixArgs = @()
}

$argsList = @()
$argsList += $pythonPrefixArgs
$argsList += $pythonScript
$argsList += '--api-base-url'; $argsList += [string]$settings.apiBaseUrl
$argsList += '--factory-token'; $argsList += [string]$settings.factoryToken
$argsList += '--hw-id'; $argsList += [string]$HwId

if (-not [string]::IsNullOrWhiteSpace($settings.board)) {
    $argsList += '--board'; $argsList += [string]$settings.board
}
if (-not [string]::IsNullOrWhiteSpace($settings.buildDir)) {
    $argsList += '--build-dir'; $argsList += [string]$settings.buildDir
}
if (-not [string]::IsNullOrWhiteSpace($settings.flashRunner)) {
    $argsList += '--flash-runner'; $argsList += [string]$settings.flashRunner
}
if (-not [string]::IsNullOrWhiteSpace($settings.operator)) {
    $argsList += '--operator'; $argsList += [string]$settings.operator
}
if (-not [string]::IsNullOrWhiteSpace($settings.batch)) {
    $argsList += '--batch'; $argsList += [string]$settings.batch
}

if ($settings.sysbuild -eq $true) { $argsList += '--sysbuild' }
if ($settings.pristine -eq $true) { $argsList += '--pristine' }
if ($NoBuild) { $argsList += '--skip-build' }
if ($NoFlash) { $argsList += '--skip-flash' }

Write-Host "[Factory] Running provisioning/build/flash..." -ForegroundColor Green
Write-Host "[Factory] HW ID: $HwId"

if ($DryRun) {
    Write-Host "[DryRun] $pythonExe $($argsList -join ' ')" -ForegroundColor Cyan
    exit 0
}

Push-Location $projectRoot
try {
    & $pythonExe @argsList
    if ($LASTEXITCODE -ne 0) {
        throw "factory_provision_build_flash.py exited with code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}

Write-Host "[Factory] Done." -ForegroundColor Green
