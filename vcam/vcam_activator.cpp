// Copyright (c) 2026 Aanish Bhirud
// SPDX-License-Identifier: MIT

#include "vcam_activator.hpp"
#include "vcam_source.hpp"
#include "vcam_stream.hpp"

#include <new>

cyclops_activator::~cyclops_activator()
{
    // Frame Server releases the activator right after ActivateObject and
    // keeps only the media source, so this is not a teardown signal: the
    // live source takes over the physical session exactly as DetachObject
    // hands it over (shutting the camera down here is what made every
    // associated-source open fail and fall back to a direct open). Only
    // when no live source can own it is the camera closed here.
    if (source_ && source_->adopt_physical(physical_activate_.get()))
        return;
    shutdown_physical();
}

// Sole owner of physical-camera teardown: ShutdownObject for an associated
// (frame-server-provided) activation, Shutdown for a source we opened
// ourselves. The stream never does more than Stop() on these.
void cyclops_activator::shutdown_physical()
{
    if (physical_self_opened_ && physical_)
        physical_->Shutdown();
    if (physical_activate_)
    {
        physical_activate_->ShutdownObject();
        physical_activate_ = nullptr;
    }
    physical_ = nullptr;
    physical_self_opened_ = false;
}

HRESULT cyclops_activator::Initialize()
{
    HRESULT hr = ensure_store();
    if (FAILED(hr))
        return hr;
    store->SetUINT32(MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES_, 1);
    ir_log(L"vcam activator created");
    return S_OK;
}

STDMETHODIMP cyclops_activator::ActivateObject(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;
    std::lock_guard<std::mutex> g(mu_);
    HRESULT hr = ensure_store();
    if (FAILED(hr))
        return hr;

    UINT32 pid = 0;
    if (SUCCEEDED(store->GetUINT32(MF_FRAMESERVER_CLIENTCONTEXT_CLIENTPID_, &pid)) && pid)
        ir_log(L"vcam activating for client pid %lu", pid);

    // IMFActivate contract: repeat calls hand back the same object until
    // ShutdownObject or DetachObject. A source the client already shut down
    // itself can't be handed out again; its camera resources are torn down
    // (source first, so nothing pumps through a camera being shut down) and
    // a fresh pair is built.
    if (source_ && !source_->is_shutdown())
        return source_->QueryInterface(riid, ppv);
    if (source_)
    {
        source_->Shutdown();
        source_ = nullptr;
    }
    shutdown_physical();
    physical_symlink_.clear();

    // 1. Frame-Server-provided associated camera (declared with
    //    AddDeviceSourceInfo at creation time).
    winrt::com_ptr<IMFCollection> coll;
    if (SUCCEEDED(store->GetUnknown(MF_VIRTUALCAMERA_ASSOCIATED_CAMERA_SOURCES_,
                                    IID_PPV_ARGS(coll.put()))))
    {
        DWORD n = 0;
        if (FAILED(coll->GetElementCount(&n)))
            n = 0;
        ir_log(L"vcam: %lu associated camera source(s)", n);
        if (n >= 1)
        {
            winrt::com_ptr<IUnknown> unk;
            if (SUCCEEDED(coll->GetElement(0, unk.put()))
                && SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(physical_activate_.put()))))
            {
                if (SUCCEEDED(physical_activate_->ActivateObject(IID_PPV_ARGS(physical_.put()))))
                    ir_log(L"vcam: using associated camera source");
                else
                    physical_activate_ = nullptr; // failed activate owns nothing
            }
        }
    }

    // 2. The symlink persisted at IMFVirtualCamera creation time. Always
    //    read: the stream opens the sensor directly by it even when an
    //    associated source exists, because the Frame Server proxy source
    //    exposes no illuminating FACEAUTH mode (measured on the N930W) and
    //    the emitter is the point of the device.
    {
        WCHAR* link = nullptr;
        UINT32 cch = 0;
        if (SUCCEEDED(store->GetAllocatedString(cyclops_vcam::symlink_attr, &link, &cch)) && link)
        {
            physical_symlink_ = link;
            CoTaskMemFree(link);
        }
    }
    if (!physical_ && !physical_symlink_.empty())
    {
        winrt::com_ptr<IMFAttributes> attrs;
        if (SUCCEEDED(MFCreateAttributes(attrs.put(), 2)))
        {
            attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                           MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
            attrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                             physical_symlink_.c_str());
            if (SUCCEEDED(MFCreateDeviceSource(attrs.get(), physical_.put())))
            {
                physical_self_opened_ = true;
                ir_log(L"vcam: opened stored symlink");
            }
        }
    }

    // 3. No source yet: leave physical_ null; the stream self-enumerates
    //    sensor cameras when it starts.

    auto* src = new (std::nothrow) cyclops_media_source();
    if (!src)
        return E_OUTOFMEMORY;
    source_.attach(src);
    // Pass our store so the source can surface Frame Server's attributes.
    hr = source_->Initialize(physical_.get(), physical_symlink_, this);
    if (FAILED(hr))
    {
        source_ = nullptr;
        shutdown_physical(); // a failed activation owns nothing it resolved
        return hr;
    }
    return source_->QueryInterface(riid, ppv);
}

STDMETHODIMP cyclops_activator::ShutdownObject()
{
    std::lock_guard<std::mutex> g(mu_);
    if (source_)
    {
        source_->Shutdown();
        source_ = nullptr;
    }
    shutdown_physical();
    ir_log(L"vcam activator shutdown");
    return S_OK;
}

STDMETHODIMP cyclops_activator::DetachObject()
{
    std::lock_guard<std::mutex> g(mu_);
    // Ownership of everything transfers to the caller: the source may still
    // be streaming, so releasing our refs must not tear the camera out from
    // under it. Physical-session teardown moves onto the stream, which runs
    // ShutdownObject/Shutdown in its own Shutdown. A source that already shut
    // down can't take it, so the camera session is closed here instead.
    if (source_ && source_->adopt_physical(physical_activate_.get()))
    {
        physical_ = nullptr;
        physical_activate_ = nullptr;
        physical_self_opened_ = false;
    }
    else
        shutdown_physical();
    source_ = nullptr;
    return S_OK;
}
