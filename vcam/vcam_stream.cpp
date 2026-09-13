// Copyright (c) 2026 Aanish Bhirud
// SPDX-License-Identifier: MIT

#include "vcam_stream.hpp"
#include "vcam_flag.hpp"

#include <algorithm>

namespace {

constexpr DWORD flag_poll_ms = 300;

// Advertise the frame rate the sensor actually runs; the pump may deliver
// lit frames slower, but pacing follows the camera, not this default.
HRESULT make_type(IMFMediaType** out, REFGUID subtype, int w, int h, int fps)
{
    winrt::com_ptr<IMFMediaType> type;
    HRESULT hr = MFCreateMediaType(type.put());
    if (FAILED(hr))
        return hr;

    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, subtype);
    MFSetAttributeSize(type.get(), MF_MT_FRAME_SIZE, w, h);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    MFSetAttributeRatio(type.get(), MF_MT_FRAME_RATE, (UINT32)fps, 1);
    MFSetAttributeRatio(type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    if (subtype == MFVideoFormat_NV12)
    {
        type->SetUINT32(MF_MT_DEFAULT_STRIDE, w);
        type->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)((UINT64)w * h * 12 * fps));
    }
    else
    {
        type->SetUINT32(MF_MT_DEFAULT_STRIDE, w * 4);
        type->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)((UINT64)w * h * 32 * fps));
    }

    *out = type.detach();
    return S_OK;
}

// l8_len can differ from w*h when the capture opened a differently-sized
// stream than the one advertised; callers must not read past it.
void fill_nv12(BYTE* dst, LONG pitch, const uint8_t* l8, size_t l8_len, int w, int h)
{
    const bool have = l8 && l8_len >= (size_t)w * h;
    for (int y = 0; y < h; y++)
    {
        if (have)
            memcpy(dst + (size_t)y * pitch, l8 + (size_t)y * w, w);
        else
            memset(dst + (size_t)y * pitch, 0, w);
    }
    memset(dst + (size_t)h * pitch, 0x80, (size_t)pitch * h / 2); // neutral chroma
}

void fill_rgb32(BYTE* dst, LONG pitch, const uint8_t* l8, size_t l8_len, int w, int h)
{
    const bool have = l8 && l8_len >= (size_t)w * h;
    for (int y = 0; y < h; y++)
    {
        BYTE* row = dst + (size_t)y * pitch;
        if (!have)
        {
            memset(row, 0, (size_t)w * 4);
            continue;
        }
        for (int x = 0; x < w; x++)
        {
            const uint8_t v = l8[(size_t)y * w + x];
            row[x * 4 + 0] = v;
            row[x * 4 + 1] = v;
            row[x * 4 + 2] = v;
            row[x * 4 + 3] = 0xFF;
        }
    }
}

} // ns

cyclops_media_stream::~cyclops_media_stream()
{
    Shutdown();
}

