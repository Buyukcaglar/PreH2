# coverage.ps1 GAMEDIR - runs scripted sessions in the headless runner and collects the executed
# offsets of code segment 0020 into work\cov.txt (one hex offset per line, merged and sorted).
param([string]$Game = "C:/Projects/Prehistorik2/_references/dos/PREH2")
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
Remove-Item work\cov_raw.txt -ErrorAction SilentlyContinue
$env:P2_COV = "work/cov_raw.txt"
$env:P2_EVERY = "1000000"
$sessions = @(
    @(9000, "300:39:1,305:39:0"),
    @(9000, "300:39:1,305:39:0,1500:1c:1,1505:1c:0,2500:14d:1,4000:14d:0,2600:39:1,2610:39:0,2700:148:1,2720:148:0,3000:150:1,3100:150:0,3200:39:1,3210:39:0,4100:14b:1,4500:14b:0,4600:19:1,4610:19:0,4700:19:1,4710:19:0,4800:3d:1,4805:3d:0,4900:3b:1,4905:3b:0,5200:14d:1,8000:14d:0,5300:148:1,5320:148:0,5600:148:1,5620:148:0,6000:148:1,6020:148:0"),
    @(4000, "300:39:1,305:39:0,1500:3:1,1505:3:0,1600:19:1,1605:19:0,1610:1e:1,1615:1e:0,1620:1f:1,1625:1f:0,1630:1f:1,1635:1f:0,1700:1c:1,1705:1c:0,2500:1:1,2505:1:0"),
    @(5000, "300:39:1,305:39:0,1500:1c:1,1505:1c:0,2600:3c:1,2605:3c:0,3500:1:1,3505:1:0,4000:4b:1,4005:4b:0,4100:4d:1,4105:4d:0")
)
foreach ($s in $sessions) {
    & .\build\p2test.exe $Game $s[0] work/frames $s[1] 2>$null | Out-Null
}
Get-Content work\cov_raw.txt | Sort-Object -Unique | Set-Content work\cov.txt
"covered offsets: " + (Get-Content work\cov.txt).Count
