// Copyright (c) 2026 Aanish Bhirud
// SPDX-License-Identifier: MIT

#pragma once

// The illuminator flag file is how the unelevated GUI talks to the media
// source running inside Frame Server (LocalService). One byte, "0" or "1",
// polled by the media source while its stream runs.

#include <windows.h>
#include <sddl.h>
#include <string>

#include "vcam_ids.hpp"

namespace cyclops_vcam {

inline std::wstring illuminator_flag_path()
{
    return install_dir() + L"\\" + illuminator_flag;
}

// DACL for state files created lazily from user or LocalService context (a
// rotated log, a recreated flag). Without it they inherit the dir's file
// mask, which has no FILE_APPEND_DATA, and every later ir_log append is
// refused. Owner stays the creator: implicit WRITE_DAC on a file the creator
// already has full write to changes nothing.
constexpr wchar_t state_file_dacl[] =
    L"D:P(A;;FA;;;BA)(A;;FA;;;SY)(A;;GRGWSD;;;BU)(A;;GRGWSD;;;LS)";

struct state_file_security
{
    SECURITY_ATTRIBUTES sa{ sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE };
    state_file_security()
    {
        ConvertStringSecurityDescriptorToSecurityDescriptorW(
            state_file_dacl, SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr);
    }
    ~state_file_security()
    {
        if (sa.lpSecurityDescriptor)
            LocalFree(sa.lpSecurityDescriptor);
    }
    state_file_security(const state_file_security&) = delete;
    state_file_security& operator=(const state_file_security&) = delete;
    SECURITY_ATTRIBUTES* get() { return sa.lpSecurityDescriptor ? &sa : nullptr; }
};

// A state file the service side touches must be a plain file: a user-planted
// hardlink or reparse point would turn a LocalService read/write into an
// arbitrary file primitive. Check once after open.
inline bool is_plain_file(HANDLE f)
{
    BY_HANDLE_FILE_INFORMATION i{};
    return GetFileInformationByHandle(f, &i)
           && i.nNumberOfLinks <= 1
           && !(i.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

// Fail closed: missing, unreadable, or non-plain files all mean "off".
inline bool read_illuminator_flag(bool default_value)
{
    HANDLE f = CreateFileW(illuminator_flag_path().c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return default_value;
    char c = '0';
    if (is_plain_file(f))
    {
        DWORD n = 0;
        ReadFile(f, &c, 1, &n, nullptr);
    }
    CloseHandle(f);
    return c != '0';
}

inline bool write_illuminator_flag(bool on)
{
    // OPEN_ALWAYS, not CREATE_ALWAYS: the plain-file check has to run before
    // anything touches the data, and CREATE_ALWAYS truncates on open, which
    // would already be the arbitrary-file write through a planted hardlink.
    state_file_security sec;
    HANDLE f = CreateFileW(illuminator_flag_path().c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           sec.get(), OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    const char c = on ? '1' : '0';
    DWORD n = 0;
    const bool ok = is_plain_file(f) && WriteFile(f, &c, 1, &n, nullptr) && n == 1
                    && SetEndOfFile(f);
    CloseHandle(f);
    return ok;
}

} // ns cyclops_vcam
