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
#include <wtsapi32.h>
#include <accctrl.h>
#include <aclapi.h>

#pragma comment(lib, "mfsensorgroup.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wtsapi32.lib")

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

// Security model for C:\ProgramData\Cyclops, split by object role. Everything
// is a protected DACL owned by Administrators: ProgramData's defaults grant
// Users delete-child, which would let any local user replace the DLL that
// Frame Server loads into a service process.
//
//  - dir:    BA/SY full; BU/LS read+execute+create-files (0x1200AB) but no
//            delete-child; files (not subdirs) also inherit delete for the
//            log rotation. That delete ACE is inherit-only (IO): applied to
//            the directory itself it would let any user rename the whole
//            install dir out from under the registered DLL path.
//  - DLL:    BA/SY full; BU/LS read+execute only.
//  - state:  BA/SY full; BU/LS read+write+delete (log rotate, flag rewrite).
constexpr wchar_t dir_sddl[] =
    L"O:BAG:BAD:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)(A;OICI;0x001200AB;;;BU)"
    L"(A;OICI;0x001200AB;;;LS)(A;OIIO;SD;;;BU)(A;OIIO;SD;;;LS)";
constexpr wchar_t dll_sddl[] =
    L"O:BAG:BAD:P(A;;FA;;;BA)(A;;FA;;;SY)(A;;FRFX;;;BU)(A;;FRFX;;;LS)";
constexpr wchar_t state_sddl[] =
    L"O:BAG:BAD:P(A;;FA;;;BA)(A;;FA;;;SY)(A;;GRGWSD;;;BU)(A;;GRGWSD;;;LS)";

PSECURITY_DESCRIPTOR make_sd(const wchar_t* sddl)
{
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &sd, nullptr))
        ir_log(L"sddl parse failed: %lu", GetLastError());
    return sd;
}

// Sets owner+group+DACL in one call: an object owned by a regular user keeps
// implicit WRITE_DAC, so ownership must move to Administrators too. Requires
// elevation for the owner change; called best-effort from unelevated paths.
bool apply_full_security(const std::wstring& path, const wchar_t* sddl)
{
    PSECURITY_DESCRIPTOR sd = make_sd(sddl);
    if (!sd)
        return false;
    PSID owner = nullptr, group = nullptr;
    PACL dacl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    GetSecurityDescriptorOwner(sd, &owner, &defaulted);
    GetSecurityDescriptorGroup(sd, &group, &defaulted);
    GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
    const DWORD err = SetNamedSecurityInfoW((LPWSTR)path.c_str(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION
        | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        owner, group, dacl, nullptr);
    LocalFree(sd);
    if (err != ERROR_SUCCESS)
    {
        ir_log(L"acl apply failed on %ls: %lu", path.c_str(), err);
        return false;
    }
    return true;
}

// C:\ProgramData\Cyclops: state files writable by unelevated users and the
// Frame Server service, the DLL locked to read+execute for both.
bool create_shared_dir()
{
    PSECURITY_DESCRIPTOR sd = make_sd(dir_sddl);
    if (!sd)
        return false;

    SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };
    const std::wstring dir = install_dir();
    bool ok = CreateDirectoryW(dir.c_str(), &sa) || GetLastError() == ERROR_ALREADY_EXISTS;
    LocalFree(sd);
    // Users can create entries in ProgramData, so a pre-existing name may be
    // a junction planted to redirect the DLL Frame Server loads as
    // LocalService. SetNamedSecurityInfo follows it and would ACL the target
    // while the attacker keeps the link; refuse to install through one.
    const DWORD attrs = GetFileAttributesW(dir.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)
        || (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
    {
        ir_log(L"install: %ls is not a plain directory (attrs 0x%lx), refusing", dir.c_str(), attrs);
        return false;
    }
    // Re-apply security even when the dir already existed: child objects only
    // inherit at creation, and a user-context creation would have left the
    // dir writable by its creator. The dir ACL is the boundary: a failure
    // here (elevated) means the original owner still controls the tree, so
    // install must not register a DLL under it.
    ok = apply_full_security(dir, dir_sddl) && ok;
    if (GetFileAttributesW(installed_dll_path().c_str()) != INVALID_FILE_ATTRIBUTES)
        apply_full_security(installed_dll_path(), dll_sddl);
    apply_full_security(illuminator_flag_path(), state_sddl);
    apply_full_security(dir + L"\\" + log_name, state_sddl);
    return ok;
}

} // ns

bool is_registered()
{
    // Registered means the COM key exists, points at our installed DLL, and
    // the file is actually on disk: a stale key alone would leave a device
    // Frame Server can't activate.
    HKEY key = nullptr;
    std::wstring path = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + clsid_str + L"\\InprocServer32";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    wchar_t reg_path[MAX_PATH]{};
    DWORD n = sizeof(reg_path);
    const LSTATUS st = RegQueryValueExW(key, nullptr, nullptr, nullptr,
                                        (BYTE*)reg_path, &n);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS)
        return false;
    const std::wstring dll = installed_dll_path();
    return _wcsicmp(reg_path, dll.c_str()) == 0
           && GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES;
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

    // Idempotent: an enumerated device with intact COM registration is done.
    // If the device is present but registration broke, fall through to
    // Start() anyway so the device is at least re-armed.
    if (is_enumerated() && is_registered())
        return true;

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
                       : op == op_uninstall ? L"--vcam-uninstall" : L"--vcam-disable";

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
        // Bounded, but generously: the helper copies a DLL (Defender scans it)
        // and round-trips Frame Server, and the UAC prompt itself waits on
        // the user. A short cap reports failure while the install still
        // completes behind the switch.
        WaitForSingleObject(sei.hProcess, 120000);
        DWORD code = 1;
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        return code == 0;
    }
    return true;
}

