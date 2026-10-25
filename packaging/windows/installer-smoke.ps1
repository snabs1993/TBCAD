# First-run sanity check of the built installer, run by release.yml on the Windows runner.
# Installs silently into a temp folder, checks the files, runs the installed programs once,
# uninstalls and checks that nothing is left behind.
param(
    [Parameter(Mandatory = $true)][string]$Setup,
    [string]$Sample = ''
)
$ErrorActionPreference = 'Stop'
$dir = Join-Path $env:RUNNER_TEMP 'tbcad-smoke'
if (-not $env:RUNNER_TEMP) { $dir = Join-Path $env:TEMP 'tbcad-smoke' }
Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue

function Run($exe, $argList) {
    $p = Start-Process $exe -ArgumentList $argList -Wait -PassThru
    return $p.ExitCode
}

# Upgrading over a running TBCAD must close it instead of failing on the locked tbcad.exe.
$iss = Get-Content (Join-Path $PSScriptRoot 'tbcad.iss') -Raw
foreach ($k in 'CloseApplications=yes', 'RestartApplications=no') {
    if ($iss -notmatch [regex]::Escape($k)) { throw "tbcad.iss lacks $k" }
}

$code = Run $Setup @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/CURRENTUSER', "/DIR=$dir", '/TASKS=assoc_tbcad')
if ($code -ne 0) { throw "installer exit code $code" }

foreach ($f in 'tbcad.exe', 'tbcad-convert.exe', 'tbcad-mcp.exe', 'LICENSE', 'THIRD_PARTY_NOTICES.txt',
               'assets', 'docs', 'samples', 'unins000.exe') {
    if (-not (Test-Path (Join-Path $dir $f))) { throw "missing after install: $f" }
}

# The app starts, draws one frame and exits (77 = no OpenGL on the runner, which is fine here).
$code = Run (Join-Path $dir 'tbcad.exe') @('--self-test')
if ($code -ne 0 -and $code -ne 77) { throw "tbcad --self-test exit code $code" }
Write-Host "tbcad --self-test exit code $code"

# The converter turns a sample DXF into a drawing.
if ($Sample -and (Test-Path $Sample)) {
    $out = Join-Path $dir 'smoke.tbcad'
    $code = Run (Join-Path $dir 'tbcad-convert.exe') @("`"$Sample`"", "-o", "`"$out`"")
    if ($code -ge 2 -or -not (Test-Path $out)) { throw "tbcad-convert exit code $code" }
}

$code = Run (Join-Path $dir 'unins000.exe') @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')
if ($code -ne 0) { throw "uninstaller exit code $code" }
Start-Sleep -Seconds 3   # the uninstaller finishes its clean-up in a child process
if (Test-Path (Join-Path $dir 'tbcad.exe')) { throw 'tbcad.exe still there after uninstall' }
if (Test-Path 'HKCU:\Software\Classes\.tbcad') { throw '.tbcad association left behind' }
Write-Host 'installer smoke test ok'
