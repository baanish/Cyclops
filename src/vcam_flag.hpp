// Copyright (c) 2026 Aanish Bhirud
// SPDX-License-Identifier: MIT

#pragma once

// The illuminator flag file is how the unelevated GUI talks to the media
// source running inside Frame Server (LocalService). One byte, "0" or "1",
// polled by the media source while its stream runs.

#include <windows.h>
#include <string>

#include "vcam_ids.hpp"

namespace cyclops_vcam {

inline std::wstring illuminator_flag_path()
{
    return install_dir() + L"\\" + illuminator_flag;
}

inline bool read_illuminator_flag(bool default_value)
{
    HANDLE f = CreateFileW(illuminator_flag_path().c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return default_value;
    char c = '1';
    DWORD n = 0;
    ReadFile(f, &c, 1, &n, nullptr);
    CloseHandle(f);
    return c != '0';
}

inline bool write_illuminator_flag(bool on)
{
    HANDLE f = CreateFileW(illuminator_flag_path().c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    const char c = on ? '1' : '0';
    DWORD n = 0;
    const bool ok = WriteFile(f, &c, 1, &n, nullptr) && n == 1;
    CloseHandle(f);
    return ok;
}

} // ns cyclops_vcam
