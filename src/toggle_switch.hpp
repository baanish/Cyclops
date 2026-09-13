// Copyright (c) 2026 Aanish Bhirud
// SPDX-License-Identifier: MIT

#pragma once

#include <QAbstractButton>
#include <QPainter>
#include <QPropertyAnimation>

// Pill switch in the style of a native Windows 11 toggle: outlined track when
// off, system-accent fill with a dark knob when on.
class toggle_switch : public QAbstractButton
{
    Q_OBJECT
    Q_PROPERTY(qreal pos READ pos WRITE set_pos)

public:
    explicit toggle_switch(QWidget* parent = nullptr) : QAbstractButton(parent)
    {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFixedSize(40, 20);
        anim_ = new QPropertyAnimation(this, "pos", this);
        anim_->setDuration(120);
        anim_->setEasingCurve(QEasingCurve::OutCubic);
        connect(this, &QAbstractButton::toggled, this, [this](bool on) {
            anim_->stop();
            anim_->setStartValue(pos_);
            anim_->setEndValue(on ? 1.0 : 0.0);
            anim_->start();
        });
    }

    void set_accent(const QColor& c) { accent_ = c; update(); }

    qreal pos() const { return pos_; }
    void set_pos(qreal v) { pos_ = v; update(); }

    QSize sizeHint() const override { return { 40, 20 }; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const QRectF track(1.5, 1.5, width() - 3, height() - 3);
        const qreal r = track.height() / 2;
        const qreal d = 12;
        const qreal x = track.left() + 3 + pos_ * (track.width() - d - 6);
        const QRectF knob(x, (height() - d) / 2, d, d);

        if (isChecked())
        {
            QColor fill = accent_;
            if (underMouse() && isEnabled())
                fill = accent_.lighter(112);
            if (!isEnabled())
                fill = accent_.darker(160);
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawRoundedRect(track, r, r);
            p.setBrush(QColor(30, 30, 30));
            p.drawEllipse(knob);
        }
        else
        {
            QColor edge = isEnabled() ? (underMouse() ? QColor(0xaa, 0xaa, 0xaa)
                                                      : QColor(0x8a, 0x8a, 0x8a))
                                      : QColor(0x50, 0x50, 0x50);
            p.setPen(QPen(edge, 1.2));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(track, r, r);
            p.setPen(Qt::NoPen);
            p.setBrush(isEnabled() ? QColor(0xcf, 0xcf, 0xcf) : QColor(0x70, 0x70, 0x70));
            p.drawEllipse(knob);
        }
    }

private:
    qreal pos_ = 0;
    QColor accent_ = QColor(0x4c, 0xc2, 0xff);
    QPropertyAnimation* anim_;
};
