// Copyright (c) 2026 Aanish Bhirud
// SPDX-License-Identifier: MIT

#include "vcam_manager.hpp"
#include "vcam_ids.hpp"
#include "vcam_flag.hpp"
#include "ir_capture.hpp"

#include <windows.h>
#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <mferror.h>
#include <sddl.h>
#include <shellapi.h>

#include <QCoreApplication>
#include <QDir>
#include <vector>

#pragma comment(lib, "mfsensorgroup.lib")
#pragma comment(lib, "advapi32.lib")

namespace cyclops_vcam {

namespace {

template<typename T> void safe_release(T*& p) { if (p) { p->Release(); p = nullptr; } }

std::wstring module_dir()
{
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, _countof(buf));
    std::wstring s = buf;
    return s.substr(0, s.find_last_of(L"\\/"));
}

std::wstring installed_dll_path()
{
    return install_dir() + L"\\" + dll_name;
}

// Opens (or creates) the virtual camera keyed by our fixed parameters.
// Re-opening with identical parameters returns the same device entry.
HRESULT open_virtual_camera(IMFVirtualCamera** out)
{
    return MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource,
                                 MFVirtualCameraLifetime_System,
                                 MFVirtualCameraAccess_CurrentUser,
                                 friendly_name, clsid_str,
                                 nullptr, 0, out);
}

// Creation path only: declares the physical camera association. Never call on
// an object you intend to Remove() - AddDeviceSourceInfo puts it in a state
// where Remove is rejected.
HRESULT configure_virtual_camera(IMFVirtualCamera* vcam)
{
    // Persist whatever sensor camera is present right now. If the camera is
    // unplugged or moved ports, the media source falls back to enumerating
    // KSCATEGORY_SENSOR_CAMERA at stream start, so this only ever helps.
    const auto sensors = enum_sensor_cameras();
    if (sensors.empty())
        return S_OK;
    vcam->SetString(symlink_attr, sensors[0].symlink.c_str());
    return vcam->AddDeviceSourceInfo(sensors[0].symlink.c_str());
}

// Removal contract per the MF sample: Remove() then Shutdown() on a fresh
// object, with no Start() and no AddDeviceSourceInfo. A System-lifetime
// device can still carry its started state across processes, so try Stop
// first when Remove is rejected with MF_E_SHUTDOWN.
HRESULT remove_virtual_camera_object(IMFVirtualCamera* vcam)
{
    HRESULT hr = vcam->Remove();
    if (hr == MF_E_SHUTDOWN)
    {
        HRESULT hs = vcam->Stop();
        HRESULT hr2 = vcam->Remove();
        ir_log(L"vcam remove retry: stop=0x%x remove=0x%x", (unsigned)hs, (unsigned)hr2);
        hr = hr2;
    }
    vcam->Shutdown();
    return hr;
}

// Removes the device registered under a given friendly name, if it exists.
// MFCreateVirtualCamera is create-or-open, so call this only for a name that
// is actually enumerated; otherwise the call itself would leave a phantom.
void remove_named_device(const wchar_t* name)
{
    bool enumerated = false;
    for (const auto& d : enum_video_cameras())
        if (d.name.rfind(name, 0) == 0)
            enumerated = true;
    if (!enumerated)
        return;

    IMFVirtualCamera* v = nullptr;
    if (FAILED(MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource,
                                    MFVirtualCameraLifetime_System,
                                    MFVirtualCameraAccess_CurrentUser,
                                    name, clsid_str, nullptr, 0, &v)))
        return;
    HRESULT hr = remove_virtual_camera_object(v);
    ir_log(L"remove_named_device(%ls): remove=0x%x", name, (unsigned)hr);
    safe_release(v);
}

bool call_dll_entry(const wchar_t* path, const char* entry)
{
    HMODULE mod = LoadLibraryW(path);
    if (!mod)
    {
        ir_log(L"LoadLibrary(%ls) failed %lu", path, GetLastError());
        return false;
    }
    auto fn = (HRESULT(WINAPI*)())GetProcAddress(mod, entry);
    HRESULT hr = fn ? fn() : E_FAIL;
    if (FAILED(hr))
        ir_log(L"%ls!%hs failed hr=0x%x", path, entry, (unsigned)hr);
    FreeLibrary(mod);
    return SUCCEEDED(hr);
}

