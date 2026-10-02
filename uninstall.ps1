# Fonix Jessica SAPI5 voice - uninstaller
# Usage:  powershell -ExecutionPolicy Bypass -File uninstall.ps1 [-NoPause]

param([switch]$NoPause)

$ErrorActionPreference = "Continue"

$isAdmin = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "Elevating..." -ForegroundColor Yellow
    $argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"")
    if ($NoPause) { $argList += "-NoPause" }
    Start-Process powershell.exe -Verb RunAs -ArgumentList $argList -Wait
    exit 0
}

$dest  = "${env:ProgramFiles(x86)}\FonixJessicaSAPI"
$dll   = "$dest\JessicaSAPI.dll"
$clsid = "{B3478C8E-CA57-4152-B62E-2CAC971CEC7F}"

Write-Host "Uninstalling Fonix Jessica SAPI5 voice..." -ForegroundColor Cyan
if (Test-Path $dll) {
    Write-Host "  regsvr32 /u $dll"
    Start-Process "$env:windir\SysWOW64\regsvr32.exe" -ArgumentList "/u","/s","`"$dll`"" -Wait
} else {
    Write-Host "  (DLL not at $dll - already uninstalled?)" -ForegroundColor Yellow
}

# Belt-and-suspenders: remove registry keys directly in case unregister missed them
$keysToDelete = @(
    "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Speech\Voices\Tokens\FonixJessica\Attributes",
    "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Speech\Voices\Tokens\FonixJessica",
    "HKLM:\SOFTWARE\Classes\WOW6432Node\CLSID\$clsid\InprocServer32",
    "HKLM:\SOFTWARE\Classes\WOW6432Node\CLSID\$clsid"
)
foreach ($k in $keysToDelete) {
    if (Test-Path $k) { Remove-Item $k -Recurse -Force -ErrorAction SilentlyContinue }
}

if (Test-Path $dest) {
    Write-Host "  Removing $dest"
    Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path $dest) {
        Write-Host "  Some files are still in use (close any app using the voice, then delete $dest)" -ForegroundColor Yellow
    }
}

Write-Host "Done." -ForegroundColor Green
if (-not $NoPause) { Read-Host "Press Enter to close" | Out-Null }
