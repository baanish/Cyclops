// Copyright (c) 2026 Aanish Bhirud
// SPDX-License-Identifier: MIT

#pragma once

#include "ir_capture.hpp"

#include <QImage>
#include <QThread>

#include <objbase.h>

#include <atomic>

// Owns an ir_capture on a worker thread; frames arrive as frameReady signals.
// set_illumination requests are applied on the worker thread between reads.
class capture_worker : public QThread
{
    Q_OBJECT

public:
    void open(const std::wstring& symlink)
    {
        if (isRunning()) // start() on a live QThread is a no-op; don't pretend
            return;
        symlink_ = symlink;
        illum_req_.store(-1); // don't carry an illuminator request across opens
        cancel_.store(false); // stop() before the first start() leaves it set
        start();
    }
    void request_illumination(bool on) { illum_req_.store(on ? 1 : 0); }
    // Signal only, no wait: a vcam open can sit inside Frame Server for
    // seconds, and stop() runs on the UI thread. Callers that need the
    // thread gone defer off the finished signal instead of blocking.
    void stop() { cancel_.store(true); }
    void shutdown()
    {
        cancel_.store(true);
        wait(10000); // bounded: a wedged close must not strand the process
    }

signals:
    void opened(int width, int height, bool illum_supported, bool strobing);
    void open_failed(const QString& why);
    void frame_ready(const QImage& img);
    void illumination_changed(bool on);

protected:
    void run() override
    {
        // MF flat APIs tolerate an uninitialized apartment, but the GUI
        // thread always had one via Qt's OLE init; match that here.
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ir_capture cap;
        if (!cap.open(symlink_))
        {
            emit open_failed(QStringLiteral("could not open the camera"));
            return;
        }
        emit opened(cap.width(), cap.height(), cap.illumination_supported(), cap.strobing());

        int reads = 0;
        while (!cancel_.load())
        {
            const int req = illum_req_.exchange(-1);
            if (req >= 0)
            {
                if (cap.set_illumination(req == 1))
                    emit illumination_changed(cap.illumination_on());
            }

            if (cap.read_frame(1000))
            {
                if (++reads == 1)
                    ir_log(L"gui: first frame delivered");
                const auto& px = cap.pixels();
                QImage img(px.data(), cap.width(), cap.height(), cap.width(), QImage::Format_Grayscale8);
                emit frame_ready(img.copy());
            }
        }

        cap.close(); // restores FACEAUTH_MODE_DISABLED
    }

private:
    std::wstring symlink_;
    std::atomic<bool> cancel_{ false };
    std::atomic<int> illum_req_{ -1 };
};
