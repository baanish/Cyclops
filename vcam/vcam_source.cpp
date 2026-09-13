// Copyright (c) 2026 Aanish Bhirud
// SPDX-License-Identifier: MIT

#include "vcam_source.hpp"
#include "vcam_stream.hpp"

#include <new>

cyclops_media_source::~cyclops_media_source() = default;

HRESULT cyclops_media_source::Initialize(IMFMediaSource* physical, const std::wstring& physical_symlink,
                                         IMFAttributes* activator_attrs)
{
    HRESULT hr = ensure_store();
    if (FAILED(hr))
        return hr;

    // Surface whatever Frame Server put on the activator (client PID,
    // associated sources) as source attributes.
    if (activator_attrs)
        activator_attrs->CopyAllItems(store.get());

    stream_.attach(new (std::nothrow) cyclops_media_stream());
    if (!stream_)
        return E_OUTOFMEMORY;
    hr = stream_->Initialize(this, physical, physical_symlink);
    if (FAILED(hr))
        return hr;

    // Sensor profiles tell camera-aware clients this is a plain color stream;
    // the frame-rate filter tracks whatever the physical camera reported.
    winrt::com_ptr<IMFSensorProfileCollection> profiles;
    if (SUCCEEDED(MFCreateSensorProfileCollection(profiles.put())))
    {
        winrt::com_ptr<IMFSensorProfile> profile;
        if (SUCCEEDED(MFCreateSensorProfile(KSCAMERAPROFILE_Legacy, 0, nullptr, profile.put())))
        {
            wchar_t filter[64];
            swprintf_s(filter, L"((RES==;FRT<=%d,1;SUT==))", stream_->fps());
            profile->AddProfileFilter(0, filter);
            profiles->AddProfile(profile.get());
        }
        store->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, profiles.get());
    }

    winrt::com_ptr<IMFStreamDescriptor> sd;
    hr = stream_->GetStreamDescriptor(sd.put());
    if (FAILED(hr))
        return hr;
    IMFStreamDescriptor* sds[1] = { sd.get() };
    hr = MFCreatePresentationDescriptor(1, sds, descriptor_.put());
    if (FAILED(hr))
        return hr;

    return MFCreateEventQueue(queue_.put());
}

int cyclops_media_source::stream_index_by_id(DWORD id)
{
    winrt::com_ptr<IMFStreamDescriptor> sd;
    if (FAILED(stream_->GetStreamDescriptor(sd.put())))
        return -1;
    DWORD sid = 0;
    if (FAILED(sd->GetStreamIdentifier(&sid)))
        return -1;
    return sid == id ? 0 : -1;
}

// ---- IMFMediaEventGenerator ---------------------------------------------------

STDMETHODIMP cyclops_media_source::BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state)
{
    winrt::slim_lock_guard g(lock_);
    if (!queue_)
        return MF_E_SHUTDOWN;
    return queue_->BeginGetEvent(cb, state);
}

STDMETHODIMP cyclops_media_source::EndGetEvent(IMFAsyncResult* res, IMFMediaEvent** evt)
{
    if (!evt)
        return E_POINTER;
    *evt = nullptr;
    winrt::slim_lock_guard g(lock_);
    if (!queue_)
        return MF_E_SHUTDOWN;
    return queue_->EndGetEvent(res, evt);
}

STDMETHODIMP cyclops_media_source::GetEvent(DWORD flags, IMFMediaEvent** evt)
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
    // GetEvent can block indefinitely; never hold the lock across it.
    return queue->GetEvent(flags, evt);
}

STDMETHODIMP cyclops_media_source::QueueEvent(MediaEventType met, REFGUID ext, HRESULT st, const PROPVARIANT* v)
{
    winrt::slim_lock_guard g(lock_);
    if (!queue_)
        return MF_E_SHUTDOWN;
    return queue_->QueueEventParamVar(met, ext, st, v);
}

