// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "openpak/qt/toast.h"

#include <algorithm>

#include <QApplication>
#include <QEnterEvent>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include "openpak/qt/avatar_cache.h"
#include "openpak/qt/host.h"
#include "openpak/qt/pages_p.h"
#include "openpak/qt/strings.h"

namespace openpak::qt {

namespace {
// Sizes at scale 1 (a 1920-wide screen or narrower). The widget is larger than the card by the
// shadow margin on every side, so the painted shadow is not clipped.
constexpr int kWidth = 330;
constexpr int kHeight = 74;
constexpr int kMargin = 16;  // gap from the window's edge
constexpr int kTopExtra = 10; // clear of the toolbar row
constexpr int kGap = 8;      // between stacked cards
constexpr int kShadowMargin = 12;
constexpr int kPictureSize = 40;
constexpr int kAccentBarWidth = 4;

float ScaleFor(QWidget* window) {
    QScreen* screen = window ? window->screen() : nullptr;
    if (!screen) {
        return 1.0f;
    }
    // 1 up to a 1920-wide screen, 2 by a 3840-wide one.
    return std::clamp(screen->geometry().width() / 1920.0f, 1.0f, 2.0f);
}

bool Dark() {
    Host* host = Host::Current();
    return host && host->IsDarkTheme();
}

QColor AccentFor(OpenPakToast::Kind kind) {
    using Kind = OpenPakToast::Kind;
    switch (kind) {
    case Kind::Online:
    case Kind::Offline:
        return QColor(88, 199, 122); // green: a friend
    case Kind::Request:
    case Kind::RequestSent:
    case Kind::ChatRequest:
    case Kind::GameInvite:
        return QColor(180, 120, 240); // purple: somebody wants something
    case Kind::Account:
        return QColor(100, 149, 237); // blue: OpenPak itself
    case Kind::Saves:
    case Kind::Mods:
        return QColor(232, 131, 62); // amber: something moved on disk
    }
    return QColor(100, 149, 237);
}

QString CategoryFor(OpenPakToast::Kind kind) {
    using Kind = OpenPakToast::Kind;
    switch (kind) {
    case Kind::Online:
    case Kind::Offline:
        return Tr("FRIEND ONLINE");
    case Kind::Request:
    case Kind::RequestSent:
        return Tr("FRIEND REQUEST");
    case Kind::GameInvite:
    case Kind::ChatRequest:
        return Tr("GAME INVITE");
    case Kind::Account:
        return Tr("OPENPAK");
    case Kind::Saves:
        return Tr("CLOUD SAVES");
    case Kind::Mods:
        return Tr("MODS");
    }
    return Tr("OPENPAK");
}
} // namespace

// One card. A top-level tool tip window: Qt::Tool maps to an xdg_toplevel on Wayland, which the
// client cannot place; Qt::ToolTip is an xdg_popup, which compositors place where asked.
class ToastCard : public QWidget {
public:
    ToastCard(QWidget* main_window, OpenPakToast::Kind kind_, const QString& text_,
              const QPixmap& picture_)
        : QWidget(main_window, Qt::FramelessWindowHint | Qt::ToolTip | Qt::WindowStaysOnTopHint |
                                   Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint),
          kind(kind_), text(text_), picture(picture_), scale(ScaleFor(main_window)) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_DeleteOnClose);
        setCursor(Qt::PointingHandCursor);
        setAccessibleName(CategoryFor(kind) + QStringLiteral(": ") + text);
        resize(static_cast<int>(kWidth * scale) + 2 * S(kShadowMargin),
               static_cast<int>(kHeight * scale) + 2 * S(kShadowMargin));
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, this, [this] { Leave(); });
        timer.start(OpenPakToast::kShowMs);
    }

    std::function<void()> on_click;
    std::function<void()> on_leave;

    void Leave() {
        timer.stop();
        if (on_leave) {
            auto leave = std::move(on_leave);
            leave();
        }
        close();
    }

