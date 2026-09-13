// Copyright (c) 2026 Aanish Bhirud
// SPDX-License-Identifier: MIT

#include "ir_capture.hpp"
#include "vcam_ids.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfcaptureengine.h>
#include <ks.h>
#include <ksmedia.h>
#include <wchar.h>
#include <stdio.h>

#include <algorithm>
#include <cstdarg>
#include <mutex>

namespace {

template<typename T> void safe_release(T*& p) { if (p) { p->Release(); p = nullptr; } }

constexpr unsigned long faceauth_property = KSPROPERTY_CAMERACONTROL_EXTENDED_FACEAUTH_MODE;

// ReadSample legitimately returns nothing on a stream tick, and half the frames
// are dropped in strobe mode, so one read_frame() may need several reads.
constexpr int max_reads_per_frame = 40;

double frame_mean(const unsigned char* p, size_t n)
{
    if (!n)
        return 0;
    // every 16th byte: this runs per frame and only feeds a lit/unlit decision
    unsigned long long total = 0;
    size_t count = 0;
    for (size_t i = 0; i < n; i += 16, count++)
        total += p[i];
    return count ? (double)total / count : 0;
}

std::vector<ir_camera_device> enum_cameras(const GUID& category)
{
    std::vector<ir_camera_device> ret;

    // Runs on bare helper threads now; MF flat APIs don't require an
    // apartment but the original UI-thread path always had one via Qt.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION)))
    {
        ir_log(L"MFStartup failed during camera enumeration");
        return ret;
    }

    IMFAttributes* attrs = nullptr;
    if (SUCCEEDED(MFCreateAttributes(&attrs, 2)))
    {
        attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_CATEGORY, category);

        IMFActivate** devices = nullptr;
        UINT32 count = 0;
        if (SUCCEEDED(MFEnumDeviceSources(attrs, &devices, &count)))
        {
            for (UINT32 i = 0; i < count; i++)
            {
                WCHAR* friendly = nullptr;
                WCHAR* symlink = nullptr;
                UINT32 cch = 0;

                devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &friendly, &cch);
                devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &symlink, &cch);

                if (symlink)
                {
                    std::wstring name = friendly ? friendly : L"camera";
                    const std::wstring base = name;
                    for (int n = 2; std::any_of(ret.cbegin(), ret.cend(),
                                                [&](const ir_camera_device& d) { return d.name == name; }); n++)
                        name = base + L" " + std::to_wstring(n);
                    ret.push_back({ name, symlink });
                }

                if (friendly) CoTaskMemFree(friendly);
                if (symlink) CoTaskMemFree(symlink);
                safe_release(devices[i]);
            }
            CoTaskMemFree(devices);
        }
        safe_release(attrs);
    }

    MFShutdown();
    return ret;
}

} // ns

std::vector<ir_camera_device> enum_sensor_cameras()
{
    return enum_cameras(KSCATEGORY_SENSOR_CAMERA);
}

std::vector<ir_camera_device> enum_video_cameras()
{
    return enum_cameras(KSCATEGORY_VIDEO_CAMERA);
}

