// Copyright (c) 2026 Aanish Bhirud
// SPDX-License-Identifier: MIT

#pragma once

// Windows Hello IR cameras live in KSCATEGORY_SENSOR_CAMERA, which the
// DirectShow capture pipeline does not support. This opens them through Media
// Foundation instead, and drives the IR emitter via the pin-scoped
// FACEAUTH_MODE extended camera control. Ported from the video-mfsensor
// OpenTrack backend.

#include <cstdint>
#include <string>
#include <vector>

struct IMFMediaSource;
struct IMFSourceReader;
struct IMFExtendedCameraController;

struct ir_camera_device
{
    std::wstring name;    // friendly name
    std::wstring symlink; // MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK
};

// KSCATEGORY_SENSOR_CAMERA: IR/depth sensor cameras, invisible to DirectShow.
std::vector<ir_camera_device> enum_sensor_cameras();

// KSCATEGORY_VIDEO_CAMERA: ordinary webcams, including virtual ones.
std::vector<ir_camera_device> enum_video_cameras();

// Frame size and rate of the stream ir_capture would read from this source
// (the Infrared-tagged one, else the first renderable). The vcam advertises
// its media type from this so it matches what capture actually delivers.
// Outputs are only written when the source reports them.
bool probe_source_geometry(IMFMediaSource* source, int& width, int& height, int& fps);

class ir_capture
{
public:
    ir_capture() = default;
    ~ir_capture();
    ir_capture(const ir_capture&) = delete;
    ir_capture& operator=(const ir_capture&) = delete;

    bool open(const std::wstring& symlink);      // create + wrap our own device source
    bool open(IMFMediaSource* source);           // adopt an already-activated source
    void close();
    bool is_open() const { return reader_ != nullptr; }

    // Toggle the IR emitter at any time. Returns false when the camera has no
    // illuminating FACEAUTH mode (in which case the feed is ambient-only IR).
    bool set_illumination(bool on);
    bool illumination_supported() const { return illum_capable_; }
    bool illumination_on() const;
    bool strobing() const { return strobing_; }

    // Fill out_ with one displayable L8 frame. When strobing, off-phase
    // (unlit) frames are dropped. Returns false if nothing arrived in time.
    // A ReadSample wedged in the driver can outlive the bound; callers bound
    // that case with their own watchdog.
    bool read_frame(int timeout_ms);

    const std::vector<uint8_t>& pixels() const { return buf_; }
    int width() const { return width_; }
    int height() const { return height_; }
    int fps() const { return fps_; }

private:
    // skipped: ReadSample returned (tick, end of stream, or a strobe off-phase
    // frame) but produced no frame; only failed warrants backing off.
    enum class read_result { frame, skipped, failed };

    bool start_with_source();
    bool read_current_type();    // refresh width_/height_/fps_; false = unusable
    read_result read_sample();   // one ReadSample into buf_; fills last_mean_
    bool probe_illumination();   // find a stream index + mode that can illuminate
    bool apply_illumination(unsigned long long mode); // commit a FACEAUTH mode
    bool sample_is_lit() const;

    IMFMediaSource* source_ = nullptr;
    bool source_owned_ = false;
    IMFSourceReader* reader_ = nullptr;
    IMFExtendedCameraController* controller_ = nullptr;

    unsigned long illum_stream_ = 0;
    unsigned long long illum_mode_ = 0;  // last committed FACEAUTH mode
    unsigned long long illum_on_mode_ = 0; // mode to use for set_illumination(true)
    bool illum_capable_ = false;
    bool strobing_ = false;              // ALTERNATIVE_FRAME_ILLUMINATION active
    bool mf_started_ = false;

    std::vector<uint8_t> buf_;
    int width_ = 0, height_ = 0, fps_ = 0;
    int stride_ = 0;         // MF_MT_DEFAULT_STRIDE for buffers without a 2D interface
    bool type_ok_ = true;    // false after a renegotiation to a subtype we can't decode

    int stream_index_ = -1;  // -1 = MF_SOURCE_READER_FIRST_VIDEO_STREAM
    int lit_metadata_ = -1;  // MF_CAPTURE_METADATA_FRAME_ILLUMINATION, -1 when absent
    double last_mean_ = 0;   // fallback phase detector when metadata is absent
    double peak_mean_ = 0;
};

void ir_log(const wchar_t* fmt, ...);