HRESULT cyclops_media_stream::Initialize(IMFMediaSource* parent, IMFMediaSource* physical,
                                         const std::wstring& physical_symlink)
{
    if (!parent)
        return E_POINTER;
    parent_.copy_from(parent);
    physical_.copy_from(physical);
    symlink_ = physical_symlink;
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    HRESULT hr = ensure_store();
    if (FAILED(hr))
        return hr;

    store->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
    store->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
    store->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
    store->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes::MFFrameSourceTypes_Color);

    // Learn the physical camera's frame size up front when we can, so the
    // advertised media types match what it will really deliver.
    if (physical_)
    {
        winrt::com_ptr<IMFPresentationDescriptor> pd;
        if (SUCCEEDED(physical_->CreatePresentationDescriptor(pd.put())))
        {
            winrt::com_ptr<IMFStreamDescriptor> sd;
            BOOL selected = FALSE;
            if (SUCCEEDED(pd->GetStreamDescriptorByIndex(0, &selected, sd.put())))
            {
                winrt::com_ptr<IMFMediaTypeHandler> th;
                winrt::com_ptr<IMFMediaType> mt;
                UINT32 w = 0, h = 0;
                if (SUCCEEDED(sd->GetMediaTypeHandler(th.put()))
                    && SUCCEEDED(th->GetCurrentMediaType(mt.put()))
                    && SUCCEEDED(MFGetAttributeSize(mt.get(), MF_MT_FRAME_SIZE, &w, &h)) && w && h)
                {
                    width_ = (int)w;
                    height_ = (int)h;
                    UINT32 fn = 0, fd = 0;
                    if (SUCCEEDED(MFGetAttributeRatio(mt.get(), MF_MT_FRAME_RATE, &fn, &fd)) && fd && fn)
                        fps_ = (int)(fn / fd);
                }
            }
        }
    }

    hr = MFCreateEventQueue(queue_.put());
    if (FAILED(hr))
        return hr;

    IMFMediaType* types[2]{};
    hr = make_type(&types[0], MFVideoFormat_NV12, width_, height_, fps_.load());
    if (SUCCEEDED(hr))
        hr = make_type(&types[1], MFVideoFormat_RGB32, width_, height_, fps_.load());
    if (FAILED(hr))
    {
        if (types[0])
            types[0]->Release();
        if (types[1])
            types[1]->Release();
        return hr;
    }

    hr = MFCreateStreamDescriptor(0, 2, types, descriptor_.put());
    types[0]->Release();
    types[1]->Release();
    if (FAILED(hr))
        return hr;

    // DevProxy reads the stream attributes off the descriptor, not just the
    // stream's own attribute store.
    descriptor_->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
    descriptor_->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
    descriptor_->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
    descriptor_->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes::MFFrameSourceTypes_Color);

    winrt::com_ptr<IMFMediaTypeHandler> handler;
    if (FAILED(descriptor_->GetMediaTypeHandler(handler.put())))
        return E_FAIL;
    winrt::com_ptr<IMFMediaType> nv12;
    if (SUCCEEDED(make_type(nv12.put(), MFVideoFormat_NV12, width_, height_, fps_.load())))
        handler->SetCurrentMediaType(nv12.get());

    ir_log(L"vcam stream init: %dx%d @%d", width_, height_, fps_.load());
    return S_OK;
}

bool cyclops_media_stream::open_capture()
{
    // Prefer the Frame-Server-provided source; it keeps exclusive-control
    // bookkeeping right. Fall back to opening the device ourselves.
    if (physical_)
    {
        if (cap_.open(physical_.get()))
            return true;
        ir_log(L"vcam: associated source open failed, trying direct");
        cap_.close();
    }
    if (!symlink_.empty() && cap_.open(symlink_))
        return true;

    for (const auto& d : enum_sensor_cameras())
    {
        if (cap_.open(d.symlink))
            return true;
    }
    return cap_.is_open();
}

// Backoff waits go through the stop event so Stop/Shutdown wakes the pump
// immediately instead of sitting out an 8s sleep.
void cyclops_media_stream::pump_sleep(DWORD ms)
{
    if (stop_event_)
        WaitForSingleObject(stop_event_, ms);
    else
        Sleep(ms);
}

// Serialized and bounded: a synchronous ReadSample can wedge in the driver
// with no cancel path, and an unbounded join here would hang Frame Server's
// whole camera stack. A pump that survives the timeout is terminated: it is
// stuck in a device call, not holding a lock we need.
void cyclops_media_stream::join_pump()
{
    std::lock_guard<std::mutex> g(join_mu_);
    if (!pump_.joinable())
        return;
    if (WaitForSingleObject(pump_.native_handle(), 5000) == WAIT_OBJECT_0)
    {
        pump_.join();
        return;
    }
    ir_log(L"vcam: pump wedged past 5s, terminating");
    TerminateThread(pump_.native_handle(), 1);
    pump_.detach();
}

