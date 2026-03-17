param(
    [Parameter(Mandatory = $true)]
    [string]$tag,
    [string]$PipeName = '\\.\pipe\specsensor_sample_pipe',
    [int]$TimeoutMs = 3000
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-PipeSimpleName {
    param([string]$FullPipeName)

    if ($FullPipeName.StartsWith('\\.\pipe\')) {
        return $FullPipeName.Substring(9)
    }

    return $FullPipeName
}

if ([string]::IsNullOrWhiteSpace($tag)) {
    Write-Error 'Parameter -tag must not be empty.'
    exit 2
}

$sample = $tag.Trim()
$simpleName = Get-PipeSimpleName -FullPipeName $PipeName

$client = New-Object System.IO.Pipes.NamedPipeClientStream(
    '.',
    $simpleName,
    [System.IO.Pipes.PipeDirection]::Out
)

try {
    $client.Connect($TimeoutMs)

    $encoding = New-Object System.Text.UTF8Encoding($false)
    $command = "CAPTURE $sample"
    $payload = $command + "`n"
    $payloadBytes = $encoding.GetBytes($payload)

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $client.Write($payloadBytes, 0, $payloadBytes.Length)
    $client.Flush()
    $stopwatch.Stop()

    Write-Host "[client] Pipe: $PipeName"
    Write-Host "[client] Sent ($($payloadBytes.Length) bytes UTF-8 in $($stopwatch.ElapsedMilliseconds) ms): $command"
}
catch {
    Write-Error "Failed to send CAPTURE command: $($_.Exception.Message)"
    exit 1
}
finally {
    $client.Dispose()
}

exit 0