// The SDDL for C:\ProgramData\Cyclops and the files in it. LocalService needs
// full control: Frame Server (svchost, LocalService) loads CyclopsVcamSource.dll
// and reads the illuminator flag from this directory.
PSECURITY_DESCRIPTOR shared_sd()
{
    static PSECURITY_DESCRIPTOR sd = [] {
        PSECURITY_DESCRIPTOR p = nullptr;
        ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;OICI;FA;;;BA)(A;OICI;FA;;;BU)(A;OICI;FA;;;LS)",
            SDDL_REVISION_1, &p, nullptr);
        return p;
    }();
    return sd;
}

void apply_shared_dacl(const std::wstring& path)
{
    if (PSECURITY_DESCRIPTOR sd = shared_sd())
        SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, sd);
}

// C:\ProgramData\Cyclops, writable by unelevated users, readable by the
// Frame Server service (LocalService).
bool create_shared_dir()
{
    PSECURITY_DESCRIPTOR sd = shared_sd();
    if (!sd)
        return false;

    SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };
    const std::wstring dir = install_dir();
    bool ok = CreateDirectoryW(dir.c_str(), &sa) || GetLastError() == ERROR_ALREADY_EXISTS;
    // Re-apply the DACL even when the dir already existed. Child objects only
    // inherit at creation, so fix the files that can already be on disk too.
    apply_shared_dacl(dir);
    apply_shared_dacl(installed_dll_path());
    apply_shared_dacl(illuminator_flag_path());
    apply_shared_dacl(dir + L"\\" + log_name);
    return ok;
}

} // ns

bool is_elevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION te{};
    DWORD len = sizeof(te);
    bool ret = GetTokenInformation(token, TokenElevation, &te, len, &len) && te.TokenIsElevated;
    CloseHandle(token);
    return ret;
}

bool is_registered()
{
    HKEY key = nullptr;
    std::wstring path = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + clsid_str + L"\\InprocServer32";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    RegCloseKey(key);
    return true;
}

// IMFVirtualCamera ops and MF device enumeration both need the MF runtime;
// without MFStartup a fresh binding answers every call with MF_E_SHUTDOWN.
struct mf_runtime
{
    mf_runtime() { MFStartup(MF_VERSION); }
    ~mf_runtime() { MFShutdown(); }
};

bool is_enumerated()
{
    for (const auto& d : enum_video_cameras())
        if (d.name == friendly_name || d.name.rfind(friendly_name, 0) == 0)
            return true;
    return false;
}

bool enable(QString* err)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED); // CLI helper path has no Qt
    mf_runtime mf;
    // Self-heal the share dir ACL; an install predating the LocalService grant
    // leaves the DLL unreadable by Frame Server.
    create_shared_dir();
    remove_named_device(legacy_friendly_name); // renamed registrations linger

    IMFVirtualCamera* vcam = nullptr;
    HRESULT hr = open_virtual_camera(&vcam);
    if (SUCCEEDED(hr))
        hr = configure_virtual_camera(vcam);
    if (SUCCEEDED(hr))
        hr = vcam->Start(nullptr);
    safe_release(vcam);

    if (FAILED(hr))
    {
        ir_log(L"vcam enable failed hr=0x%x", (unsigned)hr);
        if (err) *err = QStringLiteral("could not enable the virtual camera (hr=0x%1)").arg((unsigned)hr, 8, 16);
        return false;
    }
    return true;
}