void cyclops_media_stream::pump_loop()
{
    int failures = 0;
    DWORD backoff_ms = 500;
    while (!pump_stop_.load())
    {
        if (!cap_.is_open())
        {
            cap_.close();
            if (!open_capture())
            {
                pump_sleep(backoff_ms);
                backoff_ms = std::min<DWORD>(backoff_ms * 2, 8000);
                continue;
            }
            if (cap_.fps() > 0)
                fps_ = cap_.fps(); // actual sensor rate trumps the probed value
            ir_log(L"vcam: capture open, %dx%d @%d", cap_.width(), cap_.height(), fps_.load());
            cap_.set_illumination(cyclops_vcam::read_illuminator_flag(true));
            failures = 0;
            backoff_ms = 500;
        }

        const DWORD now = GetTickCount();
        if (now - last_flag_poll_ > flag_poll_ms)
        {
            last_flag_poll_ = now;
            const bool want = cyclops_vcam::read_illuminator_flag(true);
            if (want != cap_.illumination_on())
                cap_.set_illumination(want);
        }

        if (cap_.read_frame(1500))
        {
            {
                std::lock_guard<std::mutex> g(frame_lock_);
                latest_ = cap_.pixels();
                have_frame_ = true;
            }
            fulfill_requests();
            failures = 0;
            backoff_ms = 500;
        }
        // A stalled read path must not spin: every reopen commits a FACEAUTH
        // change on the emitter, so churn is hard on the hardware too.
        else if (++failures > 5)
        {
            ir_log(L"vcam: frames stalled, reopening");
            cap_.close();
            failures = 0;
            pump_sleep(backoff_ms);
            backoff_ms = std::min<DWORD>(backoff_ms * 2, 8000);
        }
    }
    cap_.close(); // restores FACEAUTH_MODE_DISABLED
}

HRESULT cyclops_media_stream::SetAllocator(IUnknown* alloc)
{
    if (!alloc)
        return E_POINTER;
    winrt::com_ptr<IMFVideoSampleAllocator> a;
    HRESULT hr = alloc->QueryInterface(IID_PPV_ARGS(a.put()));
    if (FAILED(hr))
        return hr;
    winrt::slim_lock_guard g(lock_);
    if (allocator_ && allocator_ready_)
    {
        allocator_->UninitializeSampleAllocator();
        allocator_ready_ = false;
    }
    allocator_ = a;
    if (d3d_manager_)
        allocator_->SetDirectXManager(d3d_manager_.get());
    return S_OK;
}

HRESULT cyclops_media_stream::set_d3d_manager(IUnknown* manager)
{
    winrt::slim_lock_guard g(lock_);
    d3d_manager_.copy_from(manager);
    if (allocator_)
        allocator_->SetDirectXManager(manager);
    return S_OK;
}

HRESULT cyclops_media_stream::Start(IMFMediaType* type)
{
    winrt::slim_lock_guard g(lock_);
    if (shutdown_ || !queue_ || !descriptor_)
        return MF_E_SHUTDOWN;

    if (state_.load() == MF_STREAM_STATE_RUNNING)
    {
        // Idempotent Start; a new type mid-run is a renegotiation: adopt it
        // and rebind the allocator lazily on the next RequestSample.
        if (!type)
            return S_OK;
        GUID g2 = GUID_NULL;
        type->GetGUID(MF_MT_SUBTYPE, &g2);
        if (g2 == format_)
            return S_OK;
        current_type_.copy_from(type);
        format_ = g2;
        if (allocator_ && allocator_ready_)
        {
            allocator_->UninitializeSampleAllocator();
            allocator_ready_ = false;
        }
        return S_OK;
    }

    if (!type)
    {
        // SetStreamState(RUNNING) path: reuse the negotiated current type.
        winrt::com_ptr<IMFMediaTypeHandler> handler;
        current_type_ = nullptr;
        if (SUCCEEDED(descriptor_->GetMediaTypeHandler(handler.put())))
            handler->GetCurrentMediaType(current_type_.put());
    }
    else
    {
        current_type_.copy_from(type);
    }
    format_ = GUID_NULL;
    if (current_type_)
        current_type_->GetGUID(MF_MT_SUBTYPE, &format_);
    if (format_ == GUID_NULL)
        format_ = MFVideoFormat_NV12;

    // Frame Server only pulls samples once it has handed us its allocator;
    // InitializeSampleAllocator binds it to the negotiated media type.
    if (allocator_ && current_type_)
        allocator_ready_ = SUCCEEDED(allocator_->InitializeSampleAllocator(10, current_type_.get()));

    {
        std::lock_guard<std::mutex> fg(frame_lock_);
        have_frame_ = false;
        latest_.clear();
    }
    pump_stop_.store(false);
    if (stop_event_)
        ResetEvent(stop_event_); // manual-reset: disarm or backoff never sleeps
    // Under lock_: join_pump is bounded, and holding the lock serializes the
    // pump_ = std::thread assignment against a concurrent Start.
    join_pump();
    pump_ = std::thread([this] { pump_loop(); });

    // Event first: a RequestSample gated on RUNNING must not let a sample
    // overtake MEStreamStarted.
    queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr);
    state_.store(MF_STREAM_STATE_RUNNING);
    ir_log(L"vcam stream started, fmt=0x%x, allocator=%d", (unsigned)format_.Data1,
           allocator_ ? 1 : 0);
    return S_OK;
}

