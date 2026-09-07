$ErrorActionPreference = 'Stop'
$benchmarkBase = $env:MOONLIGHT_BENCHMARK_BASE
if (-not $benchmarkBase) { throw 'Set MOONLIGHT_BENCHMARK_BASE to the fresh TV test URL' }
function Send-BenchmarkReport([string]$message) {
    Invoke-RestMethod -Uri ($benchmarkBase + '/report') -Method Post -ContentType 'application/json' -Body (@{report=$message} | ConvertTo-Json -Compress) | Out-Null
}
try {
    $browserPath = Get-Process -Name vivaldi -ErrorAction SilentlyContinue | Where-Object Path |
        Select-Object -First 1 -ExpandProperty Path
    if (-not $browserPath) {
        $candidates = @((Join-Path $env:LOCALAPPDATA 'Vivaldi\Application\vivaldi.exe'),
            (Join-Path $env:ProgramFiles 'Vivaldi\Application\vivaldi.exe'),
            (Join-Path ${env:ProgramFiles(x86)} 'Vivaldi\Application\vivaldi.exe'))
        $browserPath = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    }
    if (-not $browserPath -or -not (Test-Path -LiteralPath $browserPath)) {
        throw 'Installed Vivaldi executable not found'
    }
    # Use the installed session. Do not alter first-run or privacy preferences.
    # This opens only our fixed LAN benchmark URL; the page expires in 30 minutes.
    Start-Process -FilePath $browserPath -ArgumentList @('--new-window',
        '--start-fullscreen', '--start-maximized', ($benchmarkBase + '/marker#motion')) | Out-Null
    Send-BenchmarkReport 'Marker URL passed to the existing Vivaldi session; verify clock and pixels.'
} catch {
    Send-BenchmarkReport ('Benchmark launcher error: ' + $_.Exception.Message)
    exit 1
}