// ---- IMFMediaSource -----------------------------------------------------------

STDMETHODIMP cyclops_media_source::CreatePresentationDescriptor(IMFPresentationDescriptor** pd)
{
    if (!pd)
        return E_POINTER;
    *pd = nullptr;
    winrt::slim_lock_guard g(lock_);
    if (!descriptor_)
        return MF_E_SHUTDOWN;
    return descriptor_->Clone(pd);
}

STDMETHODIMP cyclops_media_source::GetCharacteristics(DWORD* chars)
{
    if (!chars)
        return E_POINTER;
    *chars = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP cyclops_media_source::Pause()
{
    return MF_E_INVALID_STATE_TRANSITION;
}

STDMETHODIMP cyclops_media_source::Shutdown()
{
    winrt::com_ptr<cyclops_media_stream> stream;
    {
        winrt::slim_lock_guard g(lock_);
        if (!queue_)
            return MF_E_SHUTDOWN;

        queue_->Shutdown();
        queue_ = nullptr;
        descriptor_ = nullptr;
        stream = stream_;
    }
    // Outside lock_: stream shutdown joins the capture pump (bounded), and
    // the event methods Frame Server keeps calling must not queue behind it.
    if (stream)
        stream->Shutdown();
    ir_log(L"vcam source shutdown");
    return S_OK;
}

STDMETHODIMP cyclops_media_source::Start(IMFPresentationDescriptor* pd, const GUID* time_format,
                                         const PROPVARIANT* pos)
{
    if (!pd || !pos)
        return E_POINTER;
    if (time_format && *time_format != GUID_NULL)
        return MF_E_UNSUPPORTED_TIME_FORMAT;
    winrt::slim_lock_guard g(lock_);
    if (!queue_ || !descriptor_)
        return MF_E_SHUTDOWN;

    DWORD count = 0;
    HRESULT hr = pd->GetStreamDescriptorCount(&count);
    if (FAILED(hr) || count != 1)
        return E_INVALIDARG;

    winrt::com_ptr<IMFStreamDescriptor> sd;
    BOOL selected = FALSE;
    hr = pd->GetStreamDescriptorByIndex(0, &selected, sd.put());
    if (FAILED(hr))
        return hr;

    const bool running = stream_->state() == MF_STREAM_STATE_RUNNING;
    if (selected)
    {
        descriptor_->SelectStream(0);

        // MENewStream on first selection, MEUpdatedStream on renegotiation.
        winrt::com_ptr<IUnknown> unk;
        stream_->QueryInterface(IID_PPV_ARGS(unk.put()));
        queue_->QueueEventParamUnk(running ? MEUpdatedStream : MENewStream,
                                   GUID_NULL, S_OK, unk.get());

        winrt::com_ptr<IMFMediaTypeHandler> handler;
        winrt::com_ptr<IMFMediaType> type;
        sd->GetMediaTypeHandler(handler.put());
        if (handler)
            handler->GetCurrentMediaType(type.put());
        hr = stream_->Start(type.get());
        if (FAILED(hr))
            return hr;
    }
    else
    {
        descriptor_->DeselectStream(0);
        if (running)
            stream_->Stop();
    }

    PROPVARIANT time;
    PropVariantInit(&time);
    InitPropVariantFromInt64(MFGetSystemTime(), &time);
    hr = queue_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &time);
    PropVariantClear(&time);
    return hr;
}

STDMETHODIMP cyclops_media_source::Stop()
{
    winrt::com_ptr<cyclops_media_stream> stream;
    {
        winrt::slim_lock_guard g(lock_);
        if (!queue_ || !descriptor_)
            return MF_E_SHUTDOWN;
        if (stream_->state() == MF_STREAM_STATE_RUNNING)
            stream = stream_;
    }
    if (stream)
        stream->Stop(); // bounded pump join; keep it off lock_

    winrt::slim_lock_guard g(lock_);
    if (!queue_ || !descriptor_)
        return MF_E_SHUTDOWN; // Shutdown landed mid-Stop
    descriptor_->DeselectStream(0);

    PROPVARIANT time;
    PropVariantInit(&time);
    InitPropVariantFromInt64(MFGetSystemTime(), &time);
    HRESULT hr = queue_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, &time);
    PropVariantClear(&time);
    return hr;
}

