<#
    Afterburner Bridge - installer for the iCUE plugin

    What it does, in order:
      1. generates a fresh self-signed code-signing certificate whose
         private key cannot be exported
      2. installs the public half of it among this machine's trusted roots
      3. signs AfterburnerBridge.dll
      4. destroys the private key
      5. stops the iCUE plugin host, copies the plugin in and restarts it

    Why the certificate is needed
    iCUE calls WinVerifyTrust before loading a plugin and will not open one
    whose signature does not chain to a root this machine trusts. It does
    not check who signed it, so a certificate generated here is enough.

    Why the private key is destroyed
    Signing needs the private key; verifying needs only the public half.
    Leaving the key behind would leave the ability to sign anything on this
    machine in a way Windows trusts. Once it is gone that signature cannot
    be reproduced by anyone, and updating the plugin simply generates a new
    certificate - which is what this script does on every run.

    Pass -KeepKey to keep the key, which is convenient while developing but
    leaves the machine in a worse state.

    Requires an elevated PowerShell session.

    Usage:
      .\install.ps1                 install or update
      .\install.ps1 -Uninstall      remove the plugin and the certificate
      .\install.ps1 -ResetIni       also replace the installed settings file

    iCUE is always closed. It has to be: the plugin is only reloaded when the
    process actually exits, and closing its window merely sends it to the
    tray. Start it again once the script is done.

    The iCUE folder is detected automatically. If that fails, point at it:
      .\install.ps1 -ICueDir "D:\Corsair iCUE5 Software"
#>

param(
    [switch]$Uninstall,
    [switch]$KeepKey,
    [switch]$ResetIni,
    [string]$ICueDir = "",     # leave empty to detect it; see Find-ICueDir
    [string]$Subject = "CN=Afterburner Bridge (local signing)"
)

$ErrorActionPreference = "Stop"

