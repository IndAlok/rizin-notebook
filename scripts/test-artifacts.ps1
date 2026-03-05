param(
    [Parameter(Mandatory = $true)]
    [string]$ServerExe,

    [string]$RizinExe = "rizin",

    [string]$TargetBinary = "$env:WINDIR\System32\notepad.exe",

    [string]$PluginDir = "",

    [string]$CutterPluginDll = "",

    [string]$CutterNativePluginDir = "$env:APPDATA\rizin\cutter\plugins\native",

    [int]$Port = 8000,

    [int]$HealthTimeoutSec = 30
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Write-Step {
    param([string]$Message)
    Write-Host "`n[STEP] $Message" -ForegroundColor Cyan
}

function Test-ServerAlive {
    param([int]$ServerPort)
    try {
        $resp = Invoke-WebRequest -Uri "http://127.0.0.1:$ServerPort/api/v1/status" -UseBasicParsing -TimeoutSec 3
        return ($resp.StatusCode -eq 200)
    }
    catch {
        return $false
    }
}

function Wait-ServerAlive {
    param(
        [int]$ServerPort,
        [int]$TimeoutSec
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Test-ServerAlive -ServerPort $ServerPort) {
            return $true
        }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

function Invoke-RizinCommands {
    param(
        [string[]]$Commands,
        [string]$BinaryToOpen
    )

    $fullCommands = @($Commands + "q")
    $cmdString = ($fullCommands -join "; ")

    $cmdArgs = @("-q", "-c", $cmdString)
    if ($BinaryToOpen) {
        $cmdArgs += $BinaryToOpen
    }

    if ($PluginDir -and (Test-Path -LiteralPath $PluginDir)) {
        $env:RZ_USER_PLUGINS = $PluginDir
    }

    $output = & $RizinExe @cmdArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "rizin command failed with exit code $LASTEXITCODE.`nCommands: $cmdString`nOutput:`n$output"
    }

    return $output
}

function Assert-Match {
    param(
        [string]$Text,
        [string]$Pattern,
        [string]$FailureMessage
    )

    if ($Text -notmatch $Pattern) {
        throw "$FailureMessage`nPattern: $Pattern`nOutput:`n$Text"
    }
}

Write-Step "Validating inputs"
if (-not (Test-Path -LiteralPath $ServerExe)) {
    throw "Server executable not found: $ServerExe"
}
if (-not (Get-Command $RizinExe -ErrorAction SilentlyContinue)) {
    throw "Rizin executable not found in PATH: $RizinExe"
}
if (-not (Test-Path -LiteralPath $TargetBinary)) {
    throw "Target binary not found: $TargetBinary"
}
if ($CutterPluginDll -and -not (Test-Path -LiteralPath $CutterPluginDll)) {
    throw "Cutter plugin DLL not found: $CutterPluginDll"
}

$serverAlreadyRunning = Test-ServerAlive -ServerPort $Port
$serverProcess = $null

try {
    if (-not $serverAlreadyRunning) {
        Write-Step "Starting server artifact"
        $serverProcess = Start-Process -FilePath $ServerExe -PassThru -WindowStyle Hidden

        Write-Step "Waiting for health endpoint"
        if (-not (Wait-ServerAlive -ServerPort $Port -TimeoutSec $HealthTimeoutSec)) {
            throw "Server did not become healthy on port $Port within $HealthTimeoutSec seconds"
        }
    }
    else {
        Write-Step "Server already running on port $Port"
    }

    Write-Step "Smoke test: server status via plugin"
    $statusOut = Invoke-RizinCommands -Commands @("NBs") -BinaryToOpen $TargetBinary
    Assert-Match -Text $statusOut -Pattern "Notebook Server Status|Version:" -FailureMessage "NBs did not return valid status output"

    Write-Step "Creating page + extracting page ID"
    $createOut = Invoke-RizinCommands -Commands @("NBn smoke-test `"$TargetBinary`"", "NBl") -BinaryToOpen $TargetBinary

    $pageId = $null
    if ($createOut -match "ID:\s*([A-Za-z0-9]{32})") {
        $pageId = $Matches[1]
    }
    elseif ($createOut -match "\b([A-Za-z0-9]{32})\b") {
        $pageId = $Matches[1]
    }

    if (-not $pageId) {
        throw "Could not extract page ID from output.`n$createOut"
    }
    Write-Host "[INFO] Page ID: $pageId" -ForegroundColor Green

    Write-Step "Running full NB command smoke sequence"
    $smokeOut = Invoke-RizinCommands -Commands @(
        "NBp $pageId",
        "NBo $pageId",
        "NBx $pageId iI",
        "NBac $pageId pd 5",
        "NBam $pageId # markdown ok",
        "NBas $pageId print(`"script ok`")",
        "NBxs $pageId 1+1",
        "NBp $pageId",
        "NBc $pageId"
    ) -BinaryToOpen $TargetBinary

    Assert-Match -Text $smokeOut -Pattern "Pipe opened|Pipe closed|Added command cell|Added markdown cell|Added script cell|Page:" -FailureMessage "NB smoke sequence output missing expected markers"

    Write-Step "Deleting test page"
    $deleteOut = Invoke-RizinCommands -Commands @("NBd $pageId", "NBl") -BinaryToOpen $TargetBinary
    Assert-Match -Text $deleteOut -Pattern "Deleted page|No pages|ID" -FailureMessage "Delete/list validation failed"

    Write-Step "Core artifact checks passed"
    Write-Host "[PASS] Server artifact + rizin plugin command path validated." -ForegroundColor Green

    if ($CutterPluginDll) {
        Write-Step "Validating Cutter plugin artifact"
        $name = [System.IO.Path]::GetFileName($CutterPluginDll)
        if ($name -ne "CutterNotebookPlugin.dll") {
            throw "Unexpected Cutter plugin filename '$name'. Expected 'CutterNotebookPlugin.dll'."
        }

        if (-not (Test-Path -LiteralPath $CutterNativePluginDir)) {
            New-Item -ItemType Directory -Path $CutterNativePluginDir -Force | Out-Null
        }

        $dest = Join-Path $CutterNativePluginDir "CutterNotebookPlugin.dll"
        Copy-Item -LiteralPath $CutterPluginDll -Destination $dest -Force
        Write-Host "[PASS] Cutter plugin DLL copied to: $dest" -ForegroundColor Green
    }

    Write-Host "`nManual Cutter plugin checks still required:" -ForegroundColor Yellow
    Write-Host "  1) Open Cutter and verify plugin loads without errors"
    Write-Host "  2) Trigger each notebook menu action once"
    Write-Host "  3) Confirm page create/open/exec flows work end-to-end"
}
finally {
    if ($serverProcess -and -not $serverAlreadyRunning) {
        Write-Step "Stopping server process started by this script"
        try {
            Stop-Process -Id $serverProcess.Id -Force -ErrorAction SilentlyContinue
        }
        catch {
        }
    }
}
