// Copyright (c) 2026 Aanish Bhirud
// SPDX-License-Identifier: MIT

#pragma once

// Shared between Cyclops.exe (which registers and drives the virtual camera)
// and CyclopsVcamSource.dll (the media source COM object Frame Server loads).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <guiddef.h>
#include <string>

namespace cyclops_vcam {

// {7c3d8e1f-4a6b-4f2d-9e5c-1b8a3d7f2e64}
constexpr GUID clsid = { 0x7c3d8e1f, 0x4a6b, 0x4f2d,
                         { 0x9e, 0x5c, 0x1b, 0x8a, 0x3d, 0x7f, 0x2e, 0x64 } };

constexpr wchar_t clsid_str[] = L"{7c3d8e1f-4a6b-4f2d-9e5c-1b8a3d7f2e64}";
constexpr wchar_t friendly_name[] = L"Cyclops (Windows Hello camera passthrough)";

// Registered under this name before the rename; install removes the stale
// device entry so camera pickers don't show both.
constexpr wchar_t legacy_friendly_name[] = L"Cyclops IR";
constexpr wchar_t dll_name[] = L"CyclopsVcamSource.dll";

// {3C31A5F8-2795-4FB9-A0A1-C733A65C0CE8}: persisted symlink attr. Set on the
// IMFVirtualCamera at creation/enable, read by the media source at activation.
constexpr GUID symlink_attr = { 0x3c31a5f8, 0x2795, 0x4fb9,
                                { 0xa0, 0xa1, 0xc7, 0x33, 0xa6, 0x5c, 0x0c, 0xe8 } };

// Created at install time with a Users+LocalService DACL: the GUI runs
// unelevated but the media source inside Frame Server still reads it.
// Resolved at runtime: ProgramData is not guaranteed to live on C:.
inline std::wstring install_dir()
{
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, _countof(buf));
    return (n && n < _countof(buf) ? std::wstring(buf) : std::wstring(L"C:\\ProgramData"))
           + L"\\Cyclops";
}

constexpr wchar_t log_name[] = L"cyclops.log";
constexpr wchar_t old_log_name[] = L"cyclops.old.log"; // ir_log rotates into this
constexpr wchar_t illuminator_flag[] = L"illuminator";

} // ns cyclops_vcam
