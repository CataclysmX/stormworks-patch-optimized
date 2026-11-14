# Merge the user's stwpatch.ini into the new template (same rules as tools/merge_ini.awk):
#   powershell -NoProfile -ExecutionPolicy Bypass -File merge_ini.ps1 <template> <user ini>
# The result follows the template (new options, comments, order); every key the user set keeps
# the user's line, also where the template only has it commented out (";threads=6"); user keys
# the template does not know are kept at the end of their section. The previous file is kept as
# stwpatch.ini.bak; nothing is written when the merge fails or changes nothing.
param([Parameter(Mandatory = $true)][string]$Template, [Parameter(Mandatory = $true)][string]$User)
$ErrorActionPreference = 'Stop'

function Get-Key([string]$line) {
    if ($line -match '^;?([A-Za-z_][A-Za-z0-9_]*)=') { return $Matches[1] }
    return $null
}

# the user's keys, by section
$entries = New-Object System.Collections.ArrayList
$index = @{}
$section = ''
foreach ($line in [IO.File]::ReadAllLines($User)) {
    if ($line -match '^\[.*\]$') { $section = $line; continue }
    if ($line -match '^[A-Za-z_][A-Za-z0-9_]*=') {
        $e = [pscustomobject]@{ Section = $section; Line = $line; Used = $false }
        [void]$entries.Add($e)
        $k = $section + "`n" + (Get-Key $line)
        if (-not $index.ContainsKey($k)) { $index[$k] = $e }
    }
}

$out = New-Object System.Collections.Generic.List[string]
function Add-Leftovers([string]$sec) {
    foreach ($e in $entries) {
        if ($e.Section -eq $sec -and -not $e.Used) { $out.Add($e.Line); $e.Used = $true }
    }
}

$section = ''
foreach ($line in [IO.File]::ReadAllLines($Template)) {
    if ($line -match '^\[.*\]$') {
        Add-Leftovers $section
        $section = $line
        $out.Add($line)
        continue
    }
    $key = Get-Key $line
    if ($key) {
        $k = $section + "`n" + $key
        if ($index.ContainsKey($k) -and -not $index[$k].Used) {
            $out.Add($index[$k].Line)
            $index[$k].Used = $true
            continue
        }
    }
    $out.Add($line)
}
Add-Leftovers $section
$printed = @{}
foreach ($e in $entries) {                     # sections the template no longer has
    if (-not $e.Used) {
        if (-not $printed.ContainsKey($e.Section)) { $out.Add(''); $out.Add($e.Section); $printed[$e.Section] = $true }
        $out.Add($e.Line)
        $e.Used = $true
    }
}

$old = [IO.File]::ReadAllLines($User)
if (($old -join "`n") -eq ($out -join "`n")) { exit 0 }
$utf8 = New-Object System.Text.UTF8Encoding $false
[IO.File]::WriteAllLines($User + '.new', $out, $utf8)
Copy-Item -LiteralPath $User -Destination ($User + '.bak') -Force
Move-Item -LiteralPath ($User + '.new') -Destination $User -Force
Write-Host '  stwpatch.ini updated: new options added, your values kept (previous file: stwpatch.ini.bak)'