# iCUE is not always under C:\Program Files. Ask Windows where it is, in
# descending order of reliability, and fall back to the usual locations.
function Find-ICueDir {
    $found = New-Object System.Collections.Generic.List[string]

    # The plugin host service runs from the install folder, and it is the
    # component that actually loads this plugin.
    $svc = Get-CimInstance Win32_Service -Filter "Name='iCUEDevicePluginHost'" -ErrorAction SilentlyContinue
    if ($svc -and $svc.PathName) {
        $exe = ($svc.PathName -replace '^"([^"]+)".*$', '$1') -replace '^([^\s]+)\s.*$', '$1'
        if (Test-Path $exe) { $found.Add((Split-Path $exe -Parent)) }
    }

    # A running iCUE tells us directly.
    $proc = Get-Process -Name "iCUE" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($proc -and $proc.Path) { $found.Add((Split-Path $proc.Path -Parent)) }

    # The uninstall entry Windows shows in Apps and Features.
    $keys = @(
        "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*",
        "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*"
    )
    Get-ItemProperty $keys -ErrorAction SilentlyContinue |
        Where-Object { $_.DisplayName -like "*iCUE*" -and $_.InstallLocation } |
        ForEach-Object { $found.Add($_.InstallLocation.TrimEnd('\')) }

    # Last resort.
    foreach ($base in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if ($base) { $found.Add((Join-Path $base "Corsair\Corsair iCUE5 Software")) }
    }

    foreach ($d in $found) {
        if ($d -and (Test-Path (Join-Path $d "iCUE.exe"))) { return $d }
    }
    return $null
}

if (-not $ICueDir) {
    $ICueDir = Find-ICueDir
    if (-not $ICueDir) {
        throw ("Could not find the iCUE installation. Pass it explicitly, e.g. " +
               "  .\install.ps1 -ICueDir 'D:\Games\Corsair iCUE5 Software'")
    }
    Write-Host "[*] Found iCUE in $ICueDir"
} elseif (-not (Test-Path (Join-Path $ICueDir "iCUE.exe"))) {
    throw "No iCUE.exe in $ICueDir - check the -ICueDir path."
}

$Vendor    = "AfterburnerBridge"
$IniName   = "AfterburnerBridge.ini"
$Service   = "iCUEDevicePluginHost"

# Everything the installer copies is taken from the script's own folder, not
# from the shell's current directory: an elevated PowerShell usually starts in
# system32, and reading from there would silently install nothing.
$Here      = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
$Dll       = Join-Path $Here "AfterburnerBridge.dll"

$PluginDir = Join-Path $ICueDir "plugins\$Vendor"
$Target    = Join-Path $PluginDir "AfterburnerBridge.dll"
$IniDst    = Join-Path $PluginDir $IniName

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "This script needs an elevated PowerShell session."
}

function Remove-PluginCerts {
    Get-ChildItem Cert:\LocalMachine\Root, Cert:\LocalMachine\TrustedPublisher,
                  Cert:\CurrentUser\My -ErrorAction SilentlyContinue |
        Where-Object { $_.Subject -eq $Subject } |
        ForEach-Object {
            Write-Host "    removing certificate $($_.Thumbprint)"
            Remove-Item $_.PSPath -Force -ErrorAction SilentlyContinue
        }
}

# Anything that might hold the DLL open. Plugins run in child processes of
# the host service, so stopping the service alone is not enough.
function Stop-PluginHost {
    if (Get-Service -Name $Service -ErrorAction SilentlyContinue) {
        Write-Host "    stopping service $Service"
        Stop-Service $Service -Force -ErrorAction SilentlyContinue
    }
    $procs = Get-Process -Name $Service -ErrorAction SilentlyContinue
    if ($procs) {
        Write-Host "    closing $($procs.Count) plugin container process(es)"
        $procs | Stop-Process -Force -ErrorAction SilentlyContinue
    }
    $icue = Get-Process -Name "iCUE" -ErrorAction SilentlyContinue
    if ($icue) { Write-Host "    closing iCUE"; $icue | Stop-Process -Force }
    Start-Sleep -Seconds 2
}

# ---------------------------------------------------------------- uninstall

if ($Uninstall) {
    Write-Host "[*] Removing the plugin..."
    Stop-PluginHost
    if (Test-Path $PluginDir) {
        Write-Host "    removing $PluginDir"
        Remove-Item $PluginDir -Recurse -Force
    }
    Remove-PluginCerts
    Write-Host "[OK] Removed. Restart iCUE."
    return
}

if (-not (Test-Path $Dll)) {
    throw "AfterburnerBridge.dll not found in $Here. Keep it next to this script."
}

# ------------------------------------------------------------ certificate

# Always start clean: last run's private key no longer exists, so there is
# nothing to reuse and no reason to keep its public half around.
Write-Host "[*] Clearing any previous certificate..."
Remove-PluginCerts

Write-Host "[*] Generating a code-signing certificate..."
$cert = New-SelfSignedCertificate -Subject $Subject `
    -Type CodeSigningCert -KeyUsage DigitalSignature `
    -KeyAlgorithm RSA -KeyLength 3072 `
    -KeyExportPolicy NonExportable `
    -CertStoreLocation Cert:\CurrentUser\My `
    -NotAfter (Get-Date).AddYears(3)
Write-Host "    thumbprint: $($cert.Thumbprint)"

# This has to happen before signing. Set-AuthenticodeSignature reports the
# result of the verification it performs afterwards, not of the signing, and
# until the certificate is trusted that verification fails with "a
# certificate chain processed but terminated in a root certificate which is
# not trusted". Only the public half is installed, so the order costs
# nothing in terms of safety.
Write-Host "[*] Trusting the public half on this machine..."
$tmpCer = Join-Path $env:TEMP "afterburner-bridge-cert.cer"
Export-Certificate -Cert $cert -FilePath $tmpCer -Force | Out-Null
foreach ($store in @("Root", "TrustedPublisher")) {
    Import-Certificate -FilePath $tmpCer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
    Write-Host "    -> LocalMachine\$store"
}
Remove-Item $tmpCer -Force

Write-Host "[*] Signing $Dll..."
$sig = Set-AuthenticodeSignature -FilePath $Dll -Certificate $cert `
        -HashAlgorithm SHA256 `
        -TimestampServer "http://timestamp.digicert.com" -ErrorAction Continue
if ($sig.Status -ne "Valid") {
    Write-Host "    timestamping did not work, signing without it"
    $sig = Set-AuthenticodeSignature -FilePath $Dll -Certificate $cert -HashAlgorithm SHA256
}
Write-Host "    signature: $($sig.Status)"
if ($sig.Status -ne "Valid") { throw "Signing failed: $($sig.StatusMessage)" }

# Last step that needs the private key, so it can go now.
if ($KeepKey) {
    Write-Warning "-KeepKey: the private key stays in Cert:\CurrentUser\My."
} else {
    Write-Host "[*] Destroying the private key..."
    Get-ChildItem Cert:\CurrentUser\My |
        Where-Object { $_.Thumbprint -eq $cert.Thumbprint } |
        ForEach-Object { Remove-Item $_.PSPath -Force }
    $left = Get-ChildItem Cert:\CurrentUser\My |
            Where-Object { $_.Thumbprint -eq $cert.Thumbprint }
    if ($left) { Write-Warning "the private key is still there" }
    else       { Write-Host "    gone: that signature can no longer be reproduced" }
}

# The signature must still verify without the key.
$check = Get-AuthenticodeSignature -FilePath $Dll
Write-Host "    signature after key removal: $($check.Status)"

# ---------------------------------------------------------------- install

Write-Host "[*] Stopping the plugin host..."
Stop-PluginHost

Write-Host "[*] Installing into $PluginDir ..."
New-Item -ItemType Directory -Force -Path $PluginDir | Out-Null

$copied = $false
for ($i = 1; $i -le 5; $i++) {
    try {
        Copy-Item $Dll $Target -Force
        $copied = $true
        break
    } catch {
        Write-Host "    attempt $i failed (file in use), retrying"
        Stop-PluginHost
        Start-Sleep -Seconds 2
    }
}
if (-not $copied) {
    throw ("Cannot overwrite ${Target}: it is still in use. " +
           "Quit iCUE from its tray icon and run this again.")
}

# The INI is not overwritten by default: it holds the user's settings, and
# updating the plugin should not discard them. -ResetIni replaces it, keeping
# a .bak copy of the old one.
$iniSrc = Join-Path $Here $IniName
if (Test-Path $iniSrc) {
    if ((Test-Path $IniDst) -and -not $ResetIni) {
        Write-Host "    $IniName already there, leaving it alone (pass -ResetIni to replace it)"
    } else {
        if (Test-Path $IniDst) {
            Copy-Item $IniDst "$IniDst.bak" -Force
            Write-Host "    previous settings saved as $IniName.bak"
        }
        Copy-Item $iniSrc $IniDst -Force
        Write-Host "    $IniName -> $IniDst"
    }
} else {
    Write-Warning "$IniName not found in $Here; the plugin will use its defaults"
}

# Device artwork. iCUE asks for two pictures and they want different framing:
# the small one in its device list and the large one on the device page. Each
# is optional; a missing file means iCUE shows no picture in that spot. Every
# one is paired with its SHA-256 by the plugin, so replacing a picture needs
# no other change.
$art = @(
    @{ src = "device-thumbnail.png"; dst = "AfterburnerBridge-thumbnail.png"  },
    @{ src = "device-promo.png";     dst = "AfterburnerBridge-promo.png"      }
)
$artCopied = 0
foreach ($a in $art) {
    $src = Join-Path $Here $a.src
    if (Test-Path $src) {
        Copy-Item $src (Join-Path $PluginDir $a.dst) -Force
        Write-Host "    artwork -> $($a.dst)"
        $artCopied++
    }
}
if ($artCopied -eq 0) {
    Write-Host "    no device-*.png found in $Here, the device will have no picture"
}

Write-Host "[*] Restarting $Service ..."
if (Get-Service -Name $Service -ErrorAction SilentlyContinue) {
    Start-Service $Service -ErrorAction SilentlyContinue
} else {
    Write-Warning "Service $Service is not installed. iCUE installs it with the first device plugin; see the README."
}

Write-Host ""
Write-Host "[OK] Done."
Write-Host "     Settings: $IniDst"
Write-Host ""
Write-Host "     Start iCUE again."
Write-Host "     Make sure MSI Afterburner is running before iCUE reads the sensors."
