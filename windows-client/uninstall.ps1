#Requires -RunAsAdministrator
$ErrorActionPreference = "Continue"

$target = "C:\Program Files\AndroidWebCam\awc-source.dll"
if (Test-Path $target) { & regsvr32.exe /u /s $target }

$clsid = "{8F3C7A21-5D64-4E9B-A17C-2B0E9D4F6A83}"
Remove-Item -Recurse -Force "HKLM:\Software\Classes\CLSID\$clsid" -ErrorAction SilentlyContinue
Remove-Item -Force $target -ErrorAction SilentlyContinue
Write-Output "unregistered"
