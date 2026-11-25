param(
    [Parameter(Mandatory = $false)]
    [string]$Executable = (Join-Path $PSScriptRoot "..\build\windows-v143-x86\Release\sfx_storage_constrained_measurements.exe"),

    [Parameter(Mandatory = $false)]
    [string]$EvidenceDirectory = (Join-Path $PSScriptRoot "..\build\stage5a-constrained-final"),

    [ValidateRange(1, 20)]
    [int]$Runs = 5
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$memoryLimitBytes = 128MB
$workingSetAcceptanceBytes = 128MB
$maximumWallMilliseconds = 30000
$passMarker = "all SFX Stage 5A constrained measurements passed"

$source = @"
using System;
using System.Runtime.InteropServices;

namespace SfxStage5A {
    [StructLayout(LayoutKind.Sequential)]
    public struct BasicLimitInformation {
        public long PerProcessUserTimeLimit;
        public long PerJobUserTimeLimit;
        public UInt32 LimitFlags;
        public UIntPtr MinimumWorkingSetSize;
        public UIntPtr MaximumWorkingSetSize;
        public UInt32 ActiveProcessLimit;
        public UIntPtr Affinity;
        public UInt32 PriorityClass;
        public UInt32 SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct IoCounters {
        public UInt64 ReadOperationCount;
        public UInt64 WriteOperationCount;
        public UInt64 OtherOperationCount;
        public UInt64 ReadTransferCount;
        public UInt64 WriteTransferCount;
        public UInt64 OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct ExtendedLimitInformation {
        public BasicLimitInformation BasicLimitInformation;
        public IoCounters IoInfo;
        public UIntPtr ProcessMemoryLimit;
        public UIntPtr JobMemoryLimit;
        public UIntPtr PeakProcessMemoryUsed;
        public UIntPtr PeakJobMemoryUsed;
    }

    public static class NativeJob {
        private const UInt32 JobObjectLimitAffinity = 0x00000010;
        private const UInt32 JobObjectLimitPriorityClass = 0x00000020;
        private const UInt32 JobObjectLimitProcessMemory = 0x00000100;
        private const UInt32 BelowNormalPriorityClass = 0x00004000;
        private const Int32 JobObjectExtendedLimitInformation = 9;

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr CreateJobObject(IntPtr securityAttributes, string name);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool SetInformationJobObject(
            IntPtr job,
            Int32 informationClass,
            ref ExtendedLimitInformation information,
            UInt32 informationLength);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool CloseHandle(IntPtr handle);

        public static bool Configure(IntPtr job, UInt64 processMemoryLimitBytes) {
            ExtendedLimitInformation information = new ExtendedLimitInformation();
            information.BasicLimitInformation.LimitFlags =
                JobObjectLimitAffinity |
                JobObjectLimitPriorityClass |
                JobObjectLimitProcessMemory;
            information.BasicLimitInformation.Affinity = (UIntPtr)1;
            information.BasicLimitInformation.PriorityClass = BelowNormalPriorityClass;
            information.ProcessMemoryLimit = (UIntPtr)processMemoryLimitBytes;
            return SetInformationJobObject(
                job,
                JobObjectExtendedLimitInformation,
                ref information,
                (UInt32)Marshal.SizeOf(typeof(ExtendedLimitInformation)));
        }
    }
}
"@

Add-Type -TypeDefinition $source -Language CSharp

$executablePath = (Resolve-Path -LiteralPath $Executable).Path
$evidencePath = [IO.Path]::GetFullPath($EvidenceDirectory)
if (Test-Path -LiteralPath $evidencePath) {
    $existing = @(Get-ChildItem -LiteralPath $evidencePath -Force)
    if ($existing.Count -ne 0) {
        throw "Evidence directory must be absent or empty: $evidencePath"
    }
} else {
    [void](New-Item -ItemType Directory -Path $evidencePath)
}

$results = @()
for ($run = 1; $run -le $Runs; ++$run) {
    $runDataPath = Join-Path $evidencePath ("run-{0}-data" -f $run)
    $stdoutPath = Join-Path $evidencePath ("run-{0}.stdout.txt" -f $run)
    $stderrPath = Join-Path $evidencePath ("run-{0}.stderr.txt" -f $run)
    $job = [SfxStage5A.NativeJob]::CreateJobObject([IntPtr]::Zero, $null)
    if ($job -eq [IntPtr]::Zero) {
        throw "CreateJobObject failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }

    try {
        if (-not [SfxStage5A.NativeJob]::Configure($job, [UInt64]$memoryLimitBytes)) {
            throw "SetInformationJobObject failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
        }

        $watch = [Diagnostics.Stopwatch]::StartNew()
        $startArguments = @{
            FilePath = $executablePath
            ArgumentList = @($runDataPath)
            PassThru = $true
            WindowStyle = "Hidden"
            RedirectStandardOutput = $stdoutPath
            RedirectStandardError = $stderrPath
        }
        $process = Start-Process @startArguments
        try {
            if (-not [SfxStage5A.NativeJob]::AssignProcessToJobObject($job, $process.Handle)) {
                $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
                if (-not $process.HasExited) {
                    $process.Kill()
                }
                throw "AssignProcessToJobObject failed: $errorCode"
            }

            [Int64]$peakWorkingSet = 0
            [Int64]$peakPrivateBytes = 0
            while (-not $process.HasExited) {
                $process.Refresh()
                $peakWorkingSet = [Math]::Max($peakWorkingSet, $process.WorkingSet64)
                $peakPrivateBytes = [Math]::Max($peakPrivateBytes, $process.PrivateMemorySize64)
                Start-Sleep -Milliseconds 25
            }
            $process.WaitForExit()
            $process.Refresh()
            $peakWorkingSet = [Math]::Max($peakWorkingSet, $process.PeakWorkingSet64)
            $peakPrivateBytes = [Math]::Max($peakPrivateBytes, $process.PrivateMemorySize64)
            $watch.Stop()

            $stdout = [IO.File]::ReadAllText($stdoutPath)
            $stderr = [IO.File]::ReadAllText($stderrPath)
            if ($process.ExitCode -ne 0) {
                throw "Run $run exited $($process.ExitCode): $stderr"
            }
            if (-not $stdout.Contains($passMarker)) {
                throw "Run $run did not emit the required pass marker"
            }
            if ($watch.ElapsedMilliseconds -ge $maximumWallMilliseconds) {
                throw "Run $run exceeded the $maximumWallMilliseconds ms wall-time limit"
            }
            if ($peakWorkingSet -gt $workingSetAcceptanceBytes) {
                throw "Run $run exceeded the $workingSetAcceptanceBytes-byte working-set limit"
            }
            if ($peakPrivateBytes -gt $memoryLimitBytes) {
                throw "Run $run exceeded the enforced $memoryLimitBytes-byte private-memory limit"
            }

            $results += [PSCustomObject]@{
                Run = $run
                ExitCode = $process.ExitCode
                WallMilliseconds = $watch.ElapsedMilliseconds
                PeakWorkingSetBytes = $peakWorkingSet
                PeakPrivateBytes = $peakPrivateBytes
                ProcessorAffinityMask = 1
                PriorityClass = "BelowNormal"
                ProcessMemoryLimitBytes = $memoryLimitBytes
                Passed = $true
            }
        } finally {
            if ($null -ne $process) {
                $process.Dispose()
            }
        }
    } finally {
        [void][SfxStage5A.NativeJob]::CloseHandle($job)
    }
}

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $executablePath).Hash
$environment = [PSCustomObject]@{
    QualifiedAtUtc = [DateTime]::UtcNow.ToString("o")
    OperatingSystem = [Environment]::OSVersion.VersionString
    Is64BitOperatingSystem = [Environment]::Is64BitOperatingSystem
    Is64BitProcess = [Environment]::Is64BitProcess
    LogicalProcessorCount = [Environment]::ProcessorCount
    QualificationExecutable = $executablePath
    QualificationExecutableSha256 = $hash
    Runs = $Runs
    ProcessorAffinityMask = 1
    PriorityClass = "BelowNormal"
    ProcessMemoryLimitBytes = $memoryLimitBytes
    WorkingSetAcceptanceBytes = $workingSetAcceptanceBytes
    MaximumWallMilliseconds = $maximumWallMilliseconds
    Results = $results
}
$environment | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidencePath "summary.json") -Encoding UTF8
$results | Export-Csv -LiteralPath (Join-Path $evidencePath "summary.csv") -NoTypeInformation -Encoding UTF8
$results | Format-Table -AutoSize
Write-Output "SFX Stage 5A bounded constrained-system qualification PASSED"
Write-Output "evidence=$evidencePath"
Write-Output "executable_sha256=$hash"

