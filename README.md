# Afterburner Bridge

Brings MSI Afterburner's sensors into Corsair iCUE 5.

iCUE only offers its own fixed list of sensors, and some of the ones you
probably want are not in it: GPU power draw, framerate, core clocks, VRAM
usage. Afterburner already measures all of that and publishes it for other
programs to read. This plugin reads it and republishes it as an iCUE device.

Once inside iCUE the values behave like any other sensor. You can put them on
the dashboard, set alert thresholds on them, and use them as triggers for
lighting effects, so the keyboard can react to actual GPU wattage, or to the
framerate dropping.

Whatever you tick in Afterburner's **Monitoring** tab shows up. Tick something
new, restart iCUE, and it is there. Nothing to configure in the plugin.

Nothing in the iCUE installation is modified.

---

## Requirements

- Windows, 64-bit
- Corsair iCUE 5
- MSI Afterburner, running, along with the RivaTuner Statistics Server that
  ships with it. Afterburner's monitoring is the data source, so while it is
  closed the plugin has nothing to publish.
- iCUE's plugin host service. See *Troubleshooting* if you have never
  installed an iCUE device plugin.

Built and tested against iCUE 5.43 and 5.51.

## Installing

Download the release, or build it yourself (see below), then from an
**elevated PowerShell** in the folder containing the files:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
.\install.ps1
```

The installer closes iCUE, because the plugin is only loaded when the process
starts. Start iCUE again afterwards and the device appears as **Afterburner
Bridge** with its sensors under it.

The installer finds the iCUE folder on its own, asking the plugin host
service where it runs from and falling back to the running iCUE process, the
uninstall entry, and the usual locations. If your installation is somewhere
it cannot work out, point at it:

```powershell
.\install.ps1 -ICueDir "D:\Games\Corsair iCUE5 Software"
```

### About the signature

iCUE refuses to load a plugin whose Authenticode signature does not chain to
a root this machine trusts. It does not check *who* signed it, so the
installer generates its own certificate, signs the DLL, trusts the public
half on this machine, and then **destroys the private key**.

That last step matters. Signing needs the private key, verifying needs only
the public half. Leaving the key on disk would leave behind the ability to
sign anything in a way Windows trusts. Once it is gone, that signature cannot
be reproduced, not even by you. Updating the plugin simply generates a new
certificate, which is what the installer does on every run.

Pass `-KeepKey` to keep it while developing.

## Settings

`AfterburnerBridge.ini`, next to the DLL in the plugin folder. The installer
leaves an existing one alone, so your settings survive an update; pass
`-ResetIni` to replace it with the shipped defaults, which keeps the old one
as `.bak`. Changes take effect when iCUE restarts.

| Key | Meaning |
|---|---|
| `[device] name` | Name shown in iCUE. |
| `[sensors] show_not_compatible` | `false` publishes only entries iCUE can label correctly. |
| `[display] unit_in_name` | Append the real unit to the sensor name when iCUE cannot label it. |
| `[units]` | Pick which label iCUE puts on a unit it has none for, e.g. `MHz = A`. |
| `[log] enabled` | A plain text log of what was published and why, written to `%TEMP%\AfterburnerBridge.log`. |

The file itself documents every key in more detail.

## The units limitation

**iCUE can only label five units: degrees, RPM, V, A and W.** That is not a
setting, it is the whole vocabulary the plugin interface exposes. Power,
voltage, current, fan speed and temperature therefore come out exactly right.

Fan speeds are published as such, so they carry the right label. That also
makes iCUE attach its Cooling and Alerts pages to the device: both come from
that one sensor type and cannot be separated.

Percentages, MHz, MB and FPS have no label available, so those sensors borrow
a type and iCUE prints that type's label next to the number. The value, the
graph and the thresholds are all correct; only the two characters after the
number are wrong. By default the plugin writes the real unit into the sensor
name instead, so it reads `Framerate (FPS)`.

If you would rather see a specific wrong label than degrees, `[units]` lets
you choose which one. And `show_not_compatible = false` drops those entries
altogether, leaving only the sensors iCUE labels properly, which is a
reasonable setup, since iCUE already shows CPU and GPU load natively.

## How it works

Afterburner publishes everything it monitors in a shared memory section named
`MAHMSharedMemory`: the same numbers its graphs and the RivaTuner overlay
show. The plugin maps it read-only and, for each entry, takes the name, the
unit, the value, the limits and the recommended number of decimals.

On the iCUE side it implements the undocumented device-plugin interface, the
same one ASUS and NVIDIA use, and presents a single device whose sensors are
those entries. The sensor type is derived from the unit; the range comes from
Afterburner's own limits, widened when they turn out to be narrower than the
live value.

The device id contains a fingerprint of the sensor list. iCUE caches a
device's sensors and reads the values back by position, so if the list changed
underneath it you would see one sensor's number under another's name. A
different list yields a different id, which iCUE treats as a new device with
nothing cached. The cost is that changing the list loses the dashboard tiles
referring to the old one.

If Afterburner is not running, the plugin falls back to publishing GPU power
alone, read through NVIDIA's NVML.

The device pictures go through the same interface. Each one is a path
relative to the plugin's own folder, paired with the SHA-256 of that file:
iCUE hashes the picture itself and discards it unless the two match, so the
plugin computes the digest at startup. An absolute path is refused.

## Troubleshooting

Start with the log. `[log] enabled = 1` writes one line per sensor with its
name, unit, type and range, plus the reason anything was skipped. It lands in
`%TEMP%\AfterburnerBridge.log`, not in the plugin folder: whether that folder
is writable depends on how iCUE was started.

**The device does not appear at all.** Check that the plugin host service
exists:

```powershell
Get-Service iCUEDevicePluginHost
```

iCUE installs that service as a dependency of a device plugin, so on a system
that never had one it is missing, and removing the last one takes it away
again. You can install it on its own with iCUE's package manager, from an
elevated PowerShell with iCUE closed:

```powershell
$dir = Split-Path (Get-Process iCUE).Path -Parent   # or set it by hand
$pkg = (Get-ChildItem $dir -Filter cuepkg.exe -Recurse | Select-Object -First 1).FullName
& $pkg --installdir $dir --format plain install plugin-host-service
```

That needs iCUE running so the first line can find the folder; otherwise set
`$dir` yourself.

**Only "GPU Power" appears.** Afterburner was not running when iCUE started
the plugin. Start Afterburner first, then restart iCUE.

**Changes to the settings do not take effect.** Closing the iCUE window is
not enough: it keeps running in the tray, and the plugin is only reloaded when
the process actually exits. Quit it from the tray icon, or just run the
installer again, which closes it for you.

**A sensor reads zero.** Check that the entry is still ticked in Afterburner's
Monitoring tab. The log names any entry that disappeared.

## Uninstalling

```powershell
.\install.ps1 -Uninstall
```

Removes the plugin folder and the certificate from the machine's trusted
stores. Restart iCUE afterwards.

## Building

Any MinGW-w64 toolchain will do. On Windows the simplest is
[w64devkit](https://github.com/skeeto/w64devkit): unpack it, run
`w64devkit.exe`, and you get a shell with `gcc` and `windres` ready. MSYS2
with the `mingw-w64-x86_64-gcc` package works the same way.

From the folder holding the sources:

```
windres afterburner-bridge.rc -O coff -o ab_rc.o
gcc -O1 -shared -o AfterburnerBridge.dll afterburner-bridge.c ab_rc.o -static-libgcc
```

Cross-compiling from Linux is the same with the prefixed tools,
`x86_64-w64-mingw32-gcc` and `x86_64-w64-mingw32-windres`.

It must be a 64-bit build; a 32-bit DLL will not load. `-O1` is what every
tested build used. Higher levels work too, but there is no reason to go
looking for trouble in a DLL that runs inside someone else's process.

The `.rc` step is optional: without it the plugin still works, but iCUE shows
no version for it.

There are no dependencies beyond the Windows API. NVML is resolved at
runtime, so it is not needed to build.

## Notes

Not affiliated with, endorsed by or supported by Corsair or MSI. It relies on
an interface Corsair does not document, which means an iCUE update could
change it. It reads Afterburner's shared memory and nothing else.

The interface was worked out by reverse engineering, with Claude's help.
