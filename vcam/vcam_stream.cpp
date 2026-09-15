// Copyright (c) 2026 Aanish Bhirud
// SPDX-License-Identifier: MIT

#include "vcam_stream.hpp"
#include "vcam_flag.hpp"

#include <algorithm>

namespace {

constexpr DWORD flag_poll_ms = 300;

// NV12 only. MF RGB32 surfaces through DirectShow as BI_RGB with a negative
// (top-down) biHeight, and Discord's aspect-ratio reducer infinite-loops on
// it; NV12 carries no signed height field at all.
HRESULT make_type(IMFMediaType** out, int w, int h, int fps)
{
    winrt::com_ptr<IMFMediaType> type;
    HRESULT hr = MFCreateMediaType(type.put());
    if (FAILED(hr))
        return hr;

    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(type.get(), MF_MT_FRAME_SIZE, w, h);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    MFSetAttributeRatio(type.get(), MF_MT_FRAME_RATE, (UINT32)fps, 1);
    MFSetAttributeRatio(type.get(), MF_MT_FRAME_RATE_RANGE_MIN, (UINT32)fps, 1);
    MFSetAttributeRatio(type.get(), MF_MT_FRAME_RATE_RANGE_MAX, (UINT32)fps, 1);
    MFSetAttributeRatio(type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    type->SetUINT32(MF_MT_DEFAULT_STRIDE, w);
    type->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)((UINT64)w * h * 12 * fps));
    type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    type->SetUINT32(MF_MT_SAMPLE_SIZE, (UINT32)(w * h * 3 / 2));
    // The L8 luma we convert is full-range gray, not studio swing.
    type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_0_255);

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
    // advertised media type matches what the pump's capture will deliver.
    // Same stream choice and rounding as ir_capture, or the two disagree.
    if (physical_)
    {
        int fps = fps_.load();
        if (probe_source_geometry(physical_.get(), width_, height_, fps))
            fps_ = fps;
    }

    hr = MFCreateEventQueue(queue_.put());
    if (FAILED(hr))
        return hr;

    IMFMediaType* types[1]{};
    hr = make_type(&types[0], width_, height_, fps_.load());
    if (FAILED(hr))
        return hr;

    hr = MFCreateStreamDescriptor(0, 1, types, descriptor_.put());
    types[0]->Release();
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
    if (FAILED(make_type(nv12.put(), width_, height_, fps_.load()))
        || FAILED(handler->SetCurrentMediaType(nv12.get())))
        return MF_E_INVALIDMEDIATYPE;

    ir_log(L"vcam stream init: %dx%d @%d", width_, height_, fps_.load());
    return S_OK;
}

