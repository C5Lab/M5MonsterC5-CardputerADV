param(
    [string]$EspIdfPath,
    [string]$EspIdfCurrentSetup,
    [string]$BuildDir,
    [string]$Board,
    [string]$Action,
    [string]$Port
)

$ErrorActionPreference = 'Stop'
# Prevent native stderr from being promoted to PowerShell terminating errors.
if (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

# Always run from project root (parent of .vscode).
$projectRoot = Split-Path -Parent $PSScriptRoot
Set-Location $projectRoot

$config = $null
$settingsPath = Join-Path $PSScriptRoot 'settings.json'
if (Test-Path $settingsPath) {
    try {
        $config = Get-Content $settingsPath -Raw | ConvertFrom-Json
    } catch {
        # Ignore malformed settings.json and continue with env/args
    }
}

if ([string]::IsNullOrWhiteSpace($EspIdfPath) -and $config) {
    $EspIdfPath = $config.'idf.espIdfPath'
}
if ([string]::IsNullOrWhiteSpace($EspIdfPath)) {
    $EspIdfPath = $EspIdfCurrentSetup
}
if ([string]::IsNullOrWhiteSpace($EspIdfPath) -and $config) {
    $EspIdfPath = $config.'idf.currentSetup'
}
if ([string]::IsNullOrWhiteSpace($EspIdfPath)) {
    $EspIdfPath = $env:IDF_PATH
}
if ([string]::IsNullOrWhiteSpace($EspIdfPath)) {
    throw 'ESP-IDF path not set. Configure idf.espIdfPath/idf.currentSetup in .vscode/settings.json or IDF_PATH.'
}

if ([string]::IsNullOrWhiteSpace($BuildDir) -or [string]::IsNullOrWhiteSpace($Board) -or [string]::IsNullOrWhiteSpace($Action)) {
    throw "Missing required args. BuildDir='$BuildDir' Board='$Board' Action='$Action'"
}

$exportScript = Join-Path $EspIdfPath 'export.ps1'
if (!(Test-Path $exportScript)) {
    throw "Cannot find $exportScript"
}

if (($Action -eq 'flash' -or $Action -eq 'monitor') -and [string]::IsNullOrWhiteSpace($Port)) {
    if ($config) {
        $Port = $config.'idf.portWin'
        if ([string]::IsNullOrWhiteSpace($Port)) {
            $Port = $config.'idf.port'
        }
    }
    if ([string]::IsNullOrWhiteSpace($Port)) {
        throw 'Serial port not set. Configure idf.portWin (or idf.port) in .vscode/settings.json.'
    }
}

. $exportScript | Out-Null

$idfArgs = @('-B', $BuildDir, "-DBOARD=$Board")
if (![string]::IsNullOrWhiteSpace($Port)) {
    $idfArgs += @('-p', $Port)
}
$idfArgs += $Action

function Invoke-IdfPy {
    param([string[]]$ArgsToRun)
    $tmpOut = Join-Path $env:TEMP ("idf_task_" + [Guid]::NewGuid().ToString("N") + ".log")
    try {
        $LASTEXITCODE = 0
        & "idf.py" @ArgsToRun *> $tmpOut
        $nativeCode = $LASTEXITCODE
        if ($null -eq $nativeCode) {
            $nativeCode = 0
        }
        $code = [int]$nativeCode
        $text = ''
        if (Test-Path $tmpOut) {
            $text = Get-Content -Path $tmpOut -Raw
            if (-not [string]::IsNullOrWhiteSpace($text)) {
                Write-Host $text
            }
        }

        return @{
            Code = $code
            Text = $text
        }
    } catch {
        $msg = $_.ToString()
        if (-not [string]::IsNullOrWhiteSpace($msg)) {
            Write-Host $msg
        }
        return @{
            Code = 1
            Text = $msg
        }
    } finally {
        if (Test-Path $tmpOut) {
            Remove-Item -Path $tmpOut -Force -ErrorAction SilentlyContinue
        }
    }
}

function Invoke-IdfPyStreaming {
    param([string[]]$ArgsToRun)
    $LASTEXITCODE = 0
    & "idf.py" @ArgsToRun
    if ($null -eq $LASTEXITCODE) {
        return 0
    }
    return [int]$LASTEXITCODE
}

if ($Action -eq 'fullclean') {
    $maxRetries = 3
    for ($attempt = 1; $attempt -le $maxRetries; $attempt++) {
        $result = Invoke-IdfPy -ArgsToRun $idfArgs
        if ($result.Code -eq 0) {
            exit 0
        }

        $isWinLock = $result.Text -match 'WinError 32' -or
                     $result.Text -match 'PermissionError' -or
                     $result.Text -match 'used by another process'
        $isNotCmakeBuildDir = $result.Text -match "doesn't seem to be a CMake build directory"

        # If idf.py refuses fullclean because build dir is not a CMake dir,
        # treat it as a stale build cache and remove it directly.
        if ($isNotCmakeBuildDir) {
            if (Test-Path $BuildDir) {
                Write-Host "idf.py fullclean refused non-CMake dir. Removing '$BuildDir' directly..."
                try {
                    Remove-Item -Path $BuildDir -Recurse -Force -ErrorAction Stop
                    Write-Host "Removed '$BuildDir'."
                } catch {
                    Write-Host "Failed to remove '$BuildDir': $($_.Exception.Message)"
                    exit 1
                }
            } else {
                Write-Host "'$BuildDir' not found; nothing to clean."
            }
            exit 0
        }

        if ($attempt -lt $maxRetries -and $isWinLock) {
            Write-Host "Retrying fullclean after file-lock error (attempt $attempt/$maxRetries)..."
            Start-Sleep -Seconds 2
            continue
        }

        exit $result.Code
    }
} else {
    $exitCode = Invoke-IdfPyStreaming -ArgsToRun $idfArgs
    if ($exitCode -ne 0) {
        exit $exitCode
    }
}
