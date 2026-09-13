// Copyright (c) 2026 Aanish Bhirud
// SPDX-License-Identifier: MIT

#include "main_window.hpp"
#include "toggle_switch.hpp"
#include "vcam_manager.hpp"
#include "vcam_ids.hpp"

#include <QComboBox>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

using cyclops_vcam::friendly_name;

// Paints the live frame clipped to the card's rounded corners; also owns the
// empty-state icon and centered status text.
class preview_view : public QWidget
{
public:
    using QWidget::QWidget;

    void set_frame(const QImage& f) { frame_ = f; text_.clear(); empty_ = QPixmap(); update(); }
    void set_empty(const QPixmap& px) { empty_ = px; frame_ = QImage(); text_.clear(); update(); }
    void set_text(const QString& t) { text_ = t; frame_ = QImage(); empty_ = QPixmap(); update(); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath path;
        path.addRoundedRect(r, 8, 8);
        p.fillPath(path, QColor(10, 10, 10));
        p.setClipPath(path);
        if (!frame_.isNull())
        {
            // Same-size draw is the common case; scaled() would still alloc
            // and walk the whole image every frame.
            const QImage s = frame_.size() == size()
                ? frame_
                : frame_.scaled(size(), Qt::KeepAspectRatio, Qt::FastTransformation);
            p.drawImage(QPoint((width() - s.width()) / 2, (height() - s.height()) / 2), s);
        }
        else if (!empty_.isNull())
        {
            const QSizeF s = empty_.size() / empty_.devicePixelRatio();
            p.drawPixmap(QPointF((width() - s.width()) / 2, (height() - s.height()) / 2), empty_);
        }
        else if (!text_.isEmpty())
        {
            p.setPen(QColor(142, 146, 154));
            p.drawText(rect(), Qt::AlignCenter, text_);
        }
        p.setClipping(false);
        p.setPen(QColor(56, 56, 56));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }

private:
    QImage frame_;
    QPixmap empty_;
    QString text_;
};

namespace {

// The user's system accent color (Settings > Personalization > Colors);
// toggles and the live badge key off it like native Win11 controls.
QColor system_accent()
{
    QSettings dwm(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\DWM"),
                  QSettings::NativeFormat);
    const QVariant v = dwm.value(QStringLiteral("AccentColor"));
    const quint32 abgr = v.isValid() ? v.toUInt() : 0xffffffff;
    if (abgr == 0xffffffff) // "automatic" accent, nothing picked
        return QColor(0x4c, 0xc2, 0xff); // stock Windows 11 dark accent
    return QColor(abgr & 0xff, (abgr >> 8) & 0xff, (abgr >> 16) & 0xff);
}

const char* stylesheet = R"(
QMainWindow, QWidget { background: #202020; color: #f0f0f0;
                       font-family: "Segoe UI Variable Text", "Segoe UI"; font-size: 13px; }
QLabel { background: transparent; }

#title { font-size: 19px; font-weight: 600; color: #ffffff; }
#subtitle { color: #9a9a9a; font-size: 12px; }

#live { background: rgba(0, 0, 0, 190); color: #d08373; border: 1px solid #4a4a4a;
        border-radius: 4px; padding: 2px 7px; font-size: 10px; font-weight: 700; }

#controls { background: #2b2b2b; border: 1px solid #383838; border-radius: 8px; }
#section { color: #f0f0f0; }
#hint { color: #9a9a9a; font-size: 11px; }
#sep { background: #383838; }

#stats { color: #9a9a9a; font-size: 11px; font-family: "Cascadia Mono", "Consolas"; }
#disclaimer { color: #b39b6e; font-size: 11px; }

QComboBox { background: #2d2d2d; border: 1px solid #444444; border-radius: 6px;
            padding: 5px 10px; min-width: 220px; }
QComboBox:hover { background: #323232; border-color: #4e4e4e; }
QComboBox::drop-down { border: none; width: 24px; }
QComboBox QAbstractItemView { background: #2d2d2d; border: 1px solid #444444;
                              color: #f0f0f0; selection-background-color: #454545; }
)";

} // ns

