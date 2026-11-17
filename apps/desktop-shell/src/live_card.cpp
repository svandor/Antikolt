// SPDX-License-Identifier: MPL-2.0

#include "live_card.h"

#include <QFocusEvent>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QShowEvent>

#include "engine_client.h"

LiveCard::LiveCard(QWidget* parent) : QWidget(parent) {
    // A native window is the whole point: without it there is no handle to hand to the engine.
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(160, 120);
}

LiveCard::~LiveCard() {
    // The surface itself is destroyed by MainWindow, which owns the engine; here we only stop
    // tracking it, because destroying windows during Qt teardown is a good way to crash.
    surface_ = 0;
}

QSize LiveCard::physicalSize() const {
    const qreal ratio = devicePixelRatioF();
    return QSize(static_cast<int>(width() * ratio), static_cast<int>(height() * ratio));
}

bool LiveCard::attach(EngineClient* engine, const QString& partition) {
    if (engine == nullptr || !engine->isRunning()) {
        return false;
    }
    if (surface_ > 0) {
        detach();
    }
    engine_ = engine;
    partition_ = partition;

    // The protocol carries device independent pixels, because that is the unit the engine lays out
    // in: Chromium turns them into physical pixels with the monitor's scale factor, so a 900×640 card
    // on a 175 % screen really is 1575×1120 pixels and crisp. Sending physical pixels here makes
    // Chromium scale a second time (measured: the page saw a 1575 px viewport and the window was
    // clamped to the screen).
    const QSize logical = size();
    const int scale_percent = static_cast<int>(devicePixelRatioF() * 100.0 + 0.5);

    // `winId()` forces (and returns) this widget's native handle. The engine embeds into it.
    engine_->createSurface(
        partition_, logical.width(), logical.height(), scale_percent,
        static_cast<quintptr>(winId()), [this](const QJsonObject& result, const QString& error) {
            if (!error.isEmpty()) {
                return;  // the failure is reported by the engine-level handler in MainWindow
            }
            surface_ = static_cast<qint64>(result.value(QStringLiteral("surface")).toDouble());
            bounds_pushed_once_ = false;
            pushBounds();
            emit surfaceAttached(surface_);
        });
    return true;
}

void LiveCard::detach() {
    if (surface_ > 0) {
        const qint64 closed = surface_;
        surface_ = 0;
        bounds_pushed_once_ = false;
        emit surfaceDetached(closed);
    }
    engine_ = nullptr;
    update();
}

void LiveCard::pushBounds() {
    if (engine_ == nullptr || surface_ <= 0) {
        return;
    }
    const QSize logical = size();
    if (logical.isEmpty()) {
        return;
    }
    // Device independent pixels on the wire; the engine applies the monitor's scale factor.
    engine_->setBounds(surface_, 0, 0, logical.width(), logical.height());
    bounds_pushed_once_ = true;
}

void LiveCard::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    pushBounds();
}

void LiveCard::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // After the widget is on screen its physical geometry is final.
    if (surface_ > 0 && !bounds_pushed_once_) {
        pushBounds();
    }
}

void LiveCard::focusInEvent(QFocusEvent* event) {
    QWidget::focusInEvent(event);
    if (engine_ != nullptr && surface_ > 0) {
        engine_->focus(surface_, true);
    }
}

void LiveCard::focusOutEvent(QFocusEvent* event) {
    QWidget::focusOutEvent(event);
    if (engine_ != nullptr && surface_ > 0) {
        engine_->focus(surface_, false);
    }
}

void LiveCard::mousePressEvent(QMouseEvent* event) {
    // A click on the page area normally goes straight to Chromium's child window; this only fires on
    // the margins. Grabbing focus keeps the keyboard working after a toolbar click.
    setFocus(Qt::MouseFocusReason);
    QWidget::mousePressEvent(event);
}

void LiveCard::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    const QPalette pal = palette();
    painter.fillRect(rect(), pal.color(QPalette::Window));

    if (surface_ > 0) {
        return;  // the engine's child window covers this area
    }

    painter.setPen(pal.color(QPalette::Mid));
    QFont font = painter.font();
    font.setPointSizeF(font.pointSizeF() * 1.1);
    painter.setFont(font);
    painter.drawText(
        rect().adjusted(24, 24, -24, -24),
        Qt::AlignCenter | Qt::TextWordWrap,
        tr("Ez a kártya hibernálva van.\nA motor indításával éled újra — a lap, a görgetés és az "
           "űrlapok a lemezen várnak."));
}
