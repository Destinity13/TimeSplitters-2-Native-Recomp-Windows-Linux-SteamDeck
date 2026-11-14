# Windows test lab agent: polls the Linux build machine for jobs, runs them in this desktop session, uploads the results.
# Start:  powershell -ExecutionPolicy Bypass -File agent.ps1 -Base http://<linux-pc>:8765/<token>
param([Parameter(Mandatory = $true)][string]$Base, [string]$Root = 'C:\TS2lab')
$ProgressPreference = 'SilentlyContinue'
New-Item -ItemType Directory -Force $Root | Out-Null
Write-Host "TS2 test lab agent. Server: $Base  Folder: $Root  (close this window to stop)"
while ($true) {
    try { $r = Invoke-WebRequest -UseBasicParsing "$Base/next" -TimeoutSec 20 } catch { Start-Sleep 5; continue }
    if ($r.StatusCode -ne 200 -or -not $r.Content) { Start-Sleep 3; continue }
    $text = if ($r.Content -is [byte[]]) { [Text.Encoding]::UTF8.GetString($r.Content) } else { $r.Content }
    $job = $text | ConvertFrom-Json
    Write-Host ("{0} job {1}: {2}" -f (Get-Date -Format HH:mm:ss), $job.id, $job.title)
    $out = Join-Path $Root ("out_" + $job.id)
    if (Test-Path $out) { Remove-Item -Recurse -Force $out }
    New-Item -ItemType Directory -Force $out | Out-Null
    $log = Join-Path $out 'agent.txt'
    try {
        foreach ($f in $job.files) {
            $dst = Join-Path $Root $f.to
            if ($f.fresh -or -not (Test-Path $dst)) {
                New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
                Write-Host "  download $($f.name)"
                Invoke-WebRequest -UseBasicParsing "$Base/files/$($f.name)" -OutFile $dst
                if ($f.unzip) {
                    $ziel = Join-Path $Root $f.unzip
                    if (Test-Path $ziel) { Remove-Item -Recurse -Force $ziel }
                    Expand-Archive -Force $dst $ziel
                }
            }
        }
        $script = Join-Path $out 'job.ps1'
        Set-Content -Path $script -Value $job.script -Encoding UTF8
        $p = Start-Process powershell -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$script`"", "`"$out`"" -PassThru `
            -RedirectStandardOutput (Join-Path $out 'stdout.txt') -RedirectStandardError (Join-Path $out 'stderr.txt') -WorkingDirectory $Root
        # progress: every 10 s show the elapsed time and the last line the job wrote (stdout or progress.txt) in this
        # window, and send the same line to the build machine so the progress is visible on both sides
        $start = Get-Date; $letzte = ''
        while (-not $p.WaitForExit(10000)) {
            $sek = [int]((Get-Date) - $start).TotalSeconds
            $zeile = ''
            foreach ($f in 'progress.txt', 'setup_console.txt', 'stdout.txt') {
                $pf = Join-Path $out $f
                if (Test-Path $pf) { $z = Get-Content $pf -Tail 1 -ErrorAction SilentlyContinue; if ($z) { $zeile = "$z"; break } }
            }
            Write-Host ("  {0,4} s  {1}" -f $sek, $zeile)
            if ($zeile -ne $letzte -or ($sek % 60) -lt 10) {
                $letzte = $zeile
                try {
                    $tmp = Join-Path $Root 'progress_upload.txt'
                    Set-Content -Path $tmp -Value ("{0} s | {1}" -f $sek, $zeile) -Encoding UTF8
                    Invoke-WebRequest -UseBasicParsing -Method Put -InFile $tmp "$Base/results/progress_$($job.id).txt" -TimeoutSec 10 | Out-Null
                } catch { }
            }
            if ($sek -gt [int]$job.timeout) { & taskkill /T /F /PID $p.Id | Out-Null; 'TIMEOUT' | Add-Content $log; break }
        }
    } catch { $_ | Out-String | Add-Content $log }
    try {
        $zip = Join-Path $Root ("result_" + $job.id + ".zip")
        if (Test-Path $zip) { Remove-Item $zip }
        Compress-Archive -Path (Join-Path $out '*') -DestinationPath $zip
        Invoke-WebRequest -UseBasicParsing -Method Put -InFile $zip "$Base/results/result_$($job.id).zip" | Out-Null
        Write-Host "  result sent"
    } catch { Write-Host "  upload failed: $_" }
}
