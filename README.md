# Cyclops

A small Windows utility that opens the IR half of a Windows Hello camera: live preview, an illuminator switch, and an optional virtual webcam so any app can use the IR feed.

Windows Hello IR cameras live in `KSCATEGORY_SENSOR_CAMERA`, which DirectShow does not enumerate - so the IR sensor you already own is invisible to camera apps. Cyclops opens it through Media Foundation instead.

## Features

- Live grayscale preview of the IR camera (L8/NV12, typically 640x360 @ 30fps)
- IR illuminator on/off switch (via the `KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE` extended camera control)
- Optional virtual camera, "Cyclops (Windows Hello camera passthrough)", visible to every Windows camera app (Camera, OBS, browsers, games) in the installing user's session (the COM registration is machine-wide, the device entry is per user). Advertises NV12 only: RGB32 reaches DirectShow clients as a negative-height `BI_RGB`, which hangs some apps' camera enumeration.
- Webcam toggle is unelevated once installed; install/uninstall asks for elevation once (HKLM COM registration)

## Requirements

- Windows 11, build 22000 or newer (the virtual camera API is `MFCreateVirtualCamera`)
- A Windows Hello IR camera that Media Foundation enumerates under `KSCATEGORY_SENSOR_CAMERA`

Tested only on a **NexiGo HelloCam N930W**. Other Hello cameras should work if they expose an IR video stream and the FACEAUTH extended control, but that is not verified - reports welcome.

## Downloads

Prebuilt zips are on the [Releases](https://github.com/baanish/Cyclops/releases) page: tagged versions are stable releases, and the `nightly` pre-release is refreshed on every push to `main`. Unzip anywhere and run `cyclops.exe`; the Qt runtime is included.

## Building

Needs MSVC, CMake >= 3.21, Qt 6.5+ (Widgets), and Windows SDK >= 10.0.22000. CI builds on `windows-2022` with Qt 6.8 (`.github/workflows/build.yml`).

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="<Qt6 prefix>"
cmake --build build --config Release
"<Qt6 prefix>\bin\windeployqt.exe" build\Release\cyclops.exe   # stage Qt DLLs next to the exe
```

Produces `cyclops.exe` (the app) and `CyclopsVcamSource.dll` (the COM media source Frame Server loads; installed automatically by the app).

## Using it

Launch `cyclops.exe`. The preview starts on the IR sensor.

- **Preview**: releases the camera entirely when off; in webcam mode this also drops Cyclops' own session so other apps get the device.
- **IR illuminator**: toggles the emitter. On the N930W the hardware only supports alternate-frame illumination; unlit frames are dropped and the preview settles at about 7.5 fps of lit frames. Expect flicker when it first turns on while auto-exposure settles.
- **Windows webcam**: first flip runs the one-time elevated install (registers the CLSID and copies the source DLL into `%ProgramData%\Cyclops`). After that the switch enables/disables the device without elevation. The preview then shows the feed *through* the virtual camera, so what you see is what other apps get.

Command-line helpers (used internally by the elevated flow, handy standalone):

```
cyclops.exe --vcam-install     register CLSID + copy DLL + create device (elevated)
cyclops.exe --vcam-uninstall   remove device + unregister + delete files (elevated)
cyclops.exe --vcam-enable      create/start the virtual camera device
cyclops.exe --vcam-disable     remove the device (re-enumerable later)
```

Runtime log: `%ProgramData%\Cyclops\cyclops.log` (rotates to `cyclops.old.log` around 2 MB).

## How it works

- `src/ir_capture.cpp` - Media Foundation `IMFSourceReader` capture of the sensor camera, plus the FACEAUTH extended-control plumbing that drives the emitter.
- `vcam/` - an in-proc COM server (`IMFMediaSource`/`IMFMediaStream2`/`IKsControl`) that Windows Camera Frame Server loads when a client opens the virtual camera. A pump thread reads the physical camera; `RequestSample` fills Frame Server-provided allocator samples with the latest frame.
- Illuminator control crosses processes through a one-byte flag file (`%ProgramData%\Cyclops\illuminator`) that the pump polls; `IKsControl` FACEAUTH SETs from client apps write the same file, so toggles from either side converge.

## Caveats

- **These cameras are not designed to run the IR emitter continuously - leaving it on can overheat the camera.** Turn the illuminator off when you are not actively using the feed. You use this at your own risk.
- ProcAmp controls (brightness/contrast/exposure sliders in Settings) belong to the RGB sensor; the IR sensor exposes none. The N930W has no fully-lit mode - only alternating-frame illumination.
- The virtual camera stays registered until you uninstall it; disabling hides it from apps but keeps the registration. A Frame Server restart can drop the device entry - the app re-enables it automatically while the webcam switch is on.
- `tools\dshow_enum.exe` lists the DirectShow media types the virtual camera exposes without starting capture - a quick compat check after any format change.
- The IR emitter is shared hardware: while the virtual camera is enabled, its pump may hold the physical sensor exclusively, so the direct preview can be dark - that's what webcam mode is for.

## License

MIT - see [LICENSE](LICENSE). Born as a salvage of the OpenTrack `video-mfsensor` backend work.