// Over-the-shoulder UAC: MFVirtualCameraAccess_CurrentUser binds the device
// to the elevated token's user. When that's a different account than the
// interactive desktop user, the user's apps won't see the camera; log it.
// The unelevated enable() path self-heals by creating the desktop user's own.
void warn_if_ots_elevation()
{
    wchar_t tok_user[128]{};
    DWORD n = _countof(tok_user);
    if (!GetUserNameW(tok_user, &n))
        return;
    const DWORD sess = WTSGetActiveConsoleSessionId();
    if (sess == 0xFFFFFFFF)
        return;
    LPWSTR buf = nullptr;
    DWORD bytes = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sess, WTSUserName, &buf, &bytes)
        || !buf)
        return;
    if (_wcsicmp(tok_user, buf) != 0)
        ir_log(L"install: elevated user %ls differs from session user %ls; "
               L"the vcam binds to %ls, run enable as the desktop user", tok_user, buf, tok_user);
    WTSFreeMemory(buf);
}

// A DLL mapped into Frame Server can't be overwritten but can be renamed;
// move the stale image aside as CyclopsVcamSource.old<n>.dll (the name
// uninstall's sweep matches) and retry.
std::wstring stale_dll_path(int n)
{
    return install_dir() + L"\\CyclopsVcamSource.old" + std::to_wstring(n) + L".dll";
}

bool install_dll(const std::wstring& src, const std::wstring& dst)
{
    if (CopyFileW(src.c_str(), dst.c_str(), FALSE))
        return true;
    const DWORD err = GetLastError();
    if (err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION)
        return false;
    for (int i = 0; i < 8; ++i)
    {
        if (MoveFileExW(dst.c_str(), stale_dll_path(i).c_str(), MOVEFILE_REPLACE_EXISTING))
            break;
        if (i == 7)
            return false;
    }
    return CopyFileW(src.c_str(), dst.c_str(), FALSE) != FALSE;
}

// ---- elevated helper bodies -------------------------------------------------

int install_main()
{
    MFStartup(MF_VERSION);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    warn_if_ots_elevation();

    if (!create_shared_dir())
    {
        ir_log(L"install: can't create %ls", install_dir().c_str());
        return 10;
    }

    const std::wstring dst = installed_dll_path();
    const std::wstring src = module_dir() + L"\\" + dll_name;
    if (!install_dll(src, dst))
    {
        ir_log(L"install: CopyFile %ls -> %ls failed %lu", src.c_str(), dst.c_str(), GetLastError());
        return 11;
    }
    // A DLL Users can still write must never be registered for LocalService.
    if (!apply_full_security(dst, dll_sddl))
    {
        DeleteFileW(dst.c_str());
        return 14;
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

    // Off until the user flips it: the flag is what the pump commits to the
    // emitter for any client that opens the virtual camera, Cyclops running
    // or not, and the hardware is not rated for unattended use.
    write_illuminator_flag(false);
    apply_full_security(illuminator_flag_path(), state_sddl);
    ir_log(L"install: ok");
    return 0;
}

int uninstall_main()
{
    MFStartup(MF_VERSION);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    remove_named_device(legacy_friendly_name);
    // MFCreateVirtualCamera is create-or-open: binding when the device is not
    // enumerated (uninstall after disable) would create a fresh entry whose
    // CLSID the lines below then delete, leaving a phantom in every picker.
    HRESULT hr = S_OK;
    if (is_enumerated())
    {
        IMFVirtualCamera* vcam = nullptr;
        hr = open_virtual_camera(&vcam);
        if (SUCCEEDED(hr))
            hr = remove_virtual_camera_object(vcam);
        safe_release(vcam);
        if (FAILED(hr))
            ir_log(L"uninstall: vcam remove failed hr=0x%x", (unsigned)hr);
    }

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

    // Everything from here runs after the log file is gone, and ir_log would
    // recreate it and keep the directory alive; the debugger stream is the
    // only place these diagnostics can go.
    ir_log(L"uninstall: removing files (vcam remove hr=0x%x)", (unsigned)hr);
    const auto debug = [](const std::wstring& s) {
        OutputDebugStringW((L"[cyclops] " + s + L"\n").c_str());
    };
    const auto del = [&](const std::wstring& p) {
        if (!DeleteFileW(p.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
            debug(L"uninstall: can't delete " + p + L" (" + std::to_wstring(GetLastError()) + L")");
    };
    del(install_dir() + L"\\" + log_name);
    del(install_dir() + L"\\" + old_log_name);
    del(install_dir() + L"\\" + illuminator_flag);
    del(dll);
    // Frameserver holds a mapped DLL open even after rename; upgrades leave
    // CyclopsVcamSource.old<n>.dll behind (see install_dll). Earlier builds
    // named them CyclopsVcamSource.dll.old<n>; sweep those too.
    for (const wchar_t* glob : { L"CyclopsVcamSource*.dll", L"CyclopsVcamSource.dll.old*" })
    {
        const std::wstring pattern = install_dir() + L"\\" + glob;
        WIN32_FIND_DATAW fd{};
        if (HANDLE h = FindFirstFileW(pattern.c_str(), &fd); h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    del(install_dir() + L"\\" + fd.cFileName);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    if (!RemoveDirectoryW(install_dir().c_str()))
        debug(L"uninstall: dir not removed (" + std::to_wstring(GetLastError())
              + L"), a DLL still mapped by Frame Server is the usual leftover");
    return FAILED(hr) ? 20 : 0;
}

} // ns cyclops_vcam