main_window::main_window()
{
    setWindowTitle(QStringLiteral("Cyclops"));
    setStyleSheet(stylesheet);
    resize(760, 620);

    auto* root = new QWidget(this);
    auto* lay = new QVBoxLayout(root);
    lay->setContentsMargins(18, 16, 18, 14);
    lay->setSpacing(12);

    const qreal dpr = devicePixelRatioF();
    empty_icon_ = QPixmap(QStringLiteral(":/assets/icon_eye.png"))
                      .scaled(88 * dpr, 88 * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    empty_icon_.setDevicePixelRatio(dpr);

    auto* top = new QHBoxLayout();
    top->setSpacing(10);
    auto* chip = new QLabel(root);
    chip->setObjectName(QStringLiteral("icon_chip"));
    QPixmap chip_px = QPixmap(QStringLiteral(":/assets/icon_eye.png"))
                          .scaled(26 * dpr, 26 * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    chip_px.setDevicePixelRatio(dpr);
    chip->setPixmap(chip_px);
    top->addWidget(chip);
    auto* name_col = new QVBoxLayout();
    name_col->setSpacing(0);
    auto* title = new QLabel(QStringLiteral("Cyclops"), root);
    title->setObjectName(QStringLiteral("title"));
    name_col->addWidget(title);
    auto* subtitle = new QLabel(QStringLiteral("Windows Hello IR passthrough"), root);
    subtitle->setObjectName(QStringLiteral("subtitle"));
    name_col->addWidget(subtitle);
    top->addLayout(name_col);
    top->addStretch(1);
    camera_box_ = new QComboBox(root);
    camera_box_->setVisible(false); // sensors populate lazily, off the UI thread
    top->addWidget(camera_box_);
    lay->addLayout(top);

    preview_ = new preview_view(root);
    preview_->setMinimumSize(640, 360);
    preview_->set_empty(empty_icon_);
    lay->addWidget(preview_, 1);

    live_ = new QLabel(QStringLiteral("LIVE"), preview_);
    live_->setObjectName(QStringLiteral("live"));
    live_->move(12, 10);
    live_->adjustSize();
    live_->hide();

    const QColor accent = system_accent();

    auto* controls = new QFrame(root);
    controls->setObjectName(QStringLiteral("controls"));
    auto* cv = new QVBoxLayout(controls);
    cv->setContentsMargins(16, 4, 16, 4);
    cv->setSpacing(0);

    auto row = [&](const QString& label_text, toggle_switch*& sw, QLabel*& state, const QString& hint) {
        if (cv->count() > 0)
        {
            auto* sep = new QFrame(controls);
            sep->setObjectName(QStringLiteral("sep"));
            sep->setFixedHeight(1);
            cv->addWidget(sep);
        }
        auto* h = new QHBoxLayout();
        h->setContentsMargins(0, 10, 0, 10);
        auto* texts = new QVBoxLayout();
        texts->setSpacing(1);
        auto* name = new QLabel(label_text, controls);
        name->setObjectName(QStringLiteral("section"));
        texts->addWidget(name);
        state = new QLabel(hint, controls);
        state->setObjectName(QStringLiteral("hint"));
        texts->addWidget(state);
        h->addLayout(texts);
        h->addStretch(1);
        sw = new toggle_switch(controls);
        sw->set_accent(accent);
        h->addWidget(sw);
        cv->addLayout(h);
    };

    row(QStringLiteral("Preview"), preview_switch_, preview_state_, QStringLiteral("on"));
    preview_switch_->setChecked(true);
    row(QStringLiteral("IR illuminator"), illum_switch_, illum_state_, QStringLiteral("off"));
    row(QStringLiteral("Windows webcam"), webcam_switch_, webcam_state_, QStringLiteral("not installed"));
    lay->addWidget(controls);

    stats_ = new QLabel(root);
    stats_->setObjectName(QStringLiteral("stats"));
    lay->addWidget(stats_);

    auto* disclaimer = new QLabel(QStringLiteral(
        "IR emitters are not designed for continuous use and can overheat the "
        "camera. Turn the illuminator off when you are not using it."), root);
    disclaimer->setObjectName(QStringLiteral("disclaimer"));
    lay->addWidget(disclaimer);

    setCentralWidget(root);

    connect(&worker_, &capture_worker::opened, this, [this](int w, int h, bool illum_ok, bool strobing) {
        ir_log(L"gui: capture opened %dx%d illum=%d strobe=%d", w, h, illum_ok ? 1 : 0,
               strobing ? 1 : 0);
        ignore_signals_ = true;
        // The vcam has no extended camera control of its own; in webcam mode
        // the switch stays live and drives the flag file the pump polls.
        illum_switch_->setEnabled(illum_ok || webcam_active_);
        illum_state_->setText(illum_ok ? QStringLiteral("off") : QStringLiteral("not supported"));
        ignore_signals_ = false;
        stats_->setText(QStringLiteral("%1x%2 · %3").arg(w).arg(h)
                            .arg(strobing ? QStringLiteral("strobe mode")
                                          : QStringLiteral("continuous")));
        frame_count_ = 0;
        fps_window_start_ = 0;
    });
    connect(&worker_, &capture_worker::open_failed, this, [this](const QString& why) {
        ir_log(L"gui: open failed: %ls", why.toStdWString().c_str());
        webcam_active_ = false; // not capturing via it; next refresh retries
        live_->hide();
        preview_->set_text(why);
        stats_->setText(QString());
    });
    connect(&worker_, &capture_worker::frame_ready, this, [this](const QImage& img) {
        preview_->set_frame(img);
        if (!live_->isVisible())
            live_->show();
        last_frame_at_ = QDateTime::currentMSecsSinceEpoch();
        frame_count_++;
        if (!fps_window_start_)
            fps_window_start_ = QDateTime::currentMSecsSinceEpoch();
        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - fps_window_start_;
        if (elapsed >= 1000)
        {
            stats_->setText(QStringLiteral("%1x%2 · %3 fps · %4")
                                .arg(img.width()).arg(img.height())
                                .arg(frame_count_ * 1000.0 / elapsed, 0, 'f', 1)
                                .arg(webcam_active_ ? QStringLiteral("via virtual camera")
                                                    : QStringLiteral("direct")));
            frame_count_ = 0;
            fps_window_start_ = QDateTime::currentMSecsSinceEpoch();
        }
    });
    connect(&worker_, &capture_worker::illumination_changed, this, [this](bool on) {
        illum_state_->setText(on ? QStringLiteral("on") : QStringLiteral("off"));
    });
    // Permanent connection: a per-defer SingleShotConnection can be connected
    // after finished() already emitted, which would drop the restart.
    connect(&worker_, &QThread::finished, this, [this] {
        if (deferred_restart_)
        {
            deferred_restart_ = false;
            schedule_restart();
        }
    });

    connect(preview_switch_, &QAbstractButton::toggled, this, [this](bool on) {
        preview_state_->setText(on ? QStringLiteral("on") : QStringLiteral("off"));
        if (!ignore_signals_)
            schedule_restart();
    });
    connect(illum_switch_, &QAbstractButton::toggled, this, [this](bool on) {
        if (!ignore_signals_)
            apply_illuminator(on);
    });
    connect(webcam_switch_, &QAbstractButton::toggled, this, [this](bool on) {
        if (ignore_signals_)
            return;
        ignore_signals_ = true;
        webcam_switch_->setChecked(!on); // revert until the operation proves out
        ignore_signals_ = false;
        webcam_switch_->setEnabled(false);
        webcam_state_->setText(on ? QStringLiteral("enabling")
                                  : QStringLiteral("disabling"));

        // MF virtual-camera calls round-trip into Frame Server and can take
        // seconds; never run them on the UI thread.
        auto* res = new std::pair<bool, QString>{};
        auto* t = QThread::create([on, res] {
            if (on)
            {
                res->first = cyclops_vcam::is_registered()
                    ? cyclops_vcam::enable(&res->second)
                    : cyclops_vcam::elevate_op(cyclops_vcam::op_install);
            }
            else
            {
                res->first = cyclops_vcam::disable(&res->second)
                    || cyclops_vcam::elevate_op(cyclops_vcam::op_disable);
            }
        });
        connect(t, &QThread::finished, this, [this, t, res] {
            webcam_switch_->setEnabled(true);
            if (!res->first && !res->second.isEmpty())
                QMessageBox::warning(this, QStringLiteral("Cyclops"), res->second);
            delete res;
            t->deleteLater();
            // Device enumeration lags the operation; re-check shortly after.
            QTimer::singleShot(1500, this, [this] { refresh_webcam_status(); });
            refresh_webcam_status();
        });
        t->start();
    });

    connect(camera_box_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!ignore_signals_)
            schedule_restart();
    });

    refresh_webcam_status();
    schedule_restart();

    // A wedged Frame Server or driver leaves the last frame on screen
    // forever; surface the stall instead of a silent freeze.
    auto* stall_timer = new QTimer(this);
    stall_timer->setInterval(2000);
    connect(stall_timer, &QTimer::timeout, this, [this] {
        if (worker_.isRunning() && last_frame_at_
            && QDateTime::currentMSecsSinceEpoch() - last_frame_at_ > 5000)
            stats_->setText(QStringLiteral("camera stalled - flip Windows webcam off/on to restart it"));
    });
    stall_timer->start();
}