protected:
    void enterEvent(QEnterEvent* event) override {
        // Hovering pauses the card; it goes on from where it was when the pointer leaves.
        remaining = timer.isActive() ? timer.remainingTime() : remaining;
        timer.stop();
        QWidget::enterEvent(event);
    }

    void leaveEvent(QEvent* event) override {
        timer.start((std::max)(remaining, 1000));
        QWidget::leaveEvent(event);
    }

    void mousePressEvent(QMouseEvent* event) override {
        QWidget::mousePressEvent(event);
        if (event->button() == Qt::LeftButton && on_click) {
            auto click = on_click;
            Leave();
            click();
            return;
        }
        Leave();
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                               QPainter::SmoothPixmapTransform);
        const int radius = S(12);
        const QRect card = rect().adjusted(S(kShadowMargin), S(kShadowMargin), -S(kShadowMargin),
                                           -S(kShadowMargin));

        // A soft shadow: low-alpha rounded rectangles growing outward.
        painter.setPen(Qt::NoPen);
        for (int i = S(8); i >= S(1); i -= (std::max)(S(1), 1)) {
            QPainterPath layer;
            layer.addRoundedRect(card.adjusted(-i, -i + S(2), i, i + S(2)), radius + i, radius + i);
            painter.fillPath(layer, QColor(0, 0, 0, 6));
        }

        QPainterPath background;
        background.addRoundedRect(card, radius, radius);
        painter.fillPath(background, Dark() ? QColor(32, 32, 37, 250) : QColor(250, 250, 252, 250));
        const QColor accent = AccentFor(kind);
        QPainterPath bar;
        bar.addRoundedRect(QRectF(card.left(), card.top(), S(kAccentBarWidth), card.height()), S(2),
                           S(2));
        painter.fillPath(bar.intersected(background), accent);

        int text_left = card.left() + S(18);
        if (!picture.isNull()) {
            const int size = S(kPictureSize);
            const QRect where(card.left() + S(16), card.top() + (card.height() - size) / 2, size, size);
            painter.save();
            QPainterPath clip;
            clip.addEllipse(where);
            painter.setClipPath(clip);
            painter.drawPixmap(where, picture);
            painter.restore();
            text_left = where.right() + S(14);
        }
        const int text_width = card.right() - S(14) - text_left;

        QFont category_font = QApplication::font();
        category_font.setBold(true);
        category_font.setPointSizeF((std::max)((category_font.pointSizeF() - 2) * scale, 6.5 * scale));
        category_font.setLetterSpacing(QFont::AbsoluteSpacing, S(1));
        QFont text_font = QApplication::font();
        text_font.setPointSizeF(text_font.pointSizeF() * scale);
        const QFontMetrics category_metrics(category_font);
        const QFontMetrics text_metrics(text_font);

        // The text may take two lines; past that it is elided.
        const QRect text_box(text_left, 0, text_width, text_metrics.height() * 2);
        const QRect needed = text_metrics.boundingRect(text_box, Qt::TextWordWrap, text);
        const int text_height = (std::min)(needed.height(), text_metrics.height() * 2);
        const int block = category_metrics.height() + S(3) + text_height;
        int top = card.top() + (card.height() - block) / 2;

        painter.setFont(category_font);
        painter.setPen(accent);
        painter.drawText(QRect(text_left, top, text_width, category_metrics.height()),
                         Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine, CategoryFor(kind));
        top += category_metrics.height() + S(3);

        painter.setFont(text_font);
        painter.setPen(Dark() ? QColor(240, 240, 243) : QColor(15, 15, 20));
        const QString shown = needed.height() > text_metrics.height() * 2
                                  ? text_metrics.elidedText(text, Qt::ElideRight, text_width * 2 - S(8))
                                  : text;
        painter.drawText(QRect(text_left, top, text_width, text_height),
                         Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, shown);
    }

private:
    int S(int base) const {
        return static_cast<int>(base * scale);
    }

    OpenPakToast::Kind kind;
    QString text;
    QPixmap picture;
    float scale;
    QTimer timer;
    int remaining = OpenPakToast::kShowMs;
};

} // namespace openpak::qt

using openpak::qt::ToastCard;
using namespace openpak::qt;

OpenPakToast::OpenPakToast(QWidget* main_window_) : QObject(main_window_), main_window(main_window_) {
    if (main_window) {
        main_window->installEventFilter(this);
    }
}

OpenPakToast::~OpenPakToast() {
    for (const auto& card : cards) {
        if (card) {
            card->on_leave = nullptr;
            card->close();
        }
    }
}

