// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <vector>

#include <QObject>
#include <QPixmap>
#include <QPointer>
#include <QString>

class QWidget;

namespace openpak::qt {
class ToastCard;
}

// OpenPak's notifications (UX spec §3.10): cards in the chosen corner of the main window
// (Host::NotificationCorner, Bottom right by default), a category line in small capitals over
// one line of text, with the avatar or the game's icon when there is one. Each stays 6 s, and
// hovering pauses it; clicking opens its target. At most 4 show at once, the oldest leaving
// first. Nothing shows while the window is minimised or notifications are off, and only a game
// invitation shows while the emulator is not the active application.
class OpenPakToast : public QObject {
    Q_OBJECT

public:
    enum class Corner { TopRight, TopLeft, BottomRight, BottomLeft };
    // Offline, RequestSent and ChatRequest stay only so older hosts build; the spec shows none of
    // them, and they are drawn like the nearest kind that it does.
    enum class Kind { Online, Offline, Request, RequestSent, ChatRequest, GameInvite, Account, Saves, Mods };

    explicit OpenPakToast(QWidget* main_window);
    ~OpenPakToast() override;

    // The spec's form: the category comes from kind; on_click runs when the card is clicked
    // (clicked(kind) is emitted either way).
    void Show(Kind kind, const QString& text, const QPixmap& picture = {},
              std::function<void()> on_click = {});

    // The older form: headline and detail make the one line; the picture comes as base64.
    void Show(const QString& headline, const QString& detail, const QString& avatar_base64,
              Kind kind = Kind::Online);

    static constexpr int kMaxShown = 4;
    static constexpr int kShowMs = 6000;

signals:
    void clicked(OpenPakToast::Kind kind);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void Place();
    void Remove(openpak::qt::ToastCard* card);

    QWidget* main_window;
    std::vector<QPointer<openpak::qt::ToastCard>> cards; // newest last
};