void ir_log(const wchar_t* fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    OutputDebugStringW(L"[cyclops] ");
    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");

    // The vcam media source runs inside the Frame Server service where a
    // debugger isn't an option, so keep a plain file log too. Leaked on
    // purpose: helper threads can outlive static teardown at process exit.
    static auto* m = new std::mutex;
    std::lock_guard<std::mutex> g(*m);
    const std::wstring dir = cyclops_vcam::install_dir();
    const std::wstring log = dir + L"\\" + cyclops_vcam::log_name;
    CreateDirectoryW(dir.c_str(), nullptr);
    // Rotate rather than grow forever: once past 2 MB, the old log is kept as
    // cyclops.old.log and a fresh one starts.
    if (HANDLE probe = CreateFileW(log.c_str(), FILE_READ_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        probe != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER sz{};
        GetFileSizeEx(probe, &sz);
        CloseHandle(probe);
        if (sz.QuadPart > 2 * 1024 * 1024)
            MoveFileExW(log.c_str(), (dir + L"\\cyclops.old.log").c_str(),
                        MOVEFILE_REPLACE_EXISTING);
    }
    if (HANDLE f = CreateFileW(log.c_str(), FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        f != INVALID_HANDLE_VALUE)
    {
        char narrow[2048];
        int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, narrow, sizeof(narrow) - 3, nullptr, nullptr);
        if (n > 0)
        {
            while (n > 1 && (narrow[n - 2] == '\n' || narrow[n - 2] == '\r'))
                narrow[--n - 1] = 0;
            DWORD written = 0;
            WriteFile(f, narrow, (DWORD)(n - 1), &written, nullptr);
            WriteFile(f, "\r\n", 2, &written, nullptr);
        }
        CloseHandle(f);
    }
}

ir_capture::~ir_capture()
{
    close();
}

bool ir_capture::open(const std::wstring& symlink)
{
    close();
    symlink_ = symlink;

    if (FAILED(MFStartup(MF_VERSION)))
    {
        ir_log(L"MFStartup failed");
        return false;
    }
    mf_started_ = true;

    IMFAttributes* attrs = nullptr;
    if (FAILED(MFCreateAttributes(&attrs, 2)))
        goto fail;
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    attrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, symlink_.c_str());

    if (FAILED(MFCreateDeviceSource(attrs, &source_)))
    {
        ir_log(L"can't open %ls", symlink_.c_str());
        safe_release(attrs);
        goto fail;
    }
    safe_release(attrs);
    source_owned_ = true;

    return start_with_source();

fail:
    close();
    return false;
}

bool ir_capture::open(IMFMediaSource* source)
{
    close();

    if (!source)
        return false;

    if (FAILED(MFStartup(MF_VERSION)))
        return false;
    mf_started_ = true;

    source_ = source;
    source_->AddRef();
    source_owned_ = false; // owned by the frame server; don't Shutdown() it

    return start_with_source();
}

bool ir_capture::start_with_source()
{
    {
        IMFGetService* svc = nullptr;
        if (SUCCEEDED(source_->QueryInterface(IID_PPV_ARGS(&svc))))
        {
            // GUID_NULL is what the IMFExtendedCameraController docs specify here.
            (void)svc->GetService(GUID_NULL, IID_PPV_ARGS(&controller_));
            safe_release(svc);
        }
        if (!controller_)
            ir_log(L"no extended camera controller, emitter stays off");
    }

    if (FAILED(MFCreateSourceReaderFromMediaSource(source_, nullptr, &reader_)))
    {
        ir_log(L"can't create source reader");
        goto fail;
    }

    // This camera family publishes exactly one format (L8 640x360@30), so the
    // format is deliberately not negotiated; the caller is told what it got.
    {
        IMFMediaType* mt = nullptr;
        if (SUCCEEDED(reader_->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &mt)))
        {
            UINT32 w = 0, h = 0;
            if (SUCCEEDED(MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &w, &h)))
            {
                width_ = (int)w;
                height_ = (int)h;
            }
            UINT32 fn = 0, fd = 0;
            if (SUCCEEDED(MFGetAttributeRatio(mt, MF_MT_FRAME_RATE, &fn, &fd)) && fd)
                fps_ = (int)(fn / fd);
            GUID subtype{};
            if (SUCCEEDED(mt->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
                subtype != MFVideoFormat_L8 && subtype != MFVideoFormat_NV12)
            {
                // NV12's leading plane is luma, so it decodes as gray for free.
                // Anything else would need a converter this code doesn't have.
                ir_log(L"unsupported subtype 0x%x", (unsigned)subtype.Data1);
                safe_release(mt);
                goto fail;
            }
            safe_release(mt);
        }
    }

    if (width_ <= 0 || height_ <= 0)
    {
        ir_log(L"no frame size");
        goto fail;
    }

    if (!read_frame(2000))
    {
        ir_log(L"no first frame");
        goto fail;
    }

    probe_illumination();
    return true;

fail:
    close();
    return false;
}

