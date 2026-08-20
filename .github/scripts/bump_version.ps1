# bump_version.ps1
# Reads the VERSION file at the repo root (e.g. "1.2.3"), increments it and
# writes the new value back. Prints the new version on stdout.
#
# Bump rules (highest priority first):
#   - commit message contains "bump major", "[major]" or "BREAKING" -> major +1
#   - commit message contains "bump minor" or "[minor]"              -> minor +1
#   - otherwise                                                      -> patch +1
#
# Pure ASCII on purpose (PowerShell 5.1 reads BOM-less .ps1 as ANSI).
param(
    [string]$VersionFile = (Join-Path $PSScriptRoot '..\..\VERSION')
)
$ErrorActionPreference = 'Stop'

if (Test-Path -LiteralPath $VersionFile) {
    $ver = (Get-Content -LiteralPath $VersionFile -Raw).Trim()
} else {
    $ver = '1.0.0'
}
$ver = $ver.TrimStart('v')

$parts = @($ver -split '\.')
$nums = @()
foreach ($p in $parts) {
    if ($p -match '^\d+$') { $nums += [int]$p } else { $nums += 0 }
}
while ($nums.Count -lt 3) { $nums += 0 }
$major = $nums[0]; $minor = $nums[1]; $patch = $nums[2]

$kind = 'patch'
$msg = $env:COMMIT_MSG
if ($msg) {
    if ($msg -match '\[major\]|bump\s+major|BREAKING') { $kind = 'major' }
    elseif ($msg -match '\[minor\]|bump\s+minor') { $kind = 'minor' }
}

switch ($kind) {
    'major' { $major++; $minor = 0; $patch = 0 }
    'minor' { $minor++; $patch = 0 }
    default { $patch++ }
}

$newVer = "$major.$minor.$patch"
Set-Content -LiteralPath $VersionFile -Value $newVer -Encoding ASCII -NoNewline
Write-Output $newVer