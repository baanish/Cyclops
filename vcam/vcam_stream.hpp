// Copyright (c) 2026 Aanish Bhirud
// SPDX-License-Identifier: MIT

#pragma once

#include "vcam_com.hpp"

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

struct cyclops_media_source;

// One video stream. A pump thread pulls frames from the physical IR camera
// (dropping the strobe's off-phase frames) into latest_; RequestSample queues
// a request and the pump fulfills exactly one per fresh camera frame, so the
// client-visible rate never exceeds the sensor's real rate.
struct cyclops_media_stream
    : attr_forwarder<IMFAttributes>, IMFMediaStream2, IKsControl
{
    // IUnknown
    STDMETHOD(QueryInterface)(REFIID iid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IMFMediaStream2)
            *ppv = static_cast<IMFMediaStream2*>(this);
        else if (iid == IID_IMFMediaStream)
            *ppv = static_cast<IMFMediaStream*>(static_cast<IMFMediaStream2*>(this));
        else if (iid == IID_IMFMediaEventGenerator)
            *ppv = static_cast<IMFMediaEventGenerator*>(static_cast<IMFMediaStream2*>(this));
        else if (iid == IID_IMFAttributes)
            *ppv = static_cast<IMFAttributes*>(this);
        else if (iid == __uuidof(IKsControl))
            *ppv = static_cast<IKsControl*>(this);
        else
        {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&refs_); }
    STDMETHOD_(ULONG, Release)() override
    {
        ULONG r = InterlockedDecrement(&refs_);
        if (!r)
            delete this;
        return r;
    }

    // IMFMediaEventGenerator
    STDMETHOD(BeginGetEvent)(IMFAsyncCallback* cb, IUnknown* state);
    STDMETHOD(EndGetEvent)(IMFAsyncResult* res, IMFMediaEvent** evt);
    STDMETHOD(GetEvent)(DWORD flags, IMFMediaEvent** evt);
    STDMETHOD(QueueEvent)(MediaEventType met, REFGUID ext, HRESULT st, const PROPVARIANT* v);

    // IMFMediaStream
    STDMETHOD(GetMediaSource)(IMFMediaSource** src);
    STDMETHOD(GetStreamDescriptor)(IMFStreamDescriptor** desc);
    STDMETHOD(RequestSample)(IUnknown* token);

    // IMFMediaStream2
    STDMETHOD(SetStreamState)(MF_STREAM_STATE state);
    STDMETHOD(GetStreamState)(MF_STREAM_STATE* state);

    // IKsControl
    STDMETHOD_(NTSTATUS, KsProperty)(PKSPROPERTY prop, ULONG len, LPVOID data, ULONG data_len, ULONG* ret);
    STDMETHOD_(NTSTATUS, KsMethod)(PKSMETHOD m, ULONG len, LPVOID data, ULONG data_len, ULONG* ret);
    STDMETHOD_(NTSTATUS, KsEvent)(PKSEVENT evt, ULONG len, LPVOID data, ULONG data_len, ULONG* ret);

    ~cyclops_media_stream(); // calls Shutdown() so the pump and camera can't leak
    HRESULT Initialize(IMFMediaSource* parent, IMFMediaSource* physical,
                       const std::wstring& physical_symlink);
    HRESULT SetAllocator(IUnknown* alloc);
    HRESULT set_d3d_manager(IUnknown* manager);
    // DetachObject moves physical-camera teardown from the activator to the
    // stream: ShutdownObject for an FS-provided activate, Shutdown (of the
    // physical source it already holds) when act is null. False when the
    // stream is already shut down and the caller must tear down itself.
    bool adopt_physical(IMFActivate* act);
    HRESULT Start(IMFMediaType* type);
    HRESULT Stop();
    void Shutdown();
    MF_STREAM_STATE state() const { return state_.load(); }
    int fps() const { return fps_.load(); }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    void pump_loop();
    void pump_sleep(DWORD ms);
    void note_failure();          // pump thread only; queues MEError when long
    void join_pump();             // locks join_mu_ and joins/abandons
    void join_pump_locked();      // caller holds join_mu_
    void stop_tail(IMFMediaSource* phys); // join + cap close + source stop
    bool open_capture();
    HRESULT validate_type(IMFMediaType* type);
    HRESULT produce_sample(IUnknown* token);
    void fulfill_requests();
    winrt::com_ptr<IKsControl> physical_ks_control();

    server_lock lock_guard_;
    LONG refs_ = 1;

    winrt::slim_mutex lock_;
    std::atomic<MF_STREAM_STATE> state_{ MF_STREAM_STATE_STOPPED };
    GUID format_ = GUID_NULL;

    // Strong ref: a stream held past its source's destruction would otherwise
    // answer GetMediaSource with a dangling pointer. Shutdown releases it,
    // which is what breaks the source<->stream cycle.
    winrt::com_ptr<IMFMediaSource> parent_;
    winrt::com_ptr<IMFMediaSource> physical_;        // may be null; then self-open
    winrt::com_ptr<IMFStreamDescriptor> descriptor_;
    winrt::com_ptr<IMFMediaEventQueue> queue_;
    winrt::com_ptr<IMFVideoSampleAllocator> allocator_;
    winrt::com_ptr<IUnknown> d3d_manager_;           // may arrive before the allocator
    winrt::com_ptr<IMFMediaType> current_type_;
    bool allocator_ready_ = false;
    winrt::com_ptr<IMFVideoSampleAllocatorCallback> alloc_cb_;
    // Swapped-out allocators are uninitialized on the pump thread between
    // samples: UninitializeSampleAllocator racing an in-flight AllocateSample
    // corrupts the pool.
    std::vector<winrt::com_ptr<IMFVideoSampleAllocator>> retiring_; // under lock_
    bool d3d_dirty_ = false;            // under lock_
    std::atomic<bool> req_logged_{ false };
    std::atomic<bool> overflow_logged_{ false };

    ir_capture cap_;
    std::wstring symlink_;

    std::thread pump_;
    std::mutex join_mu_;            // serializes pump join + capture teardown
    std::atomic<bool> pump_stop_{ false };
    // A pump wedged in a driver call is abandoned, never terminated: a killed
    // thread can orphan MF-internal locks and deadlock Frame Server. The
    // abandoned pump still owns cap_ (it self-cleans on exit); the stream is
    // then permanently failed.
    std::atomic<bool> cap_abandoned_{ false };
    HANDLE stop_event_ = nullptr;   // wakes pump_sleep early so Stop isn't slow
    bool shutdown_ = false;         // under lock_: Start must not re-enter
    std::mutex frame_lock_;
    std::vector<uint8_t> latest_;
    bool have_frame_ = false;
    bool need_discontinuity_ = false; // under lock_: first sample after Start
    DWORD last_flag_poll_ = 0;
    DWORD fail_since_ = 0;            // pump thread: first failure of a streak
    bool error_sent_ = false;         // pump thread: MEError fired this streak

    // DetachObject handoff; teardown runs in Shutdown when set.
    winrt::com_ptr<IMFActivate> physical_activate_;
    bool owns_physical_ = false;      // under lock_

    // Frame Server does not pace RequestSample, and capture clients keep a
    // pool of requests in flight: answering all of them per frame (or any of
    // them instantly) multiplies delivery far past the sensor rate. Exactly
    // one queued request is fulfilled per physical camera frame, so delivery
    // can never exceed the sensor's real fps.
    std::deque<winrt::com_ptr<IUnknown>> pending_;

    int width_ = 640, height_ = 360; // known until the physical type says otherwise
    // Written by the pump after open, read by produce_sample; the physical
    // camera dictates frame pacing, not the advertised default.
    std::atomic<int> fps_{ 30 };
};