void ir_capture::close()
{
    if (illum_mode_)
        apply_illumination(KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_DISABLED);

    safe_release(reader_);
    safe_release(controller_);

    if (source_)
    {
        if (source_owned_)
            source_->Shutdown();
        safe_release(source_);
        source_owned_ = false;
    }

    if (mf_started_)
    {
        MFShutdown();
        mf_started_ = false;
    }

    buf_.clear();
    width_ = height_ = fps_ = 0;
    illum_stream_ = 0;
    illum_mode_ = illum_on_mode_ = 0;
    illum_capable_ = false;
    strobing_ = false;
    lit_metadata_ = -1;
    last_mean_ = peak_mean_ = 0;
}

bool ir_capture::probe_illumination()
{
    illum_on_mode_ = 0;
    illum_capable_ = false;

    if (!controller_)
        return false;

    DWORD stream_count = 0;
    {
        IMFPresentationDescriptor* pd = nullptr;
        if (SUCCEEDED(source_->CreatePresentationDescriptor(&pd)))
        {
            pd->GetStreamDescriptorCount(&stream_count);
            safe_release(pd);
        }
    }

    // FACEAUTH_MODE is pin-scoped, so the per-stream indices are the ones that
    // answer; the filter-scope index is probed last only for cameras that
    // scope it differently.
    std::vector<DWORD> candidates;
    for (DWORD i = 0; i < stream_count; i++)
        candidates.push_back(i);
    if (candidates.empty())
        candidates.push_back(0);
    candidates.push_back((DWORD)MF_CAPTURE_ENGINE_MEDIASOURCE);

    for (DWORD idx : candidates)
    {
        IMFExtendedCameraControl* ctl = nullptr;
        if (FAILED(controller_->GetExtendedCameraControl(idx, faceauth_property, &ctl)) || !ctl)
            continue;

        const ULONGLONG caps = ctl->GetCapabilities();
        // BACKGROUND_SUBTRACTION keeps every frame lit, so it is worth twice
        // the framerate of the strobe. No camera tested so far advertises it.
        ULONGLONG mode = 0;
        if (caps & KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_BACKGROUND_SUBTRACTION)
            mode = KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_BACKGROUND_SUBTRACTION;
        else if (caps & KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_ALTERNATIVE_FRAME_ILLUMINATION)
            mode = KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_ALTERNATIVE_FRAME_ILLUMINATION;

        safe_release(ctl);
        if (!mode)
            continue;

        illum_stream_ = idx;
        illum_on_mode_ = mode;
        illum_capable_ = true;
        return true;
    }

    ir_log(L"no illuminating FACEAUTH mode, ambient IR only");
    return false;
}

bool ir_capture::apply_illumination(unsigned long long mode)
{
    IMFExtendedCameraControl* ctl = nullptr;
    if (FAILED(controller_->GetExtendedCameraControl(illum_stream_, faceauth_property, &ctl)) || !ctl)
        return false;

    // Never fold these into one expression: argument evaluation order is
    // unspecified and MSVC runs right-to-left, which commits before it sets.
    HRESULT hr = ctl->SetFlags(mode);
    if (SUCCEEDED(hr))
        hr = ctl->CommitSettings();
    safe_release(ctl);

    if (FAILED(hr))
    {
        ir_log(L"FACEAUTH SetFlags/Commit(0x%llx) failed, hr=0x%x", mode, (unsigned)hr);
        return false;
    }

    illum_mode_ = mode;
    strobing_ = mode == KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_ALTERNATIVE_FRAME_ILLUMINATION;
    const bool on = mode > KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_DISABLED;
    ir_log(L"IR emitter %s, stream %lu%s", on ? L"on" : L"off", illum_stream_,
           strobing_ ? L" (alternating frame illumination)" : L"");
    return true;
}

// FACEAUTH_MODE_DISABLED is 0x1, not 0: a disabled emitter still holds a
// nonzero mode, so "on" means a mode beyond DISABLED.
bool ir_capture::illumination_on() const
{
    return illum_mode_ > KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_DISABLED;
}

bool ir_capture::set_illumination(bool on)
{
    if (!illum_capable_ || !controller_)
        return false;

    if (on == illumination_on())
        return true;

    // After the emitter comes on, auto-exposure needs ~8 lit frames to settle;
    // they read ~6x too bright. Don't deliver them.
    bool ok = apply_illumination(on ? illum_on_mode_ : KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_DISABLED);
    if (ok && on)
    {
        // Bounded by wall clock too: iteration count alone still wedges the
        // worker if every read blocks.
        const DWORD deadline = GetTickCount() + 2000;
        int skipped = 0;
        for (int i = 0; i < max_reads_per_frame * 4 && skipped < 8; i++)
        {
            if ((int)(GetTickCount() - deadline) >= 0)
                break;
            if (!read_sample())
            {
                Sleep(5);
                continue;
            }
            if (sample_is_lit())
                skipped++;
        }
    }
    return ok;
}