HRESULT cyclops_media_stream::Stop()
{
    winrt::com_ptr<IMFMediaSource> phys;
    {
        winrt::slim_lock_guard g(lock_);
        if (!queue_)
            return MF_E_SHUTDOWN;
        if (state_.load() != MF_STREAM_STATE_RUNNING)
            return S_OK;
        // Claim the stop under the lock: state reads STOPPED from here, so a
        // second Stop early-outs and only one thread ever joins the pump.
        state_.store(MF_STREAM_STATE_STOPPED);
        pump_stop_.store(true);
        if (stop_event_)
            SetEvent(stop_event_);
        phys = physical_;
    }

    // Join outside lock_: bounded inside join_pump, but the lock still must
    // not be held across it.
    join_pump();
    cap_.close();
    // The reader is gone but the source keeps streaming to nowhere; Stop it.
    if (phys)
        phys->Stop();

    winrt::slim_lock_guard g(lock_);
    if (allocator_ && allocator_ready_)
    {
        allocator_->UninitializeSampleAllocator();
        allocator_ready_ = false;
    }
    pending_.clear(); // stopped streams answer no requests
    if (queue_) // Shutdown may have nulled it while the join was in flight
        queue_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
    return S_OK;
}

void cyclops_media_stream::Shutdown()
{
    winrt::com_ptr<IMFMediaSource> phys;
    {
        winrt::slim_lock_guard g(lock_);
        if (shutdown_)
            return;
        shutdown_ = true; // Start can no longer re-enter during the join below
        pump_stop_.store(true);
        state_.store(MF_STREAM_STATE_STOPPED);
        if (stop_event_)
            SetEvent(stop_event_);
        phys = physical_;
    }
    join_pump();
    cap_.close(); // restores FACEAUTH_MODE_DISABLED
    // Runtime stop only: teardown (Shutdown/ShutdownObject) belongs to the
    // activator that resolved this camera.
    if (phys)
        phys->Stop();

    winrt::slim_lock_guard g(lock_);
    if (allocator_ && allocator_ready_)
    {
        allocator_->UninitializeSampleAllocator();
        allocator_ready_ = false;
    }
    pending_.clear();
    if (queue_)
    {
        queue_->Shutdown();
        queue_ = nullptr;
    }
    descriptor_ = nullptr;
    parent_ = nullptr;
    physical_ = nullptr;
    current_type_ = nullptr;
    if (stop_event_)
    {
        CloseHandle(stop_event_);
        stop_event_ = nullptr;
    }
}

// ---- IMFMediaEventGenerator ---------------------------------------------------