bool OpenPakToast::eventFilter(QObject* watched, QEvent* event) {
    if (watched == main_window) {
        // The cards are top-level windows of their own: they follow the main window by hand.
        if (event->type() == QEvent::WindowStateChange && main_window->isMinimized()) {
            for (const auto& card : std::vector(cards)) {
                if (card) {
                    card->Leave();
                }
            }
        } else if (event->type() == QEvent::Move || event->type() == QEvent::Resize) {
            Place();
        }
    }
    return QObject::eventFilter(watched, event);
}

void OpenPakToast::Show(Kind kind, const QString& text, const QPixmap& picture,
                         std::function<void()> on_click) {
    openpak::qt::Host* host = openpak::qt::Host::Current();
    if (!main_window || (host && !host->NotificationsEnabled())) {
        return;
    }
    // isMinimized() alone is unreliable on Wayland, which does not report the iconified state
    // the way X11 did; the window's visibility catches it there.
    const bool minimized = main_window->isMinimized() ||
                           (main_window->windowHandle() &&
                            main_window->windowHandle()->visibility() == QWindow::Minimized);
    if (minimized) {
        return;
    }
    // A game invitation is time-sensitive; the rest wait for somebody to be looking.
    if (kind != Kind::GameInvite && !qApp->activeWindow()) {
        return;
    }

    const int size = static_cast<int>(kPictureSize * ScaleFor(main_window));
    auto* card = new ToastCard(main_window, kind, text,
                               picture.isNull() ? QPixmap{} : openpak::qt::Round(picture, size));
    QPointer<OpenPakToast> self{this};
    card->on_click = [self, kind, on_click] {
        if (self) {
            emit self->clicked(kind);
        }
        if (on_click) {
            on_click();
        }
    };
    card->on_leave = [self, card] {
        if (self) {
            self->Remove(card);
        }
    };
    cards.push_back(card);
    // At most four at once; the oldest leaves first.
    while (cards.size() > static_cast<size_t>(kMaxShown)) {
        QPointer<ToastCard> oldest = cards.front();
        cards.erase(cards.begin());
        if (oldest) {
            oldest->on_leave = nullptr;
            oldest->close();
        }
    }
    Place();
    card->show();
    card->raise();
    // Some window managers place a window that does not take focus themselves at map time but
    // honour a move once it is shown.
    QTimer::singleShot(0, this, &OpenPakToast::Place);
}

void OpenPakToast::Show(const QString& headline, const QString& detail,
                         const QString& avatar_base64, Kind kind) {
    const QString text = detail.isEmpty() ? headline : headline + QLatin1Char(' ') + detail;
    const QPixmap picture =
        avatar_base64.isEmpty()
            ? QPixmap{}
            : OpenPak::AvatarCache::Get(headline.toStdString(), avatar_base64.toStdString(),
                                         kPictureSize * 2);
    Show(kind, text, picture);
}

void OpenPakToast::Remove(ToastCard* card) {
    std::erase_if(cards, [card](const QPointer<ToastCard>& shown) { return !shown || shown == card; });
    Place();
}

void OpenPakToast::Place() {
    if (!main_window) {
        return;
    }
    openpak::qt::Host* host = openpak::qt::Host::Current();
    const auto corner = static_cast<Corner>(
        std::clamp(host ? host->NotificationCorner() : static_cast<int>(Corner::BottomRight), 0, 3));
    const bool left = corner == Corner::TopLeft || corner == Corner::BottomLeft;
    const bool top = corner == Corner::TopLeft || corner == Corner::TopRight;
    const float scale = ScaleFor(main_window);
    const int margin = static_cast<int>(kMargin * scale);
    const int shadow = static_cast<int>(kShadowMargin * scale);
    const int gap = static_cast<int>(kGap * scale);
    const QPoint origin = main_window->mapToGlobal(QPoint(0, 0));

    // The newest nearest the corner, the older ones stacked away from it.
    int offset = 0;
    for (auto it = cards.rbegin(); it != cards.rend(); ++it) {
        ToastCard* card = *it;
        if (!card) {
            continue;
        }
        const int card_w = card->width() - 2 * shadow;
        const int card_h = card->height() - 2 * shadow;
        const int x = left ? origin.x() + margin : origin.x() + main_window->width() - margin - card_w;
        const int y = top ? origin.y() + margin + static_cast<int>(kTopExtra * scale) + offset
                          : origin.y() + main_window->height() - margin - card_h - offset;
        card->move(x - shadow, y - shadow);
        offset += card_h + gap;
    }
}