bool cyclops_media_stream::open_capture()
{
    // Open the sensor directly first: the Frame-Server-provided associated
    // source streams fine but its proxy exposes no illuminating FACEAUTH
    // mode (measured on the N930W), and a virtual camera that can't drive
    // the emitter is the one thing this device exists to avoid. The
    // associated source is the fallback for a sensor we can't open by name.
    // This device serves the registered sensor only: never fall through to
    // another camera a client did not ask for. Without a persisted symlink,
    // whatever sensor enumerates is the one.
    if (!symlink_.empty())
    {
        if (cap_.open(symlink_))
            return true;
        ir_log(L"vcam: direct open of the registered sensor failed");
    }
    else
    {
        for (const auto& d : enum_sensor_cameras())
            if (cap_.open(d.symlink))
                return true;
    }
    if (physical_ && cap_.open(physical_.get()))
    {
        ir_log(L"vcam: using associated source, emitter %ls",
               cap_.illumination_supported() ? L"available" : L"unavailable");
        return true;
    }
    return false;
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
// whole camera stack. A pump that survives the timeout is abandoned, never
// terminated: a killed thread can orphan MF-internal locks (frame_lock_, the
// allocator's, the event queue's) and deadlock the service. The pump holds a
// strong ref on this stream so it can't outlive its members, and it frees
// cap_ itself on the way out; until it dies cap_ is off-limits.
void cyclops_media_stream::join_pump_locked()
{
    if (!pump_.joinable())
        return;
    if (WaitForSingleObject(pump_.native_handle(), 5000) == WAIT_OBJECT_0)
    {
        pump_.join();
        return;
    }
    ir_log(L"vcam: pump wedged past 5s, abandoning (still owns capture)");
    cap_abandoned_.store(true);
    pump_.detach();
}

void cyclops_media_stream::join_pump()
{
    std::lock_guard<std::mutex> g(join_mu_);
    join_pump_locked();
}

// One serialized tail for Stop and Shutdown: join_mu_ covers join, capture
// close, and the physical source Stop so concurrent callers can't run
// cap_.close() twice or race a new pump start.
void cyclops_media_stream::stop_tail(IMFMediaSource* phys)
{
    std::lock_guard<std::mutex> g(join_mu_);
    join_pump_locked();
    if (!cap_abandoned_.load())
        cap_.close(); // restores FACEAUTH_MODE_DISABLED
    if (phys)
        phys->Stop();
}

// Queued once per failure streak so a permanently unreachable camera surfaces
// as an error to the client instead of a live stream that produces nothing.
void cyclops_media_stream::note_failure()
{
    if (!fail_since_)
        fail_since_ = GetTickCount();
    if (error_sent_ || GetTickCount() - fail_since_ < 10000)
        return;
    winrt::com_ptr<IMFMediaEventQueue> q;
    {
        winrt::slim_lock_guard g(lock_);
        q = queue_;
    }
    if (q)
        q->QueueEventParamVar(MEError, GUID_NULL,
                              MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED, nullptr);
    error_sent_ = true;
    ir_log(L"vcam: capture unreachable for 10s, MEError queued");
}

void cyclops_media_stream::pump_loop()
{
    // Device source creation goes through COM activation; give the thread an
    // apartment rather than relying on the process's implicit MTA.
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int failures = 0;
    DWORD backoff_ms = 500;
    while (!pump_stop_.load())
    {
        if (!cap_.is_open())
        {
            cap_.close();
            if (!open_capture())
            {
                note_failure();
                pump_sleep(backoff_ms);
                backoff_ms = std::min<DWORD>(backoff_ms * 2, 8000);
                continue;
            }
            if (cap_.fps() > 0)
                fps_ = cap_.fps(); // actual sensor rate trumps the probed value
            ir_log(L"vcam: capture open, %dx%d @%d", cap_.width(), cap_.height(), fps_.load());
            // The advertised type is fixed at Initialize; a capture of another
            // size is black or sheared on the client side (fill_nv12), so say so.
            if (cap_.width() != width_ || cap_.height() != height_)
                ir_log(L"vcam: capture is %dx%d but the stream advertises %dx%d",
                       cap_.width(), cap_.height(), width_, height_);
            cap_.set_illumination(cyclops_vcam::read_illuminator_flag(false));
            failures = 0;
            backoff_ms = 500;
            fail_since_ = 0;
            error_sent_ = false;
        }

        const DWORD now = GetTickCount();
        if (now - last_flag_poll_ > flag_poll_ms)
        {
            last_flag_poll_ = now;
            const bool want = cyclops_vcam::read_illuminator_flag(false);
            if (want != cap_.illumination_on())
                cap_.set_illumination(want);
        }

        if (cap_.read_frame(1500))
        {
            if (pump_stop_.load())
                break;
            {
                std::lock_guard<std::mutex> g(frame_lock_);
                latest_ = cap_.pixels();
                have_frame_ = true;
            }
            fulfill_requests();
            failures = 0;
            backoff_ms = 500;
            fail_since_ = 0;
            error_sent_ = false;
        }
        // A stalled read path must not spin: every reopen commits a FACEAUTH
        // change on the emitter, so churn is hard on the hardware too.
        else if (++failures > 5)
        {
            note_failure();
            ir_log(L"vcam: frames stalled, reopening");
            cap_.close();
            failures = 0;
            pump_sleep(backoff_ms);
            backoff_ms = std::min<DWORD>(backoff_ms * 2, 8000);
        }
    }
    cap_.close(); // restores FACEAUTH_MODE_DISABLED
    if (SUCCEEDED(ci))
        CoUninitialize();
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
    if (allocator_)
    {
        // A mid-run swap can't be uninitialized here: the pump thread may be
        // inside AllocateSample on it. Retire it; the pump uninitializes
        // retired allocators between samples.
        if (state_.load() == MF_STREAM_STATE_RUNNING)
            retiring_.push_back(allocator_);
        else if (allocator_ready_)
        {
            allocator_->UninitializeSampleAllocator();
            allocator_ready_ = false;
        }
    }
    allocator_ = a;
    // The new allocator has not been bound to the media type; produce_sample
    // initializes it lazily (a stale true here made it allocate uninitialized).
    allocator_ready_ = false;
    alloc_cb_ = nullptr;
    allocator_->QueryInterface(IID_PPV_ARGS(alloc_cb_.put()));
    if (d3d_manager_)
        allocator_->SetDirectXManager(d3d_manager_.get());
    return S_OK;
}

HRESULT cyclops_media_stream::set_d3d_manager(IUnknown* manager)
{
    winrt::slim_lock_guard g(lock_);
    d3d_manager_.copy_from(manager);
    if (state_.load() == MF_STREAM_STATE_RUNNING)
        d3d_dirty_ = true; // rebind happens on the pump thread between samples
    else if (allocator_)
        allocator_->SetDirectXManager(manager);
    return S_OK;
}

// DetachObject handoff: act==nullptr marks a self-opened source (teardown is
// Shutdown, not ShutdownObject). Refused once this stream has shut down: its
// Shutdown will never run again, so the caller keeps teardown.
bool cyclops_media_stream::adopt_physical(IMFActivate* act)
{
    winrt::slim_lock_guard g(lock_);
    if (shutdown_)
        return false;
    physical_activate_.copy_from(act);
    owns_physical_ = true;
    return true;
}

// Only the advertised shape is accepted: NV12 at the sensor's resolution.
// A client-negotiated type we can't actually produce would deliver garbage.
HRESULT cyclops_media_stream::validate_type(IMFMediaType* type)
{
    GUID sub = GUID_NULL;
    if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &sub)) || sub != MFVideoFormat_NV12)
        return MF_E_INVALIDMEDIATYPE;
    UINT32 w = 0, h = 0;
    if (FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h))
        || (int)w != width_ || (int)h != height_)
        return MF_E_INVALIDMEDIATYPE;
    return S_OK;
}