STDMETHODIMP cyclops_media_stream::BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state)
{
    winrt::slim_lock_guard g(lock_);
    if (!queue_)
        return MF_E_SHUTDOWN;
    return queue_->BeginGetEvent(cb, state);
}

STDMETHODIMP cyclops_media_stream::EndGetEvent(IMFAsyncResult* res, IMFMediaEvent** evt)
{
    if (!evt)
        return E_POINTER;
    *evt = nullptr;
    winrt::slim_lock_guard g(lock_);
    if (!queue_)
        return MF_E_SHUTDOWN;
    return queue_->EndGetEvent(res, evt);
}

STDMETHODIMP cyclops_media_stream::GetEvent(DWORD flags, IMFMediaEvent** evt)
{
    if (!evt)
        return E_POINTER;
    *evt = nullptr;
    winrt::com_ptr<IMFMediaEventQueue> queue;
    {
        winrt::slim_lock_guard g(lock_);
        if (!queue_)
            return MF_E_SHUTDOWN;
        queue = queue_;
    }
    // GetEvent can block indefinitely; holding lock_ across it would wedge
    // every RequestSample and state call.
    return queue->GetEvent(flags, evt);
}

STDMETHODIMP cyclops_media_stream::QueueEvent(MediaEventType met, REFGUID ext, HRESULT st, const PROPVARIANT* v)
{
    winrt::slim_lock_guard g(lock_);
    if (!queue_)
        return MF_E_SHUTDOWN;
    return queue_->QueueEventParamVar(met, ext, st, v);
}

// ---- IMFMediaStream -----------------------------------------------------------

STDMETHODIMP cyclops_media_stream::GetMediaSource(IMFMediaSource** src)
{
    if (!src)
        return E_POINTER;
    *src = nullptr;
    winrt::slim_lock_guard g(lock_);
    if (!parent_)
        return MF_E_SHUTDOWN;
    parent_.copy_to(src);
    return S_OK;
}

STDMETHODIMP cyclops_media_stream::GetStreamDescriptor(IMFStreamDescriptor** desc)
{
    if (!desc)
        return E_POINTER;
    *desc = nullptr;
    winrt::slim_lock_guard g(lock_);
    if (!descriptor_)
        return MF_E_SHUTDOWN;
    descriptor_.copy_to(desc);
    return S_OK;
}

STDMETHODIMP cyclops_media_stream::RequestSample(IUnknown* token)
{
    {
        winrt::slim_lock_guard g(lock_);
        if (!queue_)
            return MF_E_SHUTDOWN;
        if (state_.load() != MF_STREAM_STATE_RUNNING)
            return MF_E_INVALIDREQUEST;
        if (!allocator_)
            return MF_E_NOT_INITIALIZED;
        if (!req_logged_)
        {
            req_logged_ = true;
            ir_log(L"vcam: first RequestSample (paced)");
        }
        // Requests ride the next real camera frame; past the cap we still
        // answer synchronously rather than silently lose the request.
        if (pending_.size() < 32)
        {
            pending_.emplace_back();
            if (token)
                pending_.back().copy_from(token);
            return S_OK;
        }
    }
    return produce_sample(token);
}

// Called by the pump after each fresh frame: every queued request gets that
// frame, so delivery rate tracks the camera, not the client's read speed.
void cyclops_media_stream::fulfill_requests()
{
    for (;;)
    {
        winrt::com_ptr<IUnknown> token;
        {
            winrt::slim_lock_guard g(lock_);
            if (pending_.empty())
                return;
            token = std::move(pending_.front());
            pending_.pop_front();
        }
        produce_sample(token.get()); // errors are unrecoverable; drop and move on
    }
}