// restart_capture calls arrive in bursts (ctor refresh + start, toggle +
// status refresh). stop() sets the worker's cancel flag while a previous
// open() can still be blocked inside Frame Server; coalescing through a
// single-shot avoids a cancel landing mid-open and stranding the preview.
void main_window::schedule_restart()
{
    if (restart_scheduled_)
        return;
    restart_scheduled_ = true;
    QTimer::singleShot(50, this, [this] {
        restart_scheduled_ = false;
        restart_capture();
    });
}

void main_window::restart_capture()
{
    worker_.stop();
    if (worker_.isRunning())
    {
        // The previous open is still blocked (Frame Server churn); start()
        // on a live QThread is a no-op, so retry once it finishes instead.
        ir_log(L"gui: restart deferred, worker still running");
        deferred_restart_ = true;
        return;
    }

    if (!preview_switch_->isChecked())
    {
        // No capture at all: in vcam mode this drops our session so other
        // apps get the device; the illuminator flag stays armed for the pump.
        ++restart_seq_; // an in-flight resolution must not reopen the camera
        webcam_active_ = false;
        live_->hide();
        preview_->set_empty(empty_icon_);
        stats_->setText(QStringLiteral("preview off"));
        ignore_signals_ = true;
        illum_switch_->setEnabled(webcam_switch_->isChecked());
        ignore_signals_ = false;
        return;
    }

    // Device enumeration is an RPC into Frame Server and can block for
    // seconds when the service is churning; resolve off the UI thread.
    struct cam_resolution {
        std::vector<ir_camera_device> sensors;
        std::wstring vcam_symlink;
    };
    const quint64 seq = ++restart_seq_;
    if (resolution_pending_)
        return; // the in-flight resolver is now stale; its finish reschedules
    resolution_pending_ = true;
    const bool want_vcam = webcam_switch_->isChecked();
    const bool need_sensors = sensor_cams_.empty();
    auto* res = new cam_resolution;
    auto* t = QThread::create([want_vcam, need_sensors, res] {
        if (need_sensors)
            res->sensors = enum_sensor_cameras();
        if (!want_vcam)
            return;
        for (const auto& d : enum_video_cameras())
        {
            ir_log(L"gui: video camera \"%ls\"", d.name.c_str());
            if (d.name.rfind(friendly_name, 0) == 0)
            {
                res->vcam_symlink = d.symlink;
                break;
            }
        }
    });
    connect(t, &QThread::finished, this, [this, seq, t, res] {
        std::wstring symlink = res->vcam_symlink;
        const bool via_vcam = !symlink.empty();
        const bool have_sensors = !res->sensors.empty();
        if (have_sensors && sensor_cams_.empty())
        {
            sensor_cams_ = std::move(res->sensors);
            ir_log(L"gui: %zu sensor camera(s)", sensor_cams_.size());
            ignore_signals_ = true; // initial fill must not schedule a restart
            for (const auto& d : sensor_cams_)
                camera_box_->addItem(QString::fromStdWString(d.name));
            camera_box_->setVisible(sensor_cams_.size() > 1);
            ignore_signals_ = false;
        }
        delete res;
        t->deleteLater();
        resolution_pending_ = false;

        if (seq != restart_seq_ || worker_.isRunning() || !preview_switch_->isChecked())
        {
            schedule_restart(); // a newer restart superseded this resolution
            return;
        }
        webcam_active_ = via_vcam;

        if (symlink.empty() && camera_box_->currentIndex() >= 0
            && camera_box_->currentIndex() < (int)sensor_cams_.size())
            symlink = sensor_cams_[camera_box_->currentIndex()].symlink;

        if (symlink.empty())
        {
            live_->hide();
            preview_->set_empty(empty_icon_);
            stats_->setText(QStringLiteral("no camera found"));
            return;
        }

        ignore_signals_ = true;
        illum_switch_->setEnabled(true);
        illum_switch_->setChecked(webcam_active_ ? cyclops_vcam::read_illuminator_flag(true) : false);
        illum_state_->setText(illum_switch_->isChecked() ? QStringLiteral("on") : QStringLiteral("off"));
        ignore_signals_ = false;

        ir_log(L"gui: opening %s", via_vcam ? L"vcam" : L"sensor");
        last_frame_at_ = QDateTime::currentMSecsSinceEpoch();
        worker_.open(symlink);
    });
    t->start();
}

