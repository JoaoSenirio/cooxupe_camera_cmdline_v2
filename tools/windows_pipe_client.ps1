param(
    [string]$PipeName = '\\.\pipe\specsensor_sample_pipe',
    [string]$SampleName = '',
    [int]$TimeoutMs = 3000,
    [switch]$Interactive
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

function Send-CaptureCommand {
    param(
        [string]$NamedPipe,
        [string]$Sample,
        [int]$ConnectTimeoutMs
    )

    if ([string]::IsNullOrWhiteSpace($Sample)) {
        throw 'SampleName must not be empty.'
    }

    $simpleName = Get-PipeSimpleName -FullPipeName $NamedPipe
    $client = New-Object System.IO.Pipes.NamedPipeClientStream(
        '.',
        $simpleName,
        [System.IO.Pipes.PipeDirection]::Out
    )

    try {
        $client.Connect($ConnectTimeoutMs)

        $writer = New-Object System.IO.StreamWriter($client)
        try {
            $writer.AutoFlush = $true
            $command = "CAPTURE $Sample"
            $writer.WriteLine($command)
            Write-Host "[client] Sent: $command"
        }
        finally {
            $writer.Dispose()
        }
    }
    finally {
        $client.Dispose()
    }
}

Write-Host "[client] Pipe: $PipeName"
Write-Host "[client] Expected server command: CAPTURE <sample_name>"

if (-not [string]::IsNullOrWhiteSpace($SampleName)) {
    Send-CaptureCommand -NamedPipe $PipeName -Sample $SampleName -ConnectTimeoutMs $TimeoutMs
    exit 0
}

if (-not $Interactive) {
    Write-Host 'Usage examples:'
    Write-Host "  .\tools\windows_pipe_client.ps1 -SampleName 'AMOSTRA_001'"
    Write-Host "  .\tools\windows_pipe_client.ps1 -Interactive"
    Write-Host "  .\tools\windows_pipe_client.ps1 -PipeName '\\.\pipe\specsensor_sample_pipe' -SampleName 'AMOSTRA_001'"
    exit 1
}

Write-Host "[client] Interactive mode. Type sample name and press ENTER. Type 'q' to exit."
while ($true) {
    $line = Read-Host 'sample_name'
    if ($line -eq $null) {
        continue
    }

    $trimmed = $line.Trim()
    if ($trimmed -eq '') {
        continue
    }

    if ($trimmed -eq 'q' -or $trimmed -eq 'quit' -or $trimmed -eq 'exit') {
        break
    }

    try {
        Send-CaptureCommand -NamedPipe $PipeName -Sample $trimmed -ConnectTimeoutMs $TimeoutMs
    }
    catch {
        Write-Host "[client] Failed to send command: $($_.Exception.Message)"
    }
}

Write-Host '[client] Exit'