bool ir_capture::read_sample()
{
    if (!reader_)
        return false;

    DWORD flags = 0;
    LONGLONG timestamp = 0;
    IMFSample* sample = nullptr;

    if (FAILED(reader_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                   nullptr, &flags, &timestamp, &sample)))
        return false;

    if (!sample) // stream tick or end of stream, not an error
        return false;

    lit_metadata_ = -1;
    {
        IMFAttributes* meta = nullptr;
        if (SUCCEEDED(sample->GetUnknown(MFSampleExtension_CaptureMetadata, IID_PPV_ARGS(&meta))))
        {
            UINT32 lit = 0;
            if (SUCCEEDED(meta->GetUINT32(MF_CAPTURE_METADATA_FRAME_ILLUMINATION, &lit)))
                lit_metadata_ = lit ? 1 : 0;
            safe_release(meta);
        }
    }

    // Strobe off-phase frames get dropped anyway; skip the buffer copy.
    if (lit_metadata_ == 0)
    {
        safe_release(sample);
        return false;
    }

    bool ok = false;
    IMFMediaBuffer* buffer = nullptr;
    if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)))
    {
        const size_t need = (size_t)width_ * height_;
        IMF2DBuffer* b2d = nullptr;
        if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&b2d))))
        {
            // Stride may exceed width (alignment padding): copy row by row.
            BYTE* p = nullptr;
            LONG pitch = 0;
            if (SUCCEEDED(b2d->Lock2D(&p, &pitch)))
            {
                if (p && pitch >= width_)
                {
                    buf_.resize(need);
                    for (int y = 0; y < height_; y++)
                        memcpy(buf_.data() + (size_t)y * width_, p + (size_t)y * pitch, width_);
                    last_mean_ = frame_mean(buf_.data(), need);
                    peak_mean_ = std::max(last_mean_, peak_mean_ * 0.98);
                    ok = true;
                }
                b2d->Unlock2D();
            }
            safe_release(b2d);
        }
        if (!ok) // no 2D buffer, or its lock/pitch was unusable
        {
            BYTE* p = nullptr;
            DWORD max_len = 0, cur_len = 0;
            if (SUCCEEDED(buffer->Lock(&p, &max_len, &cur_len)))
            {
                if (cur_len >= need)
                {
                    // Only the first plane is copied: L8 is all luma, and
                    // NV12's leading plane is luma with the chroma after it.
                    buf_.assign(p, p + need);
                    last_mean_ = frame_mean(buf_.data(), need);
                    peak_mean_ = std::max(last_mean_, peak_mean_ * 0.98);
                    ok = true;
                }
                else if (buf_.empty()) // once, before the first good frame
                    ir_log(L"sample is %lu bytes, need %zu", (unsigned long)cur_len, need);
                buffer->Unlock();
            }
        }
        safe_release(buffer);
    }
    safe_release(sample);

    return ok;
}

bool ir_capture::sample_is_lit() const
{
    if (!strobing_)
        return true;
    if (lit_metadata_ >= 0)
        return lit_metadata_ == 1;

    // Fallback for cameras that don't tag frames: the off-phase reads ~0 even
    // in a lit room, so anything well below the recent peak is off-phase.
    return last_mean_ > 0.5 && last_mean_ > peak_mean_ * 0.4;
}

bool ir_capture::read_frame(int timeout_ms)
{
    const DWORD deadline = GetTickCount() + (DWORD)timeout_ms;
    for (int i = 0; i < max_reads_per_frame; i++)
    {
        if (!read_sample())
        {
            if ((int)(GetTickCount() - deadline) >= 0)
                return false;
            Sleep(5);
            continue;
        }
        if (sample_is_lit())
            return true;
        if ((int)(GetTickCount() - deadline) >= 0)
            return false;
    }
    return false;
}