void main_window::apply_illuminator(bool on)
{
    if (webcam_switch_->isChecked())
    {
        // The media source inside Frame Server polls the flag file and applies
        // FACEAUTH to the physical camera itself; with the preview off it is
        // simply armed until a client opens the virtual camera.
        cyclops_vcam::write_illuminator_flag(on);
        illum_state_->setText(on ? QStringLiteral("on") : QStringLiteral("off"));
    }
    else
    {
        worker_.request_illumination(on);
    }
}

void main_window::refresh_webcam_status()
{
    if (status_pending_)
    {
        status_refresh_wanted_ = true; // enumeration is an FS RPC; never stack queries
        return;
    }
    status_pending_ = true;

    auto* res = new std::pair<bool, bool>{}; // registered, enumerated
    auto* t = QThread::create([res] {
        res->first = cyclops_vcam::is_registered();
        res->second = cyclops_vcam::is_enumerated();
    });
    connect(t, &QThread::finished, this, [this, t, res] {
        status_pending_ = false;
        const bool registered = res->first;
        const bool enumerated = res->second;
        delete res;
        t->deleteLater();

        ignore_signals_ = true;
        webcam_switch_->setChecked(enumerated);
        webcam_state_->setText(!registered ? QStringLiteral("not installed")
                             : enumerated ? QStringLiteral("on · visible as \"%1\"")
                                                  .arg(QString::fromWCharArray(cyclops_vcam::friendly_name))
                                          : QStringLiteral("installed, off"));
        ignore_signals_ = false;

        if (status_refresh_wanted_)
        {
            // A second refresh was requested while this one was in flight;
            // run it now rather than dropping it.
            status_refresh_wanted_ = false;
            refresh_webcam_status();
            return;
        }
        if (enumerated != webcam_active_)
            schedule_restart();
    });
    t->start();
}

void main_window::closeEvent(QCloseEvent* e)
{
    worker_.shutdown(); // restores FACEAUTH_MODE_DISABLED on the way out
    if (worker_.isRunning())
    {
        // A read can wedge inside Frame Server and never return; a QThread
        // destroyed while running crashes the process, so terminate it and
        // let the OS reap the MF handles (which also resets the emitter).
        worker_.terminate();
        worker_.wait(3000);
    }
    QMainWindow::closeEvent(e);
}
