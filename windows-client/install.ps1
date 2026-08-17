# Registers the virtual camera media source. MUST be run from an elevated PowerShell:
# a virtual camera source can only be registered in HKLM, and the DLL has to live
# somewhere the Frame Server service (LOCAL SERVICE) can read.
#Requires -RunAsAdministrator

$ErrorActionPreference = "Stop"

$source = Join-Path $PSScriptRoot "build\Release\awc-source.dll"
if (-not (Test-Path $source)) { throw "not built yet: $source" }

$targetDir = "C:\Program Files\AndroidWebCam"
$target = Join-Path $targetDir "awc-source.dll"

New-Item -ItemType Directory -Force -Path $targetDir | Out-Null

# The frame server keeps the source DLL loaded; stop it (and our app) so the file
# can be replaced. Both services are demand-start and come back automatically.
Get-Process awc-client -ErrorAction SilentlyContinue | Stop-Process -Force
foreach ($name in @("FrameServerMonitor", "FrameServer")) {
    $service = Get-Service $name -ErrorAction SilentlyContinue
    if ($service -and $service.Status -ne "Stopped") {
        Stop-Service $name -Force -ErrorAction SilentlyContinue
    }
}

$copied = $false
foreach ($attempt in 1..6) {
    try { Copy-Item $source $target -Force; $copied = $true; break } catch { Start-Sleep -Seconds 1 }
}
if (-not $copied) {
    throw "cannot overwrite $target - something still holds it. Close every app using the camera, then retry."
}

# The DLL runs as LOCAL SERVICE inside the frame server and logs here.
# Well-known SIDs, because account names are localised.
$logDir = "C:\ProgramData\AndroidWebCam"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
try {
    $acl = Get-Acl $logDir
    foreach ($sid in @("S-1-5-19", "S-1-5-20", "S-1-5-32-545")) {   # LOCAL SERVICE, NETWORK SERVICE, Users
        $id = New-Object System.Security.Principal.SecurityIdentifier($sid)
        $rule = New-Object System.Security.AccessControl.FileSystemAccessRule(
            $id, "Modify", "ContainerInherit,ObjectInherit", "None", "Allow")
        $acl.AddAccessRule($rule)
    }
    Set-Acl $logDir $acl
} catch {
    Write-Warning "cannot grant access to $logDir - the source DLL will not be able to log: $_"
}

# regsvr32 is a GUI-subsystem program: PowerShell does not wait for it and never
# fills $LASTEXITCODE, so wait explicitly and read the exit code from the process.
$proc = Start-Process regsvr32.exe -ArgumentList "/s", "`"$target`"" -Wait -PassThru
if ($proc.ExitCode -ne 0) { throw "regsvr32 failed with $($proc.ExitCode)" }

$clsid = "{8F3C7A21-5D64-4E9B-A17C-2B0E9D4F6A83}"
$key = "HKLM:\Software\Classes\CLSID\$clsid\InprocServer32"
if (Test-Path $key) {
    Write-Output "registered: $((Get-ItemProperty $key).'(default)')"
} else {
    throw "registration did not take"
}