HRESULT cyclops_media_stream::produce_sample(IUnknown* token)
{
    // Snapshot under the lock; allocate/fill/queue outside it so Frame Server
    // calls (GetEvent, Stop, Shutdown) never wedge behind a buffer lock.
    winrt::com_ptr<IMFMediaEventQueue> queue;
    winrt::com_ptr<IMFVideoSampleAllocator> alloc;
    GUID format = GUID_NULL;
    int w, h, fps;
    {
        winrt::slim_lock_guard g(lock_);
        if (!queue_)
            return MF_E_SHUTDOWN;
        if (state_.load() != MF_STREAM_STATE_RUNNING)
            return MF_E_INVALIDREQUEST;
        if (!allocator_)
            return MF_E_NOT_INITIALIZED;

        if (!allocator_ready_)
        {
            // The allocator arrived after Start; bind it to the media type now.
            if (!current_type_
                || FAILED(allocator_->InitializeSampleAllocator(10, current_type_.get())))
                return MF_E_NOT_INITIALIZED;
            allocator_ready_ = true;
        }
        queue = queue_;
        alloc = allocator_;
        format = format_;
        w = width_;
        h = height_;
        fps = fps_.load();
    }

    // Samples must come from the client-provided allocator: they are shared
    // buffers Frame Server can marshal across to the consuming process.
    winrt::com_ptr<IMFSample> sample;
    HRESULT hr = alloc->AllocateSample(sample.put());
    if (FAILED(hr) || !sample)
        return FAILED(hr) ? hr : E_FAIL;

    winrt::com_ptr<IMFMediaBuffer> buf;
    hr = sample->GetBufferByIndex(0, buf.put());
    if (FAILED(hr))
        return hr;

    winrt::com_ptr<IMF2DBuffer2> buf2d;
    const bool is2d = SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(buf2d.put())));

    const LONG row_bytes = (format == MFVideoFormat_RGB32) ? (LONG)w * 4 : (LONG)w;
    BYTE* p = nullptr;
    LONG pitch = row_bytes;
    DWORD buf_len = 0;
    if (is2d)
    {
        BYTE* start = nullptr;
        hr = buf2d->Lock2DSize(MF2DBuffer_LockFlags_Write, &p, &pitch, &start, &buf_len);
    }
    else
    {
        DWORD cur_len = 0;
        hr = buf->Lock(&p, &buf_len, &cur_len);
    }
    if (FAILED(hr) || !p)
        return FAILED(hr) ? hr : E_FAIL;

    // pitch<0 is valid for bottom-up RGB32 (scanline0 plus negative offsets);
    // NV12 has a chroma plane below the luma rows, so negative pitch can't work.
    const bool pitch_bad = pitch == 0
        || (format == MFVideoFormat_NV12 && pitch < row_bytes)
        || (format == MFVideoFormat_RGB32 && (pitch > -row_bytes && pitch < row_bytes));
    const size_t need = (format == MFVideoFormat_NV12)
        ? (size_t)pitch * h * 3 / 2
        : (size_t)(pitch < 0 ? -(long long)pitch : pitch) * h;
    if (pitch_bad || buf_len < need)
    {
        if (is2d)
            buf2d->Unlock2D();
        else
            buf->Unlock();
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }

    {
        std::lock_guard<std::mutex> fg(frame_lock_);
        const uint8_t* src = have_frame_ ? latest_.data() : nullptr;
        const size_t src_len = have_frame_ ? latest_.size() : 0;
        if (format == MFVideoFormat_RGB32)
            fill_rgb32(p, pitch, src, src_len, w, h);
        else
            fill_nv12(p, pitch, src, src_len, w, h);
    }
    if (is2d)
        buf2d->Unlock2D();
    else
        buf->Unlock();

    sample->SetSampleTime(MFGetSystemTime());
    sample->SetSampleDuration(fps > 0 ? 10000000LL / fps : 333333);
    if (token)
        sample->SetUnknown(MFSampleExtension_Token, token);

    return queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.get());
}

// ---- IMFMediaStream2 ----------------------------------------------------------

STDMETHODIMP cyclops_media_stream::SetStreamState(MF_STREAM_STATE value)
{
    if (value == state_.load())
        return S_OK;
    switch (value)
    {
    case MF_STREAM_STATE_RUNNING: return Start(nullptr);
    case MF_STREAM_STATE_STOPPED: return Stop();
    default: return MF_E_INVALID_STATE_TRANSITION;
    }
}