// ---- IMFMediaSourceEx ---------------------------------------------------------

STDMETHODIMP cyclops_media_source::GetSourceAttributes(IMFAttributes** attrs)
{
    if (!attrs)
        return E_POINTER;
    *attrs = nullptr;
    winrt::slim_lock_guard g(lock_);
    if (!queue_)
        return MF_E_SHUTDOWN;
    return QueryInterface(IID_PPV_ARGS(attrs));
}

STDMETHODIMP cyclops_media_source::GetStreamAttributes(DWORD stream_id, IMFAttributes** attrs)
{
    if (!attrs)
        return E_POINTER;
    *attrs = nullptr;
    winrt::slim_lock_guard g(lock_);
    if (!queue_)
        return MF_E_SHUTDOWN;
    if (stream_id != 0)
        return E_INVALIDARG;
    return stream_->QueryInterface(IID_PPV_ARGS(attrs));
}

STDMETHODIMP cyclops_media_source::SetD3DManager(IUnknown* manager)
{
    // CPU path today, but the client-provided allocator may be DX-backed.
    winrt::slim_lock_guard g(lock_);
    if (!stream_)
        return MF_E_SHUTDOWN;
    return stream_->set_d3d_manager(manager);
}

// ---- IMFGetService ------------------------------------------------------------

STDMETHODIMP cyclops_media_source::GetService(REFGUID, REFIID, LPVOID* ppv)
{
    if (ppv)
        *ppv = nullptr;
    return MF_E_UNSUPPORTED_SERVICE;
}

// ---- IMFSampleAllocatorControl ------------------------------------------------

STDMETHODIMP cyclops_media_source::SetDefaultAllocator(DWORD stream_id, IUnknown* alloc)
{
    winrt::slim_lock_guard g(lock_);
    if (stream_index_by_id(stream_id) < 0)
        return E_INVALIDARG;
    return stream_->SetAllocator(alloc);
}

STDMETHODIMP cyclops_media_source::GetAllocatorUsage(DWORD stream_id, DWORD* input_id,
                                                     MFSampleAllocatorUsage* usage)
{
    if (!input_id || !usage)
        return E_POINTER;
    if (stream_index_by_id(stream_id) < 0)
        return E_INVALIDARG;
    *input_id = stream_id;
    // Provided-allocator mode is required: samples must live in shared buffers
    // Frame Server can hand to the consuming process.
    *usage = MFSampleAllocatorUsage_UsesProvidedAllocator;
    return S_OK;
}

// ---- IKsControl ---------------------------------------------------------------

STDMETHODIMP_(NTSTATUS) cyclops_media_source::KsProperty(PKSPROPERTY prop, ULONG len, LPVOID data,
                                                        ULONG data_len, ULONG* ret)
{
    // No running-state gate: the stream routes FACEAUTH through the flag file,
    // which applies even before frames flow.
    if (stream_)
        return stream_->KsProperty(prop, len, data, data_len, ret);
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) cyclops_media_source::KsMethod(PKSMETHOD m, ULONG len, LPVOID data,
                                                      ULONG data_len, ULONG* ret)
{
    if (stream_)
        return stream_->KsMethod(m, len, data, data_len, ret);
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) cyclops_media_source::KsEvent(PKSEVENT evt, ULONG len, LPVOID data,
                                                     ULONG data_len, ULONG* ret)
{
    if (stream_)
        return stream_->KsEvent(evt, len, data, data_len, ret);
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
