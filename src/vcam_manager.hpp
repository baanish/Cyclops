// Copyright (c) 2026 Aanish Bhirud
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>
#include <string>

#include "vcam_flag.hpp"

// Driving the "expose the IR camera as a normal webcam" feature.
//
// Install (needs admin, once): copies CyclopsVcamSource.dll to
// C:\ProgramData\Cyclops, registers its CLSID under HKLM, creates the
// MFCreateVirtualCamera device with System lifetime and starts it.
// Enable/disable afterwards are unelevated create+Start() / Remove() calls.

namespace cyclops_vcam {

bool is_registered();   // the media source CLSID is registered under HKLM
bool is_enumerated();   // friendly_name currently appears as a camera device
bool is_elevated();

bool enable(QString* err);   // MFCreateVirtualCamera + AddDeviceSourceInfo + Start
bool disable(QString* err);  // MFCreateVirtualCamera + Remove (Stop retry) + Shutdown

// Re-launches this exe elevated with --vcam-install / --vcam-uninstall.
// Returns false if the user declined the UAC prompt or the launch failed.
enum install_op { op_install, op_uninstall, op_enable, op_disable };
bool elevate_op(install_op op);

// Body of the elevated helper; returns a process exit code.
int install_main();
int uninstall_main();

} // ns cyclops_vcam