bool disable(QString* err)
{
    // IMFVirtualCamera::Stop only works on the instance that called Start, and
    // that instance lived in the install helper process. Remove() + Shutdown()
    // on a fresh binding is the documented cross-process way to de-enumerate;
    // enable() recreates the device.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    mf_runtime mf;
    remove_named_device(legacy_friendly_name); // renamed registrations linger
    // MFCreateVirtualCamera is create-or-open: only bind when the device is
    // actually enumerated, else the binding itself leaves a phantom.
    if (!is_enumerated())
        return true;
    IMFVirtualCamera* vcam = nullptr;
    HRESULT hr = open_virtual_camera(&vcam);
    if (SUCCEEDED(hr))
        hr = remove_virtual_camera_object(vcam);
    safe_release(vcam);

    if (FAILED(hr))
    {
        ir_log(L"vcam disable failed hr=0x%x", (unsigned)hr);
        if (err) *err = QStringLiteral("could not disable the virtual camera (hr=0x%1)").arg((unsigned)hr, 8, 16);
        return false;
    }
    return true;
}

bool elevate_op(install_op op)
{
    const wchar_t* arg = op == op_install ? L"--vcam-install"
                       : op == op_uninstall ? L"--vcam-uninstall"
                       : op == op_enable ? L"--vcam-enable" : L"--vcam-disable";

    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, _countof(exe));
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = arg;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei))
        return false;
    if (sei.hProcess)
    {
        WaitForSingleObject(sei.hProcess, 30000);
        DWORD code = 1;
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        return code == 0;
    }
    return true;
}

// ---- elevated helper bodies -------------------------------------------------

int install_main()
{
    MFStartup(MF_VERSION);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    if (!create_shared_dir())
    {
        ir_log(L"install: can't create %ls", install_dir().c_str());
        return 10;
    }

    const std::wstring dst = installed_dll_path();
    const std::wstring src = module_dir() + L"\\" + dll_name;
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE))
    {
        ir_log(L"install: CopyFile %ls -> %ls failed %lu", src.c_str(), dst.c_str(), GetLastError());
        return 11;
    }

    if (!call_dll_entry(dst.c_str(), "DllRegisterServer"))
        return 12;

    // Remove the device entry left by the pre-rename friendly name, if any.
    remove_named_device(legacy_friendly_name);

    IMFVirtualCamera* vcam = nullptr;
    HRESULT hr = open_virtual_camera(&vcam);
    if (SUCCEEDED(hr))
        hr = configure_virtual_camera(vcam);
    if (SUCCEEDED(hr))
        hr = vcam->Start(nullptr);
    safe_release(vcam);
    if (FAILED(hr))
    {
        ir_log(L"install: vcam create/start failed hr=0x%x", (unsigned)hr);
        return 13;
    }

    write_illuminator_flag(true);
    apply_shared_dacl(illuminator_flag_path());
    ir_log(L"install: ok");
    return 0;
}

int uninstall_main()
{
    MFStartup(MF_VERSION);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMFVirtualCamera* vcam = nullptr;
    HRESULT hr = open_virtual_camera(&vcam);
    if (SUCCEEDED(hr))
        hr = remove_virtual_camera_object(vcam);
    safe_release(vcam);
    if (FAILED(hr))
        ir_log(L"uninstall: vcam remove failed hr=0x%x", (unsigned)hr);

    const std::wstring dll = installed_dll_path();
    if (!call_dll_entry(dll.c_str(), "DllUnregisterServer"))
    {
        // fall back to deleting the key directly; the file may be gone already
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID", 0,
                          KEY_WRITE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS)
        {
            RegDeleteTreeW(key, clsid_str);
            RegCloseKey(key);
        }
    }

    DeleteFileW(dll.c_str());
    DeleteFileW((install_dir() + L"\\" + illuminator_flag).c_str());
    DeleteFileW((install_dir() + L"\\" + log_name).c_str());
    // Frameserver holds a mapped DLL open even after rename; upgrades leave
    // CyclopsVcamSource.old*.dll behind.
    const std::wstring pattern = install_dir() + L"\\" + L"CyclopsVcamSource*.dll";
    WIN32_FIND_DATAW fd{};
    if (HANDLE h = FindFirstFileW(pattern.c_str(), &fd); h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                DeleteFileW((install_dir() + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(install_dir().c_str());
    ir_log(L"uninstall: done (hr=0x%x)", (unsigned)hr);
    return FAILED(hr) ? 20 : 0;
}

} // ns cyclops_vcam