HRESULT cyclops_media_stream::Start(IMFMediaType* type)
{
    {
        winrt::slim_lock_guard g(lock_);
        if (shutdown_ || !queue_ || !descriptor_)
            return MF_E_SHUTDOWN;
        // A wedged pump still owns the capture; starting a second one would
        // race it on the physical camera.
        if (cap_abandoned_.load())
            return MF_E_UNEXPECTED;

        if (state_.load() == MF_STREAM_STATE_PAUSED)
        {
            // Resume: pump and allocator are untouched, just reopen the gate.
            state_.store(MF_STREAM_STATE_RUNNING);
            PROPVARIANT ts;
            PropVariantInit(&ts);
            InitPropVariantFromInt64(MFGetSystemTime(), &ts);
            queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, &ts);
            PropVariantClear(&ts);
            return S_OK;
        }

        if (state_.load() == MF_STREAM_STATE_RUNNING)
        {
            // Idempotent Start; a new type mid-run is a renegotiation: adopt
            // it and rebind the allocator lazily on the next RequestSample.
            if (!type)
                return S_OK;
            GUID g2 = GUID_NULL;
            type->GetGUID(MF_MT_SUBTYPE, &g2);
            if (g2 == format_)
                return S_OK;
            if (FAILED(validate_type(type)))
                return MF_E_INVALIDMEDIATYPE;
            current_type_.copy_from(type);
            format_ = g2;
            if (allocator_ && allocator_ready_)
            {
                // Same rule as SetAllocator: the pump may be inside
                // AllocateSample, so retire it and let the pump uninitialize
                // between samples; produce_sample re-binds to the new type.
                retiring_.push_back(allocator_);
                allocator_ready_ = false;
            }
            return S_OK;
        }
    }

    // STOPPED. A Stop may still be joining the previous pump on another
    // thread; that join must not run under lock_ (the pump takes lock_ to
    // exit), so wait for it first, then re-check: the state can have moved.
    join_pump();

    winrt::slim_lock_guard g(lock_);
    if (shutdown_ || !queue_ || !descriptor_)
        return MF_E_SHUTDOWN;
    if (cap_abandoned_.load())
        return MF_E_UNEXPECTED;
    if (state_.load() != MF_STREAM_STATE_STOPPED)
        return S_OK; // a concurrent Start already brought the pump up

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
        if (FAILED(validate_type(type)))
            return MF_E_INVALIDMEDIATYPE;
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
    need_discontinuity_ = true;
    // The previous pump was joined above, before lock_, so nothing else reads
    // pump_stop_ or pump_ here; lock_ serializes this against another Start.
    pump_stop_.store(false);
    if (stop_event_)
        ResetEvent(stop_event_); // manual-reset: disarm or backoff never sleeps
    // The pump holds a ref on the stream so an abandoned (wedged) pump can't
    // outlive the members it touches.
    winrt::com_ptr<cyclops_media_stream> self;
    self.copy_from(this);
    pump_ = std::thread([self = std::move(self)] { self->pump_loop(); });

    // Event first: a RequestSample gated on RUNNING must not let a sample
    // overtake MEStreamStarted. Timestamp it so clients can rebase stream time.
    PROPVARIANT ts;
    PropVariantInit(&ts);
    InitPropVariantFromInt64(MFGetSystemTime(), &ts);
    queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, &ts);
    PropVariantClear(&ts);
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
        // PAUSED still has a live pump (pause only gates delivery), so it
        // stops the same way RUNNING does.
        if (state_.load() == MF_STREAM_STATE_STOPPED)
            return S_OK;
        // Claim the stop under the lock: state reads STOPPED from here, so a
        // second Stop early-outs and only one thread ever joins the pump.
        state_.store(MF_STREAM_STATE_STOPPED);
        pump_stop_.store(true);
        if (stop_event_)
            SetEvent(stop_event_);
        phys = physical_;
    }

    // Serialized tail: join, cap close, source stop run under join_mu_ so a
    // concurrent Shutdown can't re-enter cap_.close() or race the join.
    stop_tail(phys.get());

    winrt::slim_lock_guard g(lock_);
    if (allocator_ && allocator_ready_)
    {
        allocator_->UninitializeSampleAllocator();
        allocator_ready_ = false;
    }
    for (auto& a : retiring_)
        a->UninitializeSampleAllocator();
    retiring_.clear();
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
    stop_tail(phys.get());

    // Physical-session teardown lands here only if the activator detached;
    // otherwise the activator owns Shutdown/ShutdownObject for this camera.
    winrt::com_ptr<IMFActivate> pact;
    winrt::com_ptr<IMFMediaSource> phys_teardown;
    {
        winrt::slim_lock_guard g(lock_);
        if (owns_physical_)
        {
            owns_physical_ = false;
            pact = physical_activate_;
            phys_teardown = physical_;
            physical_activate_ = nullptr;
        }
    }
    if (pact)
        pact->ShutdownObject();
    else if (phys_teardown)
        phys_teardown->Shutdown();

    winrt::com_ptr<IMFMediaEventQueue> queue;
    {
        winrt::slim_lock_guard g(lock_);
        if (allocator_ && allocator_ready_)
        {
            allocator_->UninitializeSampleAllocator();
            allocator_ready_ = false;
        }
        for (auto& a : retiring_)
            a->UninitializeSampleAllocator();
        retiring_.clear();
        pending_.clear();
        queue = queue_;
        queue_ = nullptr;
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
    // Off-lock, as in the source: a pending BeginGetEvent completes during
    // queue shutdown and can re-enter this object's event methods, which take
    // lock_ (a slim_mutex is not recursive).
    if (queue)
        queue->Shutdown();
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
        // Requests ride real camera frames one-for-one. Past the cap the
        // oldest is dropped rather than answered with a duplicate frame;
        // clients hold ~10 in flight, so this is a pathological-flood bound.
        if (pending_.size() >= 512)
        {
            pending_.pop_front();
            if (!overflow_logged_.exchange(true))
                ir_log(L"vcam: request queue overflow, dropping oldest");
        }
        pending_.emplace_back();
        if (token)
            pending_.back().copy_from(token);
    }
    return S_OK;
}

