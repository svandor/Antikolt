// SPDX-License-Identifier: MPL-2.0
//
// The one live card: a hole in the Qt UI where the engine draws.
//
// Invariant I1 (docs/HU/02-ARCHITEKTURA.md) says at most one card is alive at a time, so the shell
// has exactly one of these. It is a *native* widget: its `winId()` is handed to the engine host,
// which calls `CefWindowInfo::SetAsChild` on it. Chromium then creates its `Chrome_WidgetWin_0` as a
// child of that handle, so mouse, keyboard and pixels travel through the OS and never through our
// JSON protocol.
//
// What this widget owns:
//   * the geometry contract: Qt logical pixels in, physical pixels on the wire, on every resize;
//   * focus: a cross-process child does not inherit activation, so we forward it explicitly;
//   * the honest empty state: when no surface is attached it paints what a hibernated card looks
//     like, not a blank rectangle pretending to be a page.

#pragma once

#include <QString>
#include <QWidget>

class EngineClient;

class LiveCard : public QWidget {
    Q_OBJECT

public:
    explicit LiveCard(QWidget* parent = nullptr);
    ~LiveCard() override;

    /// Bind the card to an engine and create a surface embedded in this widget. Returns false when
    /// the engine is not running (the card then stays in its empty state).
    bool attach(EngineClient* engine, const QString& partition);

    /// Destroy the surface and go back to the empty state. Used by freezing and by card switches.
    void detach();

    qint64 surface() const { return surface_; }
    bool hasSurface() const { return surface_ > 0; }

    /// The rectangle the engine is told about, in physical pixels.
    QSize physicalSize() const;

signals:
    void surfaceAttached(qint64 surface);
    void surfaceDetached(qint64 surface);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    void pushBounds();

    EngineClient* engine_ = nullptr;
    QString partition_;
    qint64 surface_ = 0;
    bool bounds_pushed_once_ = false;
};
