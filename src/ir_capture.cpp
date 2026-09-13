// Copyright (c) 2026 Aanish Bhirud
// SPDX-License-Identifier: MIT

#include "ir_capture.hpp"
#include "vcam_ids.hpp"
#include "vcam_flag.hpp"

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

// Frame size and rate off a media type. Size and rate are written only when
// present; the return says whether the size was.
bool read_type_geometry(IMFMediaType* mt, int& width, int& height, int& fps)
{
    UINT32 w = 0, h = 0;
    const bool have_size = SUCCEEDED(MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &w, &h)) && w && h;
    if (have_size)
    {
        width = (int)w;
        height = (int)h;
    }
    UINT32 fn = 0, fd = 0;
    if (SUCCEEDED(MFGetAttributeRatio(mt, MF_MT_FRAME_RATE, &fn, &fd)) && fd && fn)
        fps = (int)((fn + fd / 2) / fd);
    return have_size;
}

// Multi-sensor sources can expose several streams; prefer the one tagged
// Infrared, else the first video stream this code can render (L8 or NV12).
// MF_SOURCE_READER_FIRST_VIDEO_STREAM means nothing qualified.
DWORD pick_capture_stream(IMFPresentationDescriptor* pd)
{
    DWORD pick = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    DWORD first_renderable = pick;
    DWORD count = 0;
    pd->GetStreamDescriptorCount(&count);
    for (DWORD i = 0; i < count; i++)
    {
        IMFStreamDescriptor* sd = nullptr;
        BOOL sel = FALSE;
        if (FAILED(pd->GetStreamDescriptorByIndex(i, &sel, &sd)) || !sd)
            continue;
        UINT32 fst = 0;
        const bool infrared = SUCCEEDED(sd->GetUINT32(
            MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, &fst))
            && (fst & MFFrameSourceTypes_Infrared);
        IMFMediaTypeHandler* th = nullptr;
        bool video = false, renderable = false;
        if (SUCCEEDED(sd->GetMediaTypeHandler(&th)) && th)
        {
            GUID major{};
            video = SUCCEEDED(th->GetMajorType(&major)) && major == MFMediaType_Video;
            IMFMediaType* mt = nullptr;
            if (video && SUCCEEDED(th->GetMediaTypeByIndex(0, &mt)) && mt)
            {
                GUID sub{};
                if (SUCCEEDED(mt->GetGUID(MF_MT_SUBTYPE, &sub)))
                    renderable = sub == MFVideoFormat_L8 || sub == MFVideoFormat_NV12;
                safe_release(mt);
            }
            safe_release(th);
        }
        safe_release(sd);
        if (!video || !renderable)
            continue;
        if (first_renderable == (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM)
            first_renderable = i;
        if (infrared)
        {
            pick = i;
            break;
        }
    }
    return pick != (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM ? pick : first_renderable;
}

std::vector<ir_camera_device> enum_cameras(const GUID& category)
{
    std::vector<ir_camera_device> ret;

    // Runs on bare helper threads now; MF flat APIs don't require an
    // apartment but the original UI-thread path always had one via Qt.
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION)))
    {
        ir_log(L"MFStartup failed during camera enumeration");
        if (SUCCEEDED(ci))
            CoUninitialize();
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
    if (SUCCEEDED(ci))
        CoUninitialize();
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

bool probe_source_geometry(IMFMediaSource* source, int& width, int& height, int& fps)
{
    IMFPresentationDescriptor* pd = nullptr;
    if (!source || FAILED(source->CreatePresentationDescriptor(&pd)) || !pd)
        return false;
    DWORD idx = pick_capture_stream(pd);
    if (idx == (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM)
        idx = 0;
    bool ok = false;
    IMFStreamDescriptor* sd = nullptr;
    BOOL sel = FALSE;
    if (SUCCEEDED(pd->GetStreamDescriptorByIndex(idx, &sel, &sd)) && sd)
    {
        IMFMediaTypeHandler* th = nullptr;
        if (SUCCEEDED(sd->GetMediaTypeHandler(&th)) && th)
        {
            // No current type until something negotiates one; the first
            // available type is what the source reader would pick.
            IMFMediaType* mt = nullptr;
            if (FAILED(th->GetCurrentMediaType(&mt)) || !mt)
            {
                safe_release(mt);
                th->GetMediaTypeByIndex(0, &mt);
            }
            if (mt)
            {
                ok = read_type_geometry(mt, width, height, fps);
                safe_release(mt);
            }
            safe_release(th);
        }
        safe_release(sd);
    }
    safe_release(pd);
    return ok;
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
    // The dir is created by the installer with a tight DACL; never create it
    // here, or a user-context call would leave it weakly secured.
    // Rotate rather than grow forever: once past 2 MB, the old log is kept as
    // cyclops.old.log and a fresh one starts.
    if (HANDLE probe = CreateFileW(log.c_str(), FILE_READ_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        probe != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER sz{};
        GetFileSizeEx(probe, &sz);
        CloseHandle(probe);
        if (sz.QuadPart > 2 * 1024 * 1024)
            MoveFileExW(log.c_str(), (dir + L"\\" + cyclops_vcam::old_log_name).c_str(),
                        MOVEFILE_REPLACE_EXISTING);
    }
    cyclops_vcam::state_file_security sec; // a fresh log must stay appendable
    if (HANDLE f = CreateFileW(log.c_str(), FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, sec.get(),
                               OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        f != INVALID_HANDLE_VALUE)
    {
        if (cyclops_vcam::is_plain_file(f))
        {
            char narrow[2048];
            int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, narrow, sizeof(narrow) - 4, nullptr, nullptr);
            if (n > 0)
            {
                while (n > 1 && (narrow[n - 2] == '\n' || narrow[n - 2] == '\r'))
                    narrow[--n - 1] = 0;
                // One WriteFile per line: split writes interleave across processes.
                narrow[n - 1] = '\r';
                narrow[n] = '\n';
                DWORD written = 0;
                WriteFile(f, narrow, (DWORD)(n + 1), &written, nullptr);
            }
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
    attrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, symlink.c_str());

    if (FAILED(MFCreateDeviceSource(attrs, &source_)))
    {
        ir_log(L"can't open %ls", symlink.c_str());
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

    {
        // Releasing a source reader Shutdown()s its media source unless told
        // otherwise; an adopted (Frame-Server-provided) source must survive us.
        IMFAttributes* rattr = nullptr;
        if (FAILED(MFCreateAttributes(&rattr, 1)) || !rattr
            || FAILED(rattr->SetUINT32(MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE)))
        {
            // Never build a reader without the flag: releasing it would
            // Shutdown() an adopted Frame Server source for good.
            ir_log(L"can't create source reader attributes");
            safe_release(rattr);
            goto fail;
        }
        HRESULT hr = MFCreateSourceReaderFromMediaSource(source_, rattr, &reader_);
        safe_release(rattr);
        if (FAILED(hr))
        {
            ir_log(L"can't create source reader");
            goto fail;
        }
    }

    {
        DWORD pick = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
        IMFPresentationDescriptor* pd = nullptr;
        if (SUCCEEDED(source_->CreatePresentationDescriptor(&pd)) && pd)
        {
            pick = pick_capture_stream(pd);
            safe_release(pd);
        }
        if (pick != (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM
            && SUCCEEDED(reader_->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE))
            && SUCCEEDED(reader_->SetStreamSelection(pick, TRUE)))
        {
            stream_index_ = (int)pick;
            if (pick != 0)
                ir_log(L"selected stream %lu", pick);
        }
    }

    if (!read_current_type())
    {
        ir_log(L"unsupported or missing media type");
        goto fail;
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

    // The emitter is shared hardware and illum_mode_ only tracks what this
    // object committed: a previous holder killed mid-stream (watchdog
    // terminate) or another process may have left it strobing. Commit a
    // known state so the cache and the hardware agree from the first frame.
    if (probe_illumination())
        apply_illumination(KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_DISABLED);
    return true;

fail:
    close();
    return false;
}

bool ir_capture::read_current_type()
{
    const DWORD sidx = stream_index_ >= 0 ? (DWORD)stream_index_
                                        : (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    IMFMediaType* mt = nullptr;
    if (FAILED(reader_->GetCurrentMediaType(sidx, &mt)) || !mt)
        return false;
    read_type_geometry(mt, width_, height_, fps_);
    // Signed in the attribute; bottom-up (negative) is meaningless for L8/NV12
    // capture, so anything not wider than the image falls back to packed rows.
    UINT32 stride = 0;
    stride_ = SUCCEEDED(mt->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) && (INT32)stride > width_
                  ? (int)(INT32)stride : width_;
    GUID subtype{};
    // NV12's leading plane is luma, so it decodes as gray for free. Anything
    // else (including a type with no subtype at all) would need a converter
    // this code doesn't have.
    const bool supported = SUCCEEDED(mt->GetGUID(MF_MT_SUBTYPE, &subtype))
        && (subtype == MFVideoFormat_L8 || subtype == MFVideoFormat_NV12);
    safe_release(mt);
    if (!supported)
        ir_log(L"unsupported subtype 0x%x", (unsigned)subtype.Data1);
    type_ok_ = supported;
    return supported;
}

void ir_capture::close()
{
    if (illum_mode_)
        apply_illumination(KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_DISABLED);

    safe_release(reader_);
    safe_release(controller_);

    if (source_)
    {
        // The reader is created disconnected, so the source still runs: stop
        // it ourselves, and Shutdown only the sources we own.
        if (source_owned_)
            source_->Shutdown();
        else
            source_->Stop();
        safe_release(source_);
        source_owned_ = false;
    }

    if (mf_started_)
    {
        MFShutdown();
        mf_started_ = false;
    }

    buf_.clear();
    width_ = height_ = fps_ = stride_ = 0;
    type_ok_ = true;
    illum_stream_ = 0;
    illum_mode_ = illum_on_mode_ = 0;
    illum_capable_ = false;
    strobing_ = false;
    stream_index_ = -1;
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
    if (!controller_)
        return false;
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
            const read_result r = read_sample();
            if (r == read_result::failed)
                Sleep(5);
            else if (r == read_result::frame && sample_is_lit())
                skipped++;
        }
        // The overexposed settle frames pushed the phase-detector peak to ~6x
        // the settled mean; reseed it or the next ~40 lit frames drop too.
        last_mean_ = peak_mean_ = 0;
    }
    return ok;
}

ir_capture::read_result ir_capture::read_sample()
{
    if (!reader_)
        return read_result::failed;

    DWORD flags = 0;
    LONGLONG timestamp = 0;
    IMFSample* sample = nullptr;

    const DWORD sidx = stream_index_ >= 0 ? (DWORD)stream_index_
                                        : (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    if (FAILED(reader_->ReadSample(sidx, 0, nullptr, &flags, &timestamp, &sample)))
        return read_result::failed;

    // Driver restart or mid-stream renegotiation: refresh the geometry before
    // any buffer math, since width_/height_ describe the old type.
    if (flags & (MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED
                 | MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED))
    {
        ir_log(L"stream media type changed");
        lit_metadata_ = -1;
        if (!read_current_type())
        {
            safe_release(sample);
            return read_result::failed;
        }
    }

    // Once a renegotiation lands on a subtype this code can't decode, every
    // sample until the next type change is undecodable too, not just the one
    // that carried the flag; copying it as luma would be garbage.
    if (!type_ok_)
    {
        safe_release(sample);
        return read_result::failed;
    }

    if (!sample) // stream tick or end of stream, not an error
        return read_result::skipped;

    lit_metadata_ = -1;
    {
        IMFAttributes* meta = nullptr;
        if (SUCCEEDED(sample->GetUnknown(MFSampleExtension_CaptureMetadata, IID_PPV_ARGS(&meta))))
        {
            // The attribute is UINT64 per the SDK; some drivers store UINT32.
            UINT64 lit = 0;
            if (SUCCEEDED(meta->GetUINT64(MF_CAPTURE_METADATA_FRAME_ILLUMINATION, &lit))
                || SUCCEEDED(meta->GetUINT32(MF_CAPTURE_METADATA_FRAME_ILLUMINATION,
                                             (UINT32*)&lit)))
                lit_metadata_ = lit ? 1 : 0;
            safe_release(meta);
        }
    }

    // Strobe off-phase frames get dropped anyway; skip the buffer copy. Only
    // while strobing: a driver can tag non-illuminated frames even with the
    // emitter off, and dropping those would black the feed entirely.
    if (strobing_ && lit_metadata_ == 0)
    {
        safe_release(sample);
        return read_result::skipped;
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
                // Only the first plane is copied: L8 is all luma, and NV12's
                // leading plane is luma with the chroma after it. Rows are
                // laid out at the type's default stride, which can exceed
                // the width; the last row need not carry its padding.
                const size_t stride = (size_t)stride_;
                const size_t span = stride * (height_ - 1) + width_;
                if (cur_len >= span)
                {
                    buf_.resize(need);
                    for (int y = 0; y < height_; y++)
                        memcpy(buf_.data() + (size_t)y * width_, p + (size_t)y * stride, width_);
                    last_mean_ = frame_mean(buf_.data(), need);
                    peak_mean_ = std::max(last_mean_, peak_mean_ * 0.98);
                    ok = true;
                }
                else if (buf_.empty()) // once, before the first good frame
                    ir_log(L"sample is %lu bytes, need %zu", (unsigned long)cur_len, span);
                buffer->Unlock();
            }
        }
        safe_release(buffer);
    }
    safe_release(sample);

    return ok ? read_result::frame : read_result::failed;
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
        const read_result r = read_sample();
        if (r == read_result::frame && sample_is_lit())
            return true;
        if ((int)(GetTickCount() - deadline) >= 0)
            return false;
        // A skipped read already blocked for the next sample; only a failed
        // ReadSample would spin without the pause.
        if (r == read_result::failed)
            Sleep(5);
    }
    return false;
}