// Called by the pump after each fresh frame: the oldest queued request gets
// that frame. One request per frame keeps delivery at the sensor's real rate
// no matter how deep the client's in-flight request pool is.
void cyclops_media_stream::fulfill_requests()
{
    winrt::com_ptr<IUnknown> token;
    {
        winrt::slim_lock_guard g(lock_);
        if (pending_.empty())
            return;
        // Pause gates delivery only: the requests stay queued for the resume,
        // since produce_sample would reject them as terminal and drop them.
        if (state_.load() != MF_STREAM_STATE_RUNNING)
            return;
        // AllocateSample blocks when the client's shared pool is drained;
        // blocking here wedges the pump and defeats the bounded join. Skip
        // the frame instead: the request stays queued for the next one.
        if (alloc_cb_)
        {
            LONG free_count = -1;
            if (SUCCEEDED(alloc_cb_->GetFreeSampleCount(&free_count)) && free_count == 0)
                return;
        }
        token = std::move(pending_.front());
        pending_.pop_front();
    }
    const HRESULT hr = produce_sample(token.get());
    // Transient failures (pool emptied between the free-count check and the
    // allocate, a rejected buffer lock) put the request back so delivery
    // resumes on the next frame. Terminal states are dropped.
    if (FAILED(hr) && hr != MF_E_SHUTDOWN && hr != MF_E_INVALIDREQUEST
        && hr != MF_E_NOT_INITIALIZED)
    {
        winrt::slim_lock_guard g(lock_);
        if (pending_.size() < 512)
            pending_.push_front(std::move(token));
    }
}

