// Copyright (c) 2026 Aanish Bhirud
// SPDX-License-Identifier: MIT

#pragma once

#include "capture_worker.hpp"

#include <QMainWindow>
#include <QPixmap>
#include <vector>

class QComboBox;
class QLabel;
class preview_view;
class toggle_switch;

class main_window : public QMainWindow
{
    Q_OBJECT

public:
    main_window();

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void schedule_restart();
    void restart_capture();
    void refresh_webcam_status();
    void apply_illuminator(bool on);

    capture_worker worker_;

    preview_view* preview_ = nullptr;
    QLabel* live_ = nullptr;
    QPixmap empty_icon_;
    QComboBox* camera_box_ = nullptr;
    toggle_switch* preview_switch_ = nullptr;
    toggle_switch* illum_switch_ = nullptr;
    toggle_switch* webcam_switch_ = nullptr;
    QLabel* preview_state_ = nullptr;
    QLabel* illum_state_ = nullptr;
    QLabel* webcam_state_ = nullptr;
    QLabel* stats_ = nullptr;

    std::vector<ir_camera_device> sensor_cams_;
    bool webcam_active_ = false; // previewing through the virtual camera
    bool webcam_desired_ = false; // user toggled it on; outlives enum flicker
    bool ignore_signals_ = false;
    bool restart_scheduled_ = false;
    bool deferred_restart_ = false;
    bool resolution_pending_ = false;
    bool status_pending_ = false;
    bool status_refresh_wanted_ = false;
    qint64 resolution_since_ = 0; // wedged resolver expiry
    qint64 status_since_ = 0;     // wedged status-query expiry
    int heal_failures_ = 0;       // consecutive failed self-heals, capped
    bool heal_pending_ = false;   // one self-heal enable() in flight at a time
    quint64 restart_seq_ = 0; // drops stale async device resolutions
    quint64 webcam_op_seq_ = 0;   // bumps per user toggle; stale status results must not re-arm intent
    bool worker_opened_ = false;  // open finished; the stall watchdog runs on the short budget

    int frame_count_ = 0;
    qint64 fps_window_start_ = 0;
    qint64 last_frame_at_ = 0;
};
