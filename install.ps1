# Fonix Jessica SAPI5 voice - installer
# Usage:  Double-click RUN_ME.cmd  (recommended, self-elevates)
#    or:  powershell -ExecutionPolicy Bypass -File install.ps1 [-NoPause]
#
# Installs to:  C:\Program Files (x86)\FonixJessicaSAPI\
# Registers:    "Fonix Jessica" as a SAPI5 voice (visible to 32-bit SAPI clients)

param([switch]$NoPause)

$ErrorActionPreference = "Stop"

function Wait-ForEnter {
    if (-not $NoPause) { Read-Host "Press Enter to close" | Out-Null }
}

try {

$isAdmin = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "This installer must be run as Administrator." -ForegroundColor Red
    Write-Host "Double-click RUN_ME.cmd instead (it self-elevates)." -ForegroundColor Yellow
    Wait-ForEnter
    exit 1
}

# $PSCommandPath can be empty in some launch contexts; fall back.
$scriptPath = if ($PSCommandPath) { $PSCommandPath } else { $MyInvocation.MyCommand.Path }
$pkgDir  = Split-Path -Parent $scriptPath
$dest    = "${env:ProgramFiles(x86)}\FonixJessicaSAPI"
$destDll = "$dest\JessicaSAPI.dll"
$rsvr    = "$env:windir\SysWOW64\regsvr32.exe"

Write-Host "Fonix Jessica SAPI5 installer" -ForegroundColor Cyan
Write-Host "  Source : $pkgDir"
Write-Host "  Target : $dest"
Write-Host ""

# 0) Refuse while a SAPI client still has the old engine loaded; deleting would stop midway.
$holders = Get-Process | Where-Object {
    try { $_.Modules | Where-Object { $_.FileName -like "$dest\*" } } catch { $false }
}
if ($holders) {
    Write-Host "These programs are using Fonix Jessica; close them and run the installer again:" -ForegroundColor Red
    $holders | ForEach-Object { Write-Host "  $($_.ProcessName) (PID $($_.Id))" }
    Wait-ForEnter
    exit 1
}

# 1) Unregister any prior install (idempotent)
if (Test-Path $destDll) {
    Write-Host "Existing install detected; unregistering first..." -ForegroundColor Yellow
    Start-Process $rsvr -ArgumentList "/u","/s","`"$destDll`"" -Wait
}

# 2) Copy files
Write-Host "Copying files..." -ForegroundColor Cyan
if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
New-Item -ItemType Directory -Path $dest -Force | Out-Null
Copy-Item "$pkgDir\JessicaSAPI.dll" $dest
Copy-Item "$pkgDir\lib"  $dest -Recurse
Copy-Item "$pkgDir\data" $dest -Recurse
Copy-Item "$pkgDir\uninstall.ps1" $dest
Copy-Item "$pkgDir\README.md"     $dest -ErrorAction SilentlyContinue
Write-Host "  $((Get-ChildItem $dest -Recurse -File).Count) files installed"

# 3) Register via the 32-bit regsvr32 (CLSID + voice token land in WOW6432Node).
#    regsvr32 sometimes returns non-zero on success, so the registry is the real check.
Write-Host "Registering voice with SAPI..." -ForegroundColor Cyan
$r = Start-Process $rsvr -ArgumentList "/s","`"$destDll`"" -PassThru -Wait
if ($r.ExitCode -ne 0) {
    Write-Host "  regsvr32 returned non-zero ($($r.ExitCode)); checking registry to confirm..." -ForegroundColor Yellow
}

# 4) Verify
$tokenKey = "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Speech\Voices\Tokens\FonixJessica"
if (Test-Path $tokenKey) {
    Write-Host "OK: voice token registered" -ForegroundColor Green
    Write-Host ""
    Write-Host "Asking 32-bit SAPI for voice list..." -ForegroundColor Cyan
    $ps32  = "$env:windir\SysWOW64\WindowsPowerShell\v1.0\powershell.exe"
    $check = "`$hit = `$false; foreach (`$t in (New-Object -ComObject SAPI.SpVoice).GetVoices()) { if (`$t.GetAttribute('Name') -eq 'Fonix Jessica') { `$hit = `$true } }; " +
             "if (`$hit) { Write-Host '  [OK] Fonix Jessica' -ForegroundColor Green } else { Write-Host '  Fonix Jessica NOT visible to SAPI' -ForegroundColor Red }"
    & $ps32 -NoProfile -Command $check
} else {
    Write-Host "Voice token NOT found - registration may have failed" -ForegroundColor Red
}

Write-Host ""
Write-Host "Install complete." -ForegroundColor Green
Write-Host "Quick test (paste into PowerShell):"
Write-Host "  & `"C:\Windows\SysWOW64\WindowsPowerShell\v1.0\powershell.exe`" -Command '`$v = New-Object -ComObject SAPI.SpVoice; `$v.Voice = `$v.GetVoices(''Name=Fonix Jessica'').Item(0); `$v.Speak(''Hello from Jessica.'')'"
Write-Host ""
Write-Host "To uninstall later, run uninstall.ps1 (kept in $dest)."

} catch {
    Write-Host ""
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host $_.ScriptStackTrace -ForegroundColor DarkGray
    Wait-ForEnter
    exit 1
}

Wait-ForEnter