HRESULT cyclops_media_stream::produce_sample(IUnknown* token)
{
    // Snapshot under the lock; allocate/fill/queue outside it so Frame Server
    // calls (GetEvent, Stop, Shutdown) never wedge behind a buffer lock.
    winrt::com_ptr<IMFMediaEventQueue> queue;
    winrt::com_ptr<IMFVideoSampleAllocator> alloc;
    int w, h, fps;
    {
        winrt::slim_lock_guard g(lock_);
        if (!queue_)
            return MF_E_SHUTDOWN;
        if (state_.load() != MF_STREAM_STATE_RUNNING)
            return MF_E_INVALIDREQUEST;
        if (!allocator_)
            return MF_E_NOT_INITIALIZED;

        // Retired allocators are uninitialized here, on the pump thread,
        // between samples: tearing one down while another thread holds it
        // (SetAllocator swap, d3d rebind) corrupts the pool.
        for (auto& a : retiring_)
            a->UninitializeSampleAllocator();
        retiring_.clear();
        if (d3d_dirty_)
        {
            d3d_dirty_ = false;
            if (allocator_ && d3d_manager_)
                allocator_->SetDirectXManager(d3d_manager_.get());
        }

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

    const LONG row_bytes = (LONG)w;
    BYTE* p = nullptr;
    LONG pitch = row_bytes;
    DWORD buf_len = 0;
    if (is2d)
    {
        BYTE* start = nullptr;
        hr = buf2d->Lock2DSize(MF2DBuffer_LockFlags_Write, &p, &pitch, &start, &buf_len);
        // p may point into the buffer at an offset from start; buf_len counts
        // from start, so usable bytes at p shrink by the offset.
        if (SUCCEEDED(hr) && p && start && p >= start)
            buf_len -= (DWORD)(p - start);
    }
    else
    {
        DWORD cur_len = 0;
        hr = buf->Lock(&p, &buf_len, &cur_len);
    }
    if (FAILED(hr) || !p)
        return FAILED(hr) ? hr : E_FAIL;

    // NV12 has a chroma plane below the luma rows, so negative pitch can't work.
    const size_t need = (size_t)pitch * h * 3 / 2;
    if (pitch < row_bytes || buf_len < need)
    {
        if (is2d)
            buf2d->Unlock2D();
        else
            buf->Unlock();
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }

    bool discontinuity;
    {
        std::lock_guard<std::mutex> fg(frame_lock_);
        const uint8_t* src = have_frame_ ? latest_.data() : nullptr;
        const size_t src_len = have_frame_ ? latest_.size() : 0;
        fill_nv12(p, pitch, src, src_len, w, h);
    }
    {
        winrt::slim_lock_guard g(lock_);
        discontinuity = need_discontinuity_;
        need_discontinuity_ = false;
    }
    // Current length is the contiguous (unpadded) image size, which is what a
    // client reading through IMFMediaBuffer::Lock sees; the pitch-padded
    // footprint can exceed a 2D buffer's max length and be rejected.
    buf->SetCurrentLength((DWORD)((size_t)w * h * 3 / 2));
    if (is2d)
        buf2d->Unlock2D();
    else
        buf->Unlock();

    sample->SetSampleTime(MFGetSystemTime());
    sample->SetSampleDuration(fps > 0 ? 10000000LL / fps : 333333);
    sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    if (discontinuity)
        sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
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
    case MF_STREAM_STATE_PAUSED:
    {
        // Pause just gates delivery: the pump keeps the camera warm and
        // RequestSample stops accepting until Start resumes.
        winrt::slim_lock_guard g(lock_);
        if (state_.load() != MF_STREAM_STATE_RUNNING)
            return MF_E_INVALID_STATE_TRANSITION;
        state_.store(MF_STREAM_STATE_PAUSED);
        return S_OK;
    }
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
        // FACEAUTH_MODE's payload is HEADER + VIDEOPROCSETTING (the docs'
        // Size for this control), even though only the header's Flags carry
        // meaning; a header-only reply fails the caller's size validation.
        constexpr ULONG payload = sizeof(KSCAMERA_EXTENDEDPROP_HEADER)
                                  + sizeof(KSCAMERA_EXTENDEDPROP_VIDEOPROCSETTING);
        if (prop->Flags & KSPROPERTY_TYPE_BASICSUPPORT)
        {
            // KS basic-support contract: a ULONG of access flags, or a
            // KSPROPERTY_DESCRIPTION when the buffer is that large.
            if (!data || data_len < sizeof(ULONG))
            {
                *ret = sizeof(ULONG);
                return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
            }
            const ULONG access = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET
                                 | KSPROPERTY_TYPE_BASICSUPPORT;
            if (data_len >= sizeof(KSPROPERTY_DESCRIPTION))
            {
                auto* d = (KSPROPERTY_DESCRIPTION*)data;
                memset(d, 0, sizeof(*d));
                d->AccessFlags = access;
                d->DescriptionSize = sizeof(KSPROPERTY_DESCRIPTION);
                *ret = sizeof(KSPROPERTY_DESCRIPTION);
                return S_OK;
            }
            *(ULONG*)data = access;
            *ret = sizeof(ULONG);
            return S_OK;
        }
        const bool is_set = (prop->Flags & KSPROPERTY_TYPE_SET) != 0;
        // Windows probes control support with a zero-length data buffer.
        if (!data || data_len < (is_set ? sizeof(KSCAMERA_EXTENDEDPROP_HEADER) : payload))
        {
            *ret = payload;
            return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
        }

        auto* ec = (KSCAMERA_EXTENDEDPROP_HEADER*)data;
        if (is_set)
        {
            const bool want = (ec->Flags & (KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_ALTERNATIVE_FRAME_ILLUMINATION
                                            | KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_BACKGROUND_SUBTRACTION)) != 0;
            *ret = 0; // nothing is returned on a SET; leave no garbage in BytesReturned
            // cap_ belongs to the pump thread; the flag file is the channel.
            return cyclops_vcam::write_illuminator_flag(want)
                ? S_OK : HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
        }
        // GET: echo the pin the caller asked about and report our own mode,
        // which tracks the flag file, not the physical camera's transient
        // state.
        const bool on = cyclops_vcam::read_illuminator_flag(false);
        memset(data, 0, payload);
        ec->Version = 1;
        ec->Size = payload;
        ec->Result = 0;
        // Only the mode this source actually runs: a SET of BACKGROUND_
        // SUBTRACTION is folded into "on" and would read back as ALTERNATIVE,
        // so advertising it lies to a client that checks.
        ec->Capability = KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_DISABLED
            | KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_ALTERNATIVE_FRAME_ILLUMINATION;
        ec->Flags = on ? KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_ALTERNATIVE_FRAME_ILLUMINATION
                       : KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_DISABLED;
        *ret = payload;
        return S_OK;
    }

    // Read-only surface: GET/BASICSUPPORT forward to the physical camera, but
    // a virtual-camera client must never mutate hardware shared with Windows
    // Hello, so SET and everything else is refused.
    if (prop->Flags & (KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT))
    {
        winrt::com_ptr<IKsControl> ks = physical_ks_control();
        if (ks)
            return ks->KsProperty(prop, len, data, data_len, ret);
    }
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

// Methods and events are never forwarded: the physical camera is shared with
// Windows Hello and arbitrary method invocations from a vcam client can
// reconfigure it out from under the system.
STDMETHODIMP_(NTSTATUS) cyclops_media_stream::KsMethod(PKSMETHOD, ULONG, LPVOID,
                                                      ULONG, ULONG* ret)
{
    if (!ret)
        return E_POINTER;
    *ret = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) cyclops_media_stream::KsEvent(PKSEVENT, ULONG, LPVOID,
                                                     ULONG, ULONG* ret)
{
    if (!ret)
        return E_POINTER;
    *ret = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