STDMETHODIMP cyclops_media_stream::GetStreamState(MF_STREAM_STATE* value)
{
    if (!value)
        return E_POINTER;
    *value = state_.load();
    return S_OK;
}

// ---- IKsControl ---------------------------------------------------------------
//
// Extended camera control calls from client apps (and from Cyclops itself when
// it previews through the virtual camera) land here. FACEAUTH requests get
// forwarded to the physical camera's own extended controller.

winrt::com_ptr<IKsControl> cyclops_media_stream::physical_ks_control()
{
    winrt::com_ptr<IKsControl> ks;
    winrt::slim_lock_guard g(lock_);
    if (physical_)
        physical_->QueryInterface(IID_PPV_ARGS(ks.put()));
    return ks;
}

STDMETHODIMP_(NTSTATUS) cyclops_media_stream::KsProperty(PKSPROPERTY prop, ULONG len, LPVOID data,
                                                        ULONG data_len, ULONG* ret)
{
    if (!prop || !ret)
        return E_POINTER;
    if (len < sizeof(KSPROPERTY))
        return E_INVALIDARG;

    if (IsEqualGUID(prop->Set, KSPROPERTYSETID_ExtendedCameraControl)
        && prop->Id == KSPROPERTY_CAMERACONTROL_EXTENDED_FACEAUTH_MODE)
    {
        // Windows probes control support with a zero-length data buffer.
        if (!data || data_len < sizeof(KSCAMERA_EXTENDEDPROP_HEADER))
        {
            *ret = sizeof(KSCAMERA_EXTENDEDPROP_HEADER);
            return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
        }

        auto* ec = (KSCAMERA_EXTENDEDPROP_HEADER*)data;
        if (prop->Flags & KSPROPERTY_TYPE_SET)
        {
            const bool on = (ec->Flags & (KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_ALTERNATIVE_FRAME_ILLUMINATION
                                          | KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_BACKGROUND_SUBTRACTION)) != 0;
            // cap_ belongs to the pump thread; the flag file is the channel.
            cyclops_vcam::write_illuminator_flag(on);
            *ret = 0;
            return S_OK;
        }
        if (prop->Flags & KSPROPERTY_TYPE_GET)
        {
            const bool on = cyclops_vcam::read_illuminator_flag(true);
            ec->Version = 1;
            ec->PinId = (ULONG)KSCAMERA_EXTENDEDPROP_FILTERSCOPE;
            ec->Size = sizeof(KSCAMERA_EXTENDEDPROP_HEADER);
            ec->Result = 0;
            ec->Capability = KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_DISABLED
                | KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_ALTERNATIVE_FRAME_ILLUMINATION
                | KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_BACKGROUND_SUBTRACTION;
            ec->Flags = on ? KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_ALTERNATIVE_FRAME_ILLUMINATION
                           : KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_DISABLED;
            *ret = sizeof(KSCAMERA_EXTENDEDPROP_HEADER);
            return S_OK;
        }
    }

    // Everything else goes to the physical camera's own control surface.
    winrt::com_ptr<IKsControl> ks = physical_ks_control();
    if (ks)
        return ks->KsProperty(prop, len, data, data_len, ret);
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) cyclops_media_stream::KsMethod(PKSMETHOD m, ULONG len, LPVOID data,
                                                      ULONG data_len, ULONG* ret)
{
    if (!ret)
        return E_POINTER;
    winrt::com_ptr<IKsControl> ks = physical_ks_control();
    if (ks)
        return ks->KsMethod(m, len, data, data_len, ret);
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) cyclops_media_stream::KsEvent(PKSEVENT evt, ULONG len, LPVOID data,
                                                     ULONG data_len, ULONG* ret)
{
    if (!ret)
        return E_POINTER;
    winrt::com_ptr<IKsControl> ks = physical_ks_control();
    if (ks)
        return ks->KsEvent(evt, len, data, data_len, ret);
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
