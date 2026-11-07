param(
    [Parameter(Mandatory=$true)][ValidatePattern('^COM[1-9][0-9]*$')][string]$Port,
    [ValidateRange(1,4000000)][int]$Baud = 115200
)
$ErrorActionPreference = 'Stop'
$nativePort = $null
try {
    # Load only the established native no-reset backend, never the monitor loop.
    $tokens = $null; $parseErrors = $null
    $tree = [System.Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $PSScriptRoot 'monitor_serial.ps1'), [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count) { throw 'Monitor backend parse failed' }
    $definition = $tree.Find({param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq 'Get-MonitorPortSource'
    }, $true)
    if ($null -eq $definition) { throw 'Monitor backend missing' }
    . ([scriptblock]::Create($definition.Extent.Text))
    Add-Type -TypeDefinition (Get-MonitorPortSource)
    # Console.In.ReadLineAsync can block synchronously on .NET Framework's
    # synchronized TextReader. Keep stdin on a bounded background reader.
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Concurrent;
using System.Threading;
public sealed class RecoveryHostLine { public string Text; }
public sealed class RecoveryHostInput {
    public readonly BlockingCollection<RecoveryHostLine> Lines = new BlockingCollection<RecoveryHostLine>(8);
    public RecoveryHostInput() {
        Thread reader = new Thread(() => {
            while (true) {
                string line = Console.In.ReadLine();
                Lines.Add(new RecoveryHostLine { Text = line });
                if (line == null) return;
            }
        });
        reader.IsBackground = true; reader.Start();
    }
}
'@
    [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
    $nativePort = New-Object Wqn.SerialMonitor.NativePort($Port, $Baud)
    $nativePort.OpenNoReset()
    $inputReader = New-Object RecoveryHostInput
    while ($true) {
        $data = $nativePort.ReadExisting()
        if ($data.Length) { [Console]::Out.Write($data); [Console]::Out.Flush() }
        $line = $null
        if ($inputReader.Lines.TryTake([ref]$line)) {
            if ($null -eq $line.Text) { break }
            $nativePort.WriteAscii($line.Text + "`n")
        }
        Start-Sleep -Milliseconds 2
    }
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
} finally {
    if ($null -ne $nativePort) { $nativePort.Dispose() }
}
