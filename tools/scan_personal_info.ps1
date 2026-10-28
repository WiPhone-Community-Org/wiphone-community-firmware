<#
.SYNOPSIS
Windows PowerShell launcher for the repository personal-information scanner.

.EXAMPLE
.\tools\scan_personal_info.ps1
.EXAMPLE
.\tools\scan_personal_info.ps1 -Root . -Json scan-report.json

Exit codes: 0 = no high-risk findings, 1 = high-risk findings, 2 = invalid usage.
#>
param(
    [string]$Root = ".",
    [string]$Json,
    [string[]]$Exclude = @(),
    [double]$MaxFileMb = 50,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RemainingArguments
)

$ErrorActionPreference = "Stop"
$pythonArgs = @()
if ($env:PYTHON) {
    $pythonCommand = $env:PYTHON
} elseif (Get-Command py -ErrorAction SilentlyContinue) {
    $pythonCommand = "py"
    $pythonArgs += "-3"
} elseif (Get-Command python -ErrorAction SilentlyContinue) {
    $pythonCommand = "python"
} elseif (Get-Command python3 -ErrorAction SilentlyContinue) {
    $pythonCommand = "python3"
} else {
    Write-Error "Python 3 is required but was not found in PATH"
    exit 2
}
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$core = Join-Path $scriptDir "scan_personal_info.py"
$argsList = $pythonArgs + @($core, $Root)

if ($Json) { $argsList += @("--json", $Json) }
foreach ($item in $Exclude) { $argsList += @("--exclude", $item) }
$argsList += @("--max-file-mb", $MaxFileMb.ToString([System.Globalization.CultureInfo]::InvariantCulture))
if ($RemainingArguments) { $argsList += $RemainingArguments }

& $pythonCommand @argsList
exit $LASTEXITCODE
