// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <thread>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QEvent>
#include <QEventLoop>
#include <QFileDialog>
#include <QFont>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPropertyAnimation>
#include <QPainterPath>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "openpak/platform.h"
#include "openpak/account.h"
#include "openpak/qt/host.h"
#include "openpak/qt/account_dialog.h"
#include "openpak/qt/account_page_p.h"
#include "openpak/qt/avatar_cache.h"
#include "openpak/qt/extra_pages.h"
#include "openpak/qt/pages_p.h"
#include "openpak/session.h"
#include "openpak/qt/host.h"
#include "openpak/qt/network_probe.h"
#include "openpak/qt/host.h"
#include "openpak/qt/controller_navigation.h"

#ifdef ENABLE_WEB_SERVICE
#include "openpak/api.h"
#endif

namespace {

constexpr int kHeaderAvatarSize = 72;

QColor CardBg() {
    return openpak::qt::Host::Current()->IsDarkTheme() ? QColor(30, 30, 34) : QColor(244, 244, 248);
}

QColor DimColor() {
    return openpak::qt::Host::Current()->IsDarkTheme() ? QColor(150, 150, 158) : QColor(110, 110, 120);
}

QColor AccentColor() {
    const QString hex = QString::fromStdString(openpak::qt::Host::Current()->AccentColor().toStdString());
    if (QColor(hex).isValid()) {
        return QColor(hex);
    }
    const QColor pa = QApplication::palette().color(QPalette::Highlight);
    return (pa.isValid() && pa != Qt::black) ? pa : QColor(100, 149, 237);
}

QPixmap RoundedPixmap(const QPixmap& source, int size) {
    const qreal dpr = qApp->devicePixelRatio();
    QPixmap out(qMax(1, static_cast<int>(size * dpr)), qMax(1, static_cast<int>(size * dpr)));
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addEllipse(0, 0, size, size);
    painter.setClipPath(clip);
    if (source.isNull()) {
        painter.fillRect(0, 0, size, size, QColor(100, 149, 237));
    } else {
        painter.drawPixmap(0, 0, size, size, source);
    }
    return out;
}

QPixmap RoundedRectPixmap(const QPixmap& source, int size, int radius) {
    if (source.isNull()) {
        return {};
    }
    const qreal dpr = qApp->devicePixelRatio();
    QPixmap out(qMax(1, static_cast<int>(size * dpr)), qMax(1, static_cast<int>(size * dpr)));
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addRoundedRect(0, 0, size, size, radius, radius);
    painter.setClipPath(clip);
    painter.drawPixmap(0, 0, size, size, source);
    return out;
}

std::filesystem::path BackgroundImagePath() {
    return openpak::Platform::CacheDir() /
           "legacy_profile_background.png";
}

class HeaderCard : public QWidget {
public:
    explicit HeaderCard(QWidget* parent) : QWidget(parent) {
        auto* timer = new QTimer(this);
        timer->setInterval(33);
        connect(timer, &QTimer::timeout, this, [this] {
            phase += 0.035;
            update();
        });
        timer->start();
    }

    void SetBackgroundImage(const QPixmap& pixmap) {
        background = pixmap;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRect card = rect().adjusted(1, 1, -1, -1);
        QPainterPath clip_path;
        clip_path.addRoundedRect(card, 12, 12);
        painter.setClipPath(clip_path);
        painter.fillPath(clip_path, CardBg());

        if (!background.isNull()) {
            const QSize scaled_size = background.size().scaled(card.size(), Qt::KeepAspectRatioByExpanding);
            const QPixmap scaled = background.scaled(scaled_size, Qt::KeepAspectRatioByExpanding,
                                                      Qt::SmoothTransformation);
            const QPoint offset((card.width() - scaled.width()) / 2,
                                (card.height() - scaled.height()) / 2);
            painter.drawPixmap(card.topLeft() + offset, scaled);

            QLinearGradient scrim(card.left(), 0, card.right(), 0);
            scrim.setColorAt(0.0, QColor(0, 0, 0, 150));
            scrim.setColorAt(0.55, QColor(0, 0, 0, 80));
            scrim.setColorAt(1.0, QColor(0, 0, 0, 40));
            painter.fillRect(card, scrim);
        }

        const QColor accent = AccentColor();
        const qreal breathe = 0.5 + 0.5 * std::sin(phase);

        const int glow_w = std::min(100, card.width() / 3);
        QColor edge = accent;
        edge.setAlphaF(0.22 * breathe);
        QColor edge_fade = accent;
        edge_fade.setAlphaF(0.0);

        QLinearGradient left_glow(card.left(), 0, card.left() + glow_w, 0);
        left_glow.setColorAt(0.0, edge);
        left_glow.setColorAt(1.0, edge_fade);
        painter.fillRect(QRect(card.left(), card.top(), glow_w, card.height()), left_glow);

        QLinearGradient right_glow(card.right() - glow_w, 0, card.right(), 0);
        right_glow.setColorAt(0.0, edge_fade);
        right_glow.setColorAt(1.0, edge);
        painter.fillRect(QRect(card.right() - glow_w, card.top(), glow_w, card.height()), right_glow);

        if (background.isNull()) {
            struct WaveBand {
                qreal base_frac, amp_frac, phase_off, speed, shade;
                int alpha;
            };
            static constexpr WaveBand kBands[] = {
                {0.55, 0.05, 0.0, 1.5, 0.28, 70},   {0.42, 0.07, 1.1, 1.2, -0.30, 60},
                {0.70, 0.035, 2.3, 0.9, 0.50, 50},  {0.30, 0.09, 0.6, 1.7, -0.42, 42},
                {0.85, 0.03, 3.5, 0.7, 0.66, 34},
            };
            const auto shade = [](QColor c, qreal f) {
                const auto adj = [f](int v) {
                    return f >= 0.0 ? static_cast<int>(v + (255 - v) * f) : static_cast<int>(v * (1.0 + f));
                };
                return QColor(adj(c.red()), adj(c.green()), adj(c.blue()));
            };
            constexpr int kSteps = 48;
            for (const auto& band : kBands) {
                const qreal base_y = card.top() + card.height() * band.base_frac;
                const qreal amp = card.height() * band.amp_frac;
                const qreal ph = phase * band.speed + band.phase_off;

                QPainterPath fill;
                fill.moveTo(card.left(), card.bottom());
                for (int s = 0; s <= kSteps; ++s) {
                    const qreal nx = static_cast<qreal>(s) / kSteps;
                    const qreal x = card.left() + nx * card.width();
                    const qreal y = base_y + std::sin(nx * 2 * M_PI + ph) * amp +
                                   std::cos(nx * 2 * M_PI * 1.7 + ph * 0.8) * amp * 0.4;
                    fill.lineTo(x, y);
                }
                fill.lineTo(card.right(), card.bottom());
                fill.closeSubpath();

                QColor band_color = shade(accent, band.shade);
                band_color.setAlpha(band.alpha);
                painter.fillPath(fill, band_color);
            }
        }

        painter.setClipping(false);
        painter.setPen(QPen(QColor(255, 255, 255, 20), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(clip_path);
    }

private:
    qreal phase = 0.0;
    QPixmap background;
};

class ClickableFrame : public QFrame {
public:
    explicit ClickableFrame(QWidget* parent = nullptr) : QFrame(parent) {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
    }

    std::function<void()> on_click;

protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && on_click && rect().contains(event->pos())) {
            on_click();
        }
        QFrame::mouseReleaseEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override {
        if ((event->key() == Qt::Key_Space || event->key() == Qt::Key_Return ||
            event->key() == Qt::Key_Enter) &&
            on_click) {
            on_click();
            return;
        }
        QFrame::keyPressEvent(event);
    }

    void paintEvent(QPaintEvent* event) override {
        QFrame::paintEvent(event);
        if (!hasFocus()) {
            return;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(AccentColor(), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 10, 10);
    }
};

QString DashCardStyle() {
    return QStringLiteral("QFrame { background: %1; border-radius: 12px; }").arg(CardBg().name());
}

QFrame* MakeDashCard() {
    auto* card = new QFrame;
    card->setStyleSheet(DashCardStyle());
    return card;
}

QLabel* MakeCardTitle(const QString& text) {
    auto* label = new QLabel(text);
    QFont f = label->font();
    f.setBold(true);
    f.setPointSize(f.pointSize() + 1);
    label->setFont(f);
    return label;
}

QLabel* MakeDimLabel(const QString& text) {
    auto* label = new QLabel(text);
    QPalette pal = label->palette();
    pal.setColor(QPalette::WindowText, DimColor());
    label->setPalette(pal);
    return label;
}

class OpenPakToggleSwitch : public QWidget {
public:
    explicit OpenPakToggleSwitch(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(46, 26);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
    }

    void SetChecked(bool checked_) {
        checked = checked_;
        update();
    }
    bool IsChecked() const {
        return checked;
    }

    std::function<void(bool)> on_toggled;

protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && rect().contains(event->pos())) {
            Toggle();
        }
        QWidget::mouseReleaseEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return ||
            event->key() == Qt::Key_Enter) {
            Toggle();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRectF track = rect().adjusted(1, 1, -1, -1);
        const QColor track_color = checked ? AccentColor() : QColor(255, 255, 255, 35);
        QPainterPath track_path;
        track_path.addRoundedRect(track, track.height() / 2.0, track.height() / 2.0);
        painter.fillPath(track_path, track_color);

        if (hasFocus()) {
            painter.setPen(QPen(AccentColor(), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(track_path);
        }

        const qreal knob_d = track.height() - 6;
        const qreal knob_x = checked ? track.right() - knob_d - 3 : track.left() + 3;
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawEllipse(QRectF(knob_x, track.top() + 3, knob_d, knob_d));
    }

private:
    void Toggle() {
        checked = !checked;
        update();
        if (on_toggled) {
            on_toggled(checked);
        }
    }

    bool checked = false;
};

class OpenPakDropdownRow : public ClickableFrame {
public:
    OpenPakDropdownRow(const QString& text, bool selected, QWidget* parent = nullptr)
        : ClickableFrame(parent) {
        setObjectName(QStringLiteral("dropdownRow"));
        setStyleSheet(
            QStringLiteral("QFrame#dropdownRow { background: transparent; border-radius: 8px; }"
                          "QFrame#dropdownRow:hover { background: rgba(255,255,255,20); }"));
        auto* label = new QLabel(selected ? QStringLiteral("✓  ") + text : QStringLiteral("     ") + text);
        if (selected) {
            QFont f = label->font();
            f.setBold(true);
            label->setFont(f);
            label->setStyleSheet(QStringLiteral("color: %1;").arg(AccentColor().name()));
        } else {
            label->setStyleSheet(QStringLiteral("color: rgba(255,255,255,220);"));
        }
        auto* row_layout = new QHBoxLayout(this);
        row_layout->setContentsMargins(10, 8, 10, 8);
        row_layout->addWidget(label);
    }
};

class OpenPakDropdown : public QWidget {
public:
    explicit OpenPakDropdown(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedHeight(34);
        setMinimumWidth(160);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
    }

    void SetValues(QStringList values_, int current) {
        values = std::move(values_);
        index = std::clamp(current, 0, static_cast<int>(values.size()) - 1);
        update();
    }
    int CurrentIndex() const {
        return index;
    }

    std::function<void(int)> on_changed;
    std::function<void(QWidget*)> on_popup_opened;
    std::function<void()> on_popup_closed;

    void OpenPopup() {
        if (values.isEmpty() || popup) {
            return;
        }
        QWidget* root = window();
        popup = new QFrame(root);
        popup->setObjectName(QStringLiteral("dropdownPopup"));
        popup->setStyleSheet(QStringLiteral(
            "QFrame#dropdownPopup { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, "
            "  stop:0 rgba(32, 32, 38, 250), stop:1 rgba(22, 22, 27, 255)); "
            "  border: 1px solid rgba(255,255,255,40); border-radius: 10px; }"));

        auto* layout = new QVBoxLayout(popup);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(2);
        OpenPakDropdownRow* first_row = nullptr;
        for (int i = 0; i < values.size(); ++i) {
            auto* row = new OpenPakDropdownRow(values[i], i == index, popup);
            row->on_click = [this, i] { SetIndex(i); ClosePopup(); };
            layout->addWidget(row);
            if (!first_row) {
                first_row = row;
            }
        }

        popup->adjustSize();
        popup->setFixedWidth(std::max(width(), popup->sizeHint().width()));
        QPoint local_pos = mapTo(root, QPoint(0, height() + 4));
        if (local_pos.y() + popup->height() > root->height()) {
            local_pos = mapTo(root, QPoint(0, -popup->height() - 4));
        }
        if (local_pos.x() + popup->width() > root->width()) {
            local_pos.setX(root->width() - popup->width());
        }
        popup->move(local_pos);
        popup->show();
        popup->raise();
        if (first_row) {
            first_row->setFocus();
        }
        if (on_popup_opened) {
            on_popup_opened(popup);
        }
    }

    void ClosePopup() {
        if (!popup) {
            return;
        }
        popup->deleteLater();
        popup = nullptr;
        setFocus();
        if (on_popup_closed) {
            on_popup_closed();
        }
    }

    bool IsPopupOpen() const {
        return popup != nullptr;
    }

protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            popup ? ClosePopup() : OpenPopup();
        }
        QWidget::mouseReleaseEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return ||
            event->key() == Qt::Key_Enter) {
            OpenPopup();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRectF box = rect().adjusted(1, 1, -1, -1);
        QPainterPath box_path;
        box_path.addRoundedRect(box, 8, 8);
        painter.fillPath(box_path, QColor(255, 255, 255, 20));
        painter.setPen(QPen(hasFocus() ? AccentColor() : QColor(255, 255, 255, 30), hasFocus() ? 2 : 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(box_path);

        painter.setPen(QColor(255, 255, 255, 220));
        painter.setFont(font());
        const QString text = values.isEmpty() ? QString{} : values[index];
        painter.drawText(box.adjusted(12, 0, -24, 0), Qt::AlignVCenter | Qt::AlignLeft, text);

        QFont chevron_font = font();
        chevron_font.setBold(true);
        painter.setFont(chevron_font);
        painter.setPen(hasFocus() ? AccentColor() : DimColor());
        painter.drawText(QRectF(box.right() - 22, box.top(), 22, box.height()), Qt::AlignCenter,
                         QStringLiteral("▾"));
    }

private:
    void SetIndex(int new_index) {
        if (values.isEmpty()) {
            return;
        }
        index = new_index;
        update();
        if (on_changed) {
            on_changed(index);
        }
    }

    QStringList values;
    int index = 0;
    QPointer<QFrame> popup;
};

class OpenPakSignalIcon : public QWidget {
public:
    explicit OpenPakSignalIcon(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(26, 16);
        auto* timer = new QTimer(this);
        timer->setInterval(45);
        connect(timer, &QTimer::timeout, this, [this] {
            if (!online) {
                return;
            }
            phase += 0.09;
            update();
        });
        timer->start();
    }

    void SetOnline(bool online_) {
        online = online_;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        constexpr int kBars = 3;
        constexpr qreal kBarWidth = 5.0;
        constexpr qreal kGap = 3.0;

        for (int i = 0; i < kBars; ++i) {
            const qreal bar_h = height() * (0.42 + 0.29 * i);
            const qreal x = i * (kBarWidth + kGap);
            const qreal y = height() - bar_h;
            QPainterPath path;
            path.addRoundedRect(QRectF(x, y, kBarWidth, bar_h), 1.5, 1.5);

            QColor color;
            if (online) {
                const qreal wave = 0.5 + 0.5 * std::sin(phase - i * 1.1);
                color = QColor(50, 195, 85);
                color.setAlphaF(0.45 + 0.55 * wave);
            } else {
                color = QColor(224, 57, 62, 210);
            }
            painter.fillPath(path, color);
        }

        if (!online) {
            painter.setPen(QPen(QColor(224, 57, 62), 2.2, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(QPointF(0, 0), QPointF(width(), height()));
            painter.drawLine(QPointF(0, height()), QPointF(width(), 0));
        }
    }

private:
    bool online = true;
    qreal phase = 0.0;
};

} // Anonymous namespace

QString openpak::qt::style::Card() { return DashCardStyle(); }
QColor openpak::qt::style::Dim() { return DimColor(); }

class OpenPakPageNavArrow : public QWidget {
public:
    enum class Side { Left, Right };

    explicit OpenPakPageNavArrow(Side side_, QWidget* parent = nullptr)
        : QWidget(parent), side(side_) {
        setFixedHeight(40);
        setMinimumWidth(170); // fits "Invitations" and "Cloud saves", the longest page titles
    }

    void SetState(bool enabled_, const QString& label_) {
        widget_enabled = enabled_;
        label = label_;
        setCursor(widget_enabled ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }

    std::function<void()> on_click;

protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (widget_enabled && event->button() == Qt::LeftButton && on_click) {
            on_click();
        }
        QWidget::mouseReleaseEvent(event);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QColor fg = widget_enabled ? AccentColor() : DimColor();
        const qreal alpha = widget_enabled ? 1.0 : 0.35;

        QColor bubble = fg;
        bubble.setAlphaF(0.16 * alpha);
        QPainterPath bubble_path;
        const int bubble_size = 26;
        const QRect bubble_rect =
            side == Side::Left ? QRect(4, (height() - bubble_size) / 2, bubble_size, bubble_size)
                               : QRect(width() - bubble_size - 4, (height() - bubble_size) / 2,
                                       bubble_size, bubble_size);
        bubble_path.addEllipse(bubble_rect);
        painter.fillPath(bubble_path, bubble);
        painter.setPen(QPen(fg, 1.4));
        painter.drawPath(bubble_path);

        QFont bubble_font = font();
        bubble_font.setBold(true);
        painter.setFont(bubble_font);
        painter.setPen(fg);
        painter.drawText(bubble_rect, Qt::AlignCenter, side == Side::Left ? tr("L") : tr("R"));

        QFont label_font = font();
        label_font.setPointSize(label_font.pointSize() - 1);
        painter.setFont(label_font);
        QColor label_color = fg;
        label_color.setAlphaF(alpha);
        painter.setPen(label_color);

        const int text_margin = bubble_size + 10;
        const QRect text_rect = side == Side::Left
                                    ? QRect(text_margin, 0, width() - text_margin - 4, height())
                                    : QRect(4, 0, width() - text_margin - 4, height());
        painter.drawText(text_rect,
                         (side == Side::Left ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter,
                         QFontMetrics(label_font).elidedText(label, Qt::ElideRight, text_rect.width()));
    }

private:
    Side side;
    bool widget_enabled = false;
    QString label;
};


OpenPakAccountDialog::OpenPakAccountDialog(openpak::qt::Host* controller_, QWidget* parent,
                                             int initial_page_)
    : QDialog(parent), controller(controller_), initial_page(initial_page_),
      friends_snapshot(std::make_shared<WebService::OpenPakApi::AccountFriends>()) {
    setWindowTitle(tr("OpenPak"));
    setFixedSize(999, 598);

    // Otherwise the same physical controller drives both this dialog's navigation and a
    // running game simultaneously -- restored in the destructor.
    controller->SetGuestInputSuspended(true);

    controller_navigation = controller->CreateNavigation(this);
    if (!controller_navigation) {
        controller_navigation = new openpak::qt::Navigation(this);
    }
    WireControllerNav();

    queue = new openpak::qt::WorkQueue(this);

    status = new QLabel;
    status->setWordWrap(true);
    status->setStyleSheet(QStringLiteral("QLabel { background: rgba(255,255,255,15); "
                                         "color: rgba(255,255,255,200); border-radius: 10px; "
                                         "padding: 8px 14px; }"));
    busy = new QProgressBar;
    busy->setRange(0, 0); // indeterminate: how long a request takes is the server's business
    busy->setFixedSize(90, 4);
    busy->setTextVisible(false);
    busy->hide();
    queue->on_busy = [this](bool working) { busy->setVisible(working); };

    openpak::qt::PageContext context;
    context.host = controller;
    context.queue = queue;
    context.status = [this](const QString& message) { status->setText(message); };
    context.confirm = [this](const QString& title, const QString& message, const QString& yes) {
        return ConfirmAction(title, message, yes, tr("Cancel"));
    };
    context.close = [this] { accept(); };
    context.friends = friends_snapshot;
    context.friends_changed = [this] { UpdateDashboard(); };
    context.invite_to_chat = [this](u64 pid, const QString& name) {
        emit InviteToChatRequested(pid, name);
    };
    context.title = std::make_shared<std::string>();

    pages_stack = new QStackedWidget;
    pages_stack->addWidget(MakeAccountPage());
    pages = {openpak::qt::MakeFriendsPage(context),  openpak::qt::MakeInvitationsPage(context),
             openpak::qt::MakeSavesPage(context),    openpak::qt::MakeModsPage(context),
             openpak::qt::MakeNewsPage(context),     openpak::qt::MakeStatusPage(context)};
    for (openpak::qt::Page* page : pages) {
        pages_stack->addWidget(page);
    }
    page_titles = {tr("Account"),     tr("Friends"), tr("Invitations"), tr("Cloud saves"),
                   tr("Mods"),        tr("News"),    tr("Status")};

    page_title_label = new QLabel;
    QFont page_title_font = page_title_label->font();
    page_title_font.setBold(true);
    page_title_font.setPointSize(page_title_font.pointSize() + 1);
    page_title_label->setFont(page_title_font);
    page_title_label->setAlignment(Qt::AlignCenter);

    nav_left_arrow = new OpenPakPageNavArrow(OpenPakPageNavArrow::Side::Left);
    nav_left_arrow->on_click = [this] { GoToPage(current_page - 1); };
    nav_right_arrow = new OpenPakPageNavArrow(OpenPakPageNavArrow::Side::Right);
    nav_right_arrow->on_click = [this] { GoToPage(current_page + 1); };

    requests_badge = new QLabel;
    requests_badge->setAlignment(Qt::AlignCenter);
    requests_badge->setFixedSize(18, 18);
    requests_badge->setStyleSheet(
        QStringLiteral("QLabel { background-color: #e0393e; color: white; font-size: 10px; "
                       "font-weight: bold; border-radius: 9px; }"));
    requests_badge->hide();

    auto* nav_bar = new QHBoxLayout;
    nav_bar->addWidget(nav_left_arrow);
    nav_bar->addStretch();
    nav_bar->addWidget(page_title_label);
    nav_bar->addWidget(requests_badge);
    nav_bar->addStretch();
    nav_bar->addWidget(nav_right_arrow);

    auto* ok_button = new QPushButton(tr("OK"));
    ok_button->setCursor(Qt::PointingHandCursor);
    connect(ok_button, &QPushButton::clicked, this, &QDialog::accept);
    auto* status_bar = new QHBoxLayout;
    status_bar->setSpacing(10);
    status_bar->addWidget(busy, 0, Qt::AlignVCenter);
    status_bar->addWidget(status, 1);
    status_bar->addWidget(ok_button);

    setStyleSheet(
        QStringLiteral("QPushButton:focus, QToolButton:focus, QCheckBox:focus, "
                       "QComboBox:focus, QLineEdit:focus { "
                       "outline: none; border: 2px solid %1; border-radius: 6px; }")
            .arg(AccentColor().name()));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);
    layout->addLayout(nav_bar);
    layout->addWidget(pages_stack, 1);
    layout->addLayout(status_bar);

    LoadSavedBackground();
    GoToPage(std::clamp(initial_page, 0, pages_stack->count() - 1));
    UpdateHeroSizing();

    // Every page loads when the window opens, as the reference's do; the queue runs them one
    // after another, the page that is showing first.
    ReloadAll();

    // The reference's account cache redraws Friends and Invitations every 30 seconds.
    refresh_timer.setInterval(30000);
    connect(&refresh_timer, &QTimer::timeout, this, [this] {
        ReloadPage(kFriendsPage, false);
        ReloadPage(kInvitationsPage, false);
    });
    refresh_timer.start();

    // Signing in or out changes what every page can show.
    connect(controller, &openpak::qt::Host::AccountLinked, this, &OpenPakAccountDialog::ReloadAll);
    connect(controller, &openpak::qt::Host::AccountUnlinked, this, &OpenPakAccountDialog::ReloadAll);
    connect(controller, &openpak::qt::Host::StatusChanged, this,
            [this](const QString& message) { status->setText(message); });
}

OpenPakAccountDialog::~OpenPakAccountDialog() {
    controller->SetGuestInputSuspended(false);
}

QWidget* OpenPakAccountDialog::MakeAccountPage() {
    header_card = new HeaderCard(this);
    header_avatar = new QLabel;
    header_avatar->setFixedSize(kHeaderAvatarSize, kHeaderAvatarSize);
    header_avatar->setPixmap(RoundedPixmap({}, kHeaderAvatarSize));
    header_avatar->setCursor(Qt::PointingHandCursor);
    header_avatar->setToolTip(tr("Click to change your profile picture"));
    header_avatar->installEventFilter(this);

    header_name = new QLabel;
    header_name->setStyleSheet(QStringLiteral("QLabel { color: white; }"));
    auto* name_shadow = new QGraphicsDropShadowEffect(header_name);
    name_shadow->setBlurRadius(12);
    name_shadow->setColor(QColor(0, 0, 0, 230));
    name_shadow->setOffset(0, 1);
    header_name->setGraphicsEffect(name_shadow);

    edit_name_button = new QToolButton;
    edit_name_button->setText(QStringLiteral("✎"));
    edit_name_button->setCursor(Qt::PointingHandCursor);
    edit_name_button->setToolTip(tr("Change your username"));
    edit_name_button->setAutoRaise(true);
    edit_name_button->setFocusPolicy(Qt::NoFocus);
    connect(edit_name_button, &QToolButton::clicked, this, &OpenPakAccountDialog::OnEditUsername);

    auto* header_name_row = new QHBoxLayout;
    header_name_row->setSpacing(4);
    header_name_row->addWidget(header_name);
    header_name_row->addWidget(edit_name_button);
    header_name_row->addStretch();

    header_code = new QLabel;
    header_code->setCursor(Qt::PointingHandCursor);
    header_code->setToolTip(tr("Click to copy"));
    header_code->installEventFilter(this);
    header_code->setStyleSheet(QStringLiteral("QLabel { padding: 2px 8px; border-radius: 8px; "
                                              "background: rgba(0,0,0,150); color: white; }"));

    // The address this emulator's consoles reach, as the reference prints under the name.
    account_server = new QLabel;
    account_server->setStyleSheet(QStringLiteral("QLabel { color: rgba(255,255,255,200); }"));

    auto* header_text = new QVBoxLayout;
    header_text->setSpacing(6);
    header_text->addStretch();
    header_text->addLayout(header_name_row);
    header_text->addWidget(header_code, 0, Qt::AlignLeft);
    header_text->addWidget(account_server, 0, Qt::AlignLeft);
    header_text->addStretch();

    auto* header_content = new QHBoxLayout;
    header_content->setSpacing(16);
    header_content->addWidget(header_avatar);
    header_content->addLayout(header_text);

    auto* background_button = new QToolButton;
    background_button->setText(QStringLiteral("🖼"));
    background_button->setCursor(Qt::PointingHandCursor);
    background_button->setToolTip(tr("Change the background image"));
    background_button->setAutoRaise(true);
    background_button->setFocusPolicy(Qt::NoFocus);
    background_button->setPopupMode(QToolButton::InstantPopup);
    background_button->setStyleSheet(QStringLiteral("QToolButton { color: white; }"));
    auto* background_menu = new QMenu(background_button);
    background_menu->addAction(tr("Choose Background Image..."), this,
                               &OpenPakAccountDialog::OnChangeBackground);
    background_menu->addAction(tr("Remove Background"), this,
                               &OpenPakAccountDialog::OnRemoveBackground);
    background_button->setMenu(background_menu);

    sign_button = new QPushButton;
    sign_button->setCursor(Qt::PointingHandCursor);
    connect(sign_button, &QPushButton::clicked, this, &OpenPakAccountDialog::OnSignInOrOut);

    auto* header_top_row = new QHBoxLayout;
    header_top_row->addStretch();
    header_top_row->addWidget(sign_button);
    header_top_row->addWidget(background_button);

    auto* header_middle_row = new QHBoxLayout;
    header_middle_row->addStretch();
    header_middle_row->addLayout(header_content);
    header_middle_row->addStretch();

    auto* header_outer = new QVBoxLayout(header_card);
    header_outer->setContentsMargins(10, 6, 10, 14);
    header_outer->addLayout(header_top_row);
    header_outer->addLayout(header_middle_row);
    header_outer->addStretch();
    header_card->setMinimumHeight(190);
    header_card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // The account's facts, as the reference's second card lists them.
    auto* details_card = MakeDashCard();
    auto* details_layout = new QGridLayout(details_card);
    details_layout->setContentsMargins(18, 14, 18, 14);
    details_layout->setHorizontalSpacing(16);
    details_layout->setVerticalSpacing(8);
    details_layout->setColumnMinimumWidth(0, 150);
    details_layout->setColumnStretch(1, 1);
    int detail_row = 0;
    const auto add_detail = [&](const QString& label, QLabel*& value, QWidget* extra = nullptr) {
        details_layout->addWidget(MakeDimLabel(label), detail_row, 0);
        value = new QLabel(QStringLiteral("—"));
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setWordWrap(true);
        details_layout->addWidget(value, detail_row, 1);
        if (extra) {
            details_layout->addWidget(extra, detail_row, 2);
        }
        ++detail_row;
    };
    auto* copy_button = new QPushButton(tr("Copy"));
    copy_button->setCursor(Qt::PointingHandCursor);
    connect(copy_button, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(account_code->text());
        status->setText(tr("Copied."));
    });
    add_detail(tr("Friend code"), account_code, copy_button);
    account_code->setFont(QFont(QStringLiteral("monospace")));
    add_detail(tr("Switch identity"), account_pid);
    add_detail(tr("Linked consoles"), account_consoles);
    add_detail(tr("Console link"), account_link);
    add_detail(tr("Players online"), account_players);

    account_signed_out = new QLabel(tr("Sign in to OpenPak to use this."));
    account_signed_out->setAlignment(Qt::AlignCenter);
    account_signed_out->setStyleSheet(DashCardStyle());
    account_signed_out->setMinimumHeight(120);
    account_details = details_card;

    auto* status_card = MakeDashCard();
    auto* status_card_layout = new QVBoxLayout(status_card);
    status_card_layout->setContentsMargins(18, 16, 18, 16);
    status_card_layout->setSpacing(10);
    auto* status_title = MakeCardTitle(tr("OpenPak Status"));
    status_title->setAlignment(Qt::AlignCenter);
    status_card_layout->addWidget(status_title);

    auto* online_icon = new OpenPakSignalIcon;
    dash_online_dot = online_icon;
    dash_online_text = new QLabel;
    dash_online_text->setAlignment(Qt::AlignCenter);
    const bool redirect_enabled = controller->RedirectEnabled();
    online_icon->SetOnline(redirect_enabled);
    if (redirect_enabled) {
        dash_online_text->setText(tr("Online"));
        dash_online_text->setStyleSheet(QStringLiteral("color: #3fdb76; font-weight: 600;"));
    } else {
        dash_online_text->setText(tr("Offline: Please enable \"OpenPak Redirection\""));
        dash_online_text->setStyleSheet(QStringLiteral("color: #ff5b56; font-weight: 600;"));
        dash_online_text->setWordWrap(true);
    }
    auto* online_row = new QHBoxLayout;
    online_row->setSpacing(10);
    online_row->addStretch();
    online_row->addWidget(dash_online_dot, 0, Qt::AlignVCenter);
    online_row->addWidget(dash_online_text, 0, Qt::AlignVCenter);
    online_row->addStretch();
    status_card_layout->addLayout(online_row);

    auto* stats_row = new QHBoxLayout;
    stats_row->setSpacing(0);
    const auto make_stat = [&](const QString& label, QLabel*& value_out) {
        auto* col = new QVBoxLayout;
        col->setSpacing(4);
        auto* label_widget = MakeDimLabel(label);
        label_widget->setAlignment(Qt::AlignHCenter);
        col->addWidget(label_widget);
        value_out = new QLabel(QStringLiteral("0"));
        QFont vf = value_out->font();
        vf.setBold(true);
        vf.setPointSize(vf.pointSize() + 7);
        value_out->setFont(vf);
        value_out->setAlignment(Qt::AlignHCenter);
        col->addWidget(value_out);
        stats_row->addLayout(col, 1);
    };
    make_stat(tr("Friends Online"), dash_friends_online_value);
    make_stat(tr("In-Game"), dash_in_game_value);
    status_card_layout->addLayout(stats_row);

    // The connection check and the notification settings, which only this window offers.
    auto* settings_card = MakeDashCard();
    auto* settings_layout = new QVBoxLayout(settings_card);
    settings_layout->setContentsMargins(18, 14, 18, 14);
    settings_layout->setSpacing(4);
    const auto make_value_pill = [](const QString& text) {
        auto* label = new QLabel(text);
        label->setAlignment(Qt::AlignCenter);
        label->setFixedWidth(96);
        label->setStyleSheet(QStringLiteral("background: rgba(255,255,255,25); color: %1; "
                                            "font-weight: 600; border-radius: 9px; padding: 2px 10px;")
                                 .arg(DimColor().name()));
        return label;
    };
    const auto settings_row = [&](const QString& title, QWidget* value) {
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(title));
        row->addStretch(1);
        row->addWidget(value);
        settings_layout->addLayout(row);
    };
    nat_label = make_value_pill(tr("Not Tested"));
    ping_label = make_value_pill(QStringLiteral("--"));
    settings_row(tr("NAT Type"), nat_label);
    settings_row(tr("Ping"), ping_label);
    auto* test_connection_button = new QPushButton(tr("Test Connection"));
    test_connection_button->setCursor(Qt::PointingHandCursor);
    settings_layout->addWidget(test_connection_button);
    home_default_focus = test_connection_button;

    auto* notifications_toggle = new OpenPakToggleSwitch;
    notifications_toggle->SetChecked(controller->NotificationsEnabled());
    notifications_toggle->on_toggled = [this](bool checked) {
        controller->SetNotificationsEnabled(checked);
    };
    settings_row(tr("Notifications"), notifications_toggle);
    auto* notification_corner = new OpenPakDropdown;
    notification_corner->SetValues({tr("Top Right"), tr("Top Left"), tr("Bottom Right"),
                                    tr("Bottom Left")},
                                   std::clamp(controller->NotificationCorner(), 0, 3));
    notification_corner->on_popup_opened = [this](QWidget* popup) { active_popup = popup; };
    notification_corner->on_popup_closed = [this] { active_popup = nullptr; };
    notification_corner->on_changed = [this](int index) { controller->SetNotificationCorner(index); };
    settings_row(tr("Corner"), notification_corner);
    settings_layout->addStretch(1);

    auto* right_column = new QVBoxLayout;
    right_column->setSpacing(12);
    right_column->addWidget(status_card);
    right_column->addWidget(settings_card, 1);
    auto* right_column_widget = new QWidget;
    right_column_widget->setLayout(right_column);
    right_column_widget->setFixedWidth(300);

    auto* left_column = new QVBoxLayout;
    left_column->setSpacing(12);
    left_column->addWidget(header_card, 1);
    left_column->addWidget(details_card);
    left_column->addWidget(account_signed_out);

    auto* page = new QWidget;
    auto* page_layout = new QHBoxLayout(page);
    page_layout->setContentsMargins(0, 0, 0, 0);
    page_layout->setSpacing(16);
    page_layout->addLayout(left_column, 1);
    page_layout->addWidget(right_column_widget);

    network_probe = new ExternalReferenceProbe(this);
    status_check_timer = new QTimer(this);
    status_check_timer->setInterval(350);
    const QString checking_style =
        QStringLiteral("background: rgba(255,255,255,25); color: %1; font-weight: 600; "
                       "border-radius: 9px; padding: 2px 10px;")
            .arg(DimColor().name());
    connect(status_check_timer, &QTimer::timeout, this, [this] {
        status_check_dots = (status_check_dots + 1) % 4;
        const QString dots = QString(status_check_dots, QLatin1Char('.'));
        if (nat_checking) {
            nat_label->setText(tr("Checking") + dots);
        }
        if (ping_checking) {
            ping_label->setText(tr("Checking") + dots);
        }
        if (!nat_checking && !ping_checking) {
            status_check_timer->stop();
        }
    });
    const auto pill = [](const char* rgb, const char* hex) {
        return QStringLiteral("background: rgba(%1,40); color: %2; font-weight: 600; "
                              "border-radius: 9px; padding: 2px 10px;")
            .arg(QLatin1String(rgb), QLatin1String(hex));
    };
    connect(network_probe, &ExternalReferenceProbe::NatStatusChanged, this,
            [this, checking_style, pill](ExternalReferenceProbe::NatStatus nat_status) {
                switch (nat_status) {
                case ExternalReferenceProbe::NatStatus::Checking:
                    nat_checking = true;
                    nat_label->setText(tr("Checking"));
                    nat_label->setStyleSheet(checking_style);
                    break;
                case ExternalReferenceProbe::NatStatus::Open:
                    nat_checking = false;
                    nat_label->setText(tr("Open"));
                    nat_label->setStyleSheet(pill("50,195,85", "#32c355"));
                    break;
                case ExternalReferenceProbe::NatStatus::Strict:
                    nat_checking = false;
                    nat_label->setText(tr("Strict"));
                    nat_label->setStyleSheet(pill("232,131,62", "#e8833e"));
                    break;
                case ExternalReferenceProbe::NatStatus::Unknown:
                    nat_checking = false;
                    nat_label->setText(tr("Unknown"));
                    nat_label->setStyleSheet(pill("224,57,62", "#e0393e"));
                    break;
                }
            });
    connect(network_probe, &ExternalReferenceProbe::PingResult, this, [this](int ms) {
        ping_checking = false;
        ping_label->setText(ms >= 0 ? tr("%1 ms").arg(ms) : QStringLiteral("--"));
    });
    connect(test_connection_button, &QPushButton::clicked, this,
            [this, test_connection_button, checking_style] {
                nat_checking = true;
                ping_checking = true;
                status_check_dots = 0;
                nat_label->setText(tr("Checking"));
                nat_label->setStyleSheet(checking_style);
                ping_label->setText(tr("Checking"));
                ping_label->setStyleSheet(checking_style);
                status_check_timer->start();
                network_probe->ProbeNat();
                network_probe->PingBackend();
                test_connection_button->setEnabled(false);
                QTimer::singleShot(10000, test_connection_button, [test_connection_button] {
                    test_connection_button->setEnabled(true);
                });
            });
    return page;
}

void OpenPakAccountDialog::RefreshAccount() {
    const bool signed_in = openpak::qt::SignedIn();
    sign_button->setText(signed_in ? tr("Sign out") : tr("Sign in..."));
    account_details->setVisible(signed_in);
    account_signed_out->setVisible(!signed_in);
    edit_name_button->setVisible(signed_in);
    const std::string server = controller->ServerIp();
    account_server->setText(server.empty() ? QString{} : QString::fromStdString(server));
    account_server->setVisible(!server.empty());

    // What the library already holds, drawn at once; the site's answer fills in the rest.
    const std::string name = Common::OpenPakAccount::GetUsername();
    const std::string code = Common::OpenPakAccount::GetFriendCode();
    header_name->setText(signed_in ? QString::fromStdString(name.empty() ? "—" : name)
                                   : tr("Not signed in"));
    header_code->setText(QString::fromStdString(code));
    header_code->setVisible(signed_in && !code.empty());
    account_code->setText(code.empty() ? QStringLiteral("—") : QString::fromStdString(code));
    const u64 pid = Common::OpenPakAccount::GetPid();
    account_pid->setText(pid != 0 ? tr("PID %1").arg(pid) : QStringLiteral("—"));
    account_link->setText(openpak::client::session::Linked()
                              ? tr("Linked as %1").arg(QString::fromStdString(
                                    openpak::client::session::Nickname()))
                              : tr("This emulator's console account is not linked yet; signing in "
                                   "links it."));
    if (!signed_in) {
        header_avatar_source = {};
        UpdateHeroSizing();
        return;
    }

    struct Loaded {
        WebService::OpenPakApi::Profile profile;
        WebService::OpenPakApi::NetworkStatus population;
    };
    queue->Run(
        this,
        [] { return Loaded{WebService::OpenPakApi::GetProfile(), WebService::OpenPakApi::GetStatus()}; },
        [this](Loaded loaded) {
            const auto& profile = loaded.profile;
            if (profile.ok) {
                if (!profile.name.empty()) {
                    header_name->setText(QString::fromStdString(profile.name));
                }
                // The Switch identity's code wins, as in the reference; the site's fills in for
                // an account without one.
                if (Common::OpenPakAccount::GetFriendCode().empty() && !profile.friend_code.empty()) {
                    account_code->setText(QString::fromStdString(profile.friend_code));
                    header_code->setText(QString::fromStdString(profile.friend_code));
                    header_code->show();
                }
                QStringList consoles;
                for (const std::string& name_space : profile.linked_platforms) {
                    consoles << openpak::qt::ConsoleName(name_space);
                }
                account_consoles->setText(consoles.isEmpty() ? QStringLiteral("—")
                                                             : consoles.join(QStringLiteral(", ")));
                const QPixmap avatar = OpenPak::AvatarCache::Get("self", profile.image_base64, 256);
                header_avatar_source = avatar;
                UpdateHeroSizing();
            } else if (!profile.error.empty()) {
                status->setText(QString::fromStdString(profile.error));
            }
            account_players->setText(
                loaded.population.ok
                    ? tr("%n player(s) online", nullptr, loaded.population.players_online)
                    : QStringLiteral("—"));
        });
}

void OpenPakAccountDialog::OnSignInOrOut() {
    if (openpak::qt::SignedIn()) {
        // No confirmation, as in the reference: signing back in is one step away.
        controller->SignOut();
        *friends_snapshot = {};
        status->setText(tr("Signed out of OpenPak."));
    } else {
        controller->SignIn();
    }
    ReloadAll();
}

void OpenPakAccountDialog::ReloadPage(int index, bool force) {
    if (index == kAccountPage) {
        RefreshAccount();
    } else if (index > 0 && index - 1 < static_cast<int>(pages.size())) {
        pages[index - 1]->reload(force);
    }
}

void OpenPakAccountDialog::ReloadAll() {
    // The page that is showing first, then the rest in order; the queue keeps them all.
    ReloadPage(current_page, true);
    for (int index = 0; index < pages_stack->count(); ++index) {
        if (index != current_page) {
            ReloadPage(index, false);
        }
    }
}

void OpenPakAccountDialog::UpdateDashboard() {
    int online = 0;
    int in_game = 0;
    for (const auto& entry : friends_snapshot->friends) {
        online += entry.online ? 1 : 0;
        in_game += entry.online && !entry.title_id.empty() ? 1 : 0;
    }
    dash_friends_online_value->setText(QString::number(online));
    dash_in_game_value->setText(QString::number(in_game));

    const auto incoming = std::count_if(friends_snapshot->requests.begin(), friends_snapshot->requests.end(),
                                        [](const auto& request) { return request.incoming; });
    if (incoming > 0) {
        requests_badge->setText(incoming > 99 ? QStringLiteral("99+") : QString::number(incoming));
        requests_badge->setToolTip(tr("Friend requests waiting on the Friends page"));
        requests_badge->show();
    } else {
        requests_badge->hide();
    }
}

bool OpenPakAccountDialog::eventFilter(QObject* watched, QEvent* event) {
    const bool is_activate_key =
        event->type() == QEvent::KeyPress &&
        (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Space ||
         static_cast<QKeyEvent*>(event)->key() == Qt::Key_Return ||
         static_cast<QKeyEvent*>(event)->key() == Qt::Key_Enter);

    if (watched == header_code &&
        (event->type() == QEvent::MouseButtonRelease || is_activate_key)) {
        QApplication::clipboard()->setText(header_code->text());
        status->setText(tr("Copied."));
        return true;
    }
    if (watched == header_avatar && openpak::qt::SignedIn() &&
        (event->type() == QEvent::MouseButtonRelease || is_activate_key)) {
        OnChangeAvatar();
        return true;
    }
    return QDialog::eventFilter(watched, event);
}

void OpenPakAccountDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_PageDown) {
        GoToPage(current_page + 1);
        return;
    }
    if (event->key() == Qt::Key_PageUp) {
        GoToPage(current_page - 1);
        return;
    }
    QDialog::keyPressEvent(event);
}

void OpenPakAccountDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    UpdateHeroSizing();
}

void OpenPakAccountDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (QApplication::focusWidget() && isAncestorOf(QApplication::focusWidget())) {
        return;
    }
    if (current_page == kAccountPage && home_default_focus) {
        home_default_focus->setFocus();
    } else {
        focusNextChild();
    }
}

void OpenPakAccountDialog::UpdateHeroSizing() {
    if (!header_card || !header_avatar) {
        return;
    }
    const int card_h = std::max(header_card->height(), 1);
    const int avatar_size = std::clamp(static_cast<int>(card_h * 0.55), 72, 200);
    header_avatar->setFixedSize(avatar_size, avatar_size);
    header_avatar->setPixmap(RoundedPixmap(header_avatar_source, avatar_size));

    QFont name_font = header_name->font();
    name_font.setPointSizeF(std::clamp(avatar_size * 0.19, 15.0, 30.0));
    name_font.setBold(true);
    header_name->setFont(name_font);

    QFont code_font(QStringLiteral("monospace"));
    code_font.setPointSizeF(std::clamp(avatar_size * 0.105, 10.0, 16.0));
    header_code->setFont(code_font);
}

void OpenPakAccountDialog::GoToPage(int index) {
    if (!pages_stack) {
        return;
    }
    index = std::clamp(index, 0, pages_stack->count() - 1);
    if (index == current_page) {
        return;
    }
    const bool first = current_page < 0;
    current_page = index;
    pages_stack->setCurrentIndex(index);
    if (page_title_label && index < page_titles.size()) {
        page_title_label->setText(page_titles[index]);
    }
    UpdatePageNav();
    // A page is refreshed each time it is navigated to, as the reference's RefreshPage does;
    // the window's opening loads are ReloadAll's.
    if (!first) {
        ReloadPage(index, false);
    }
    if (index == kAccountPage && home_default_focus) {
        home_default_focus->setFocus();
    } else {
        focusNextChild();
    }
}

void OpenPakAccountDialog::WireControllerNav() {
    connect(controller_navigation, &openpak::qt::Navigation::leftShoulderPressed, this,
            [this] { GoToPage(current_page - 1); });
    connect(controller_navigation, &openpak::qt::Navigation::rightShoulderPressed, this,
            [this] { GoToPage(current_page + 1); });
    connect(controller_navigation, &openpak::qt::Navigation::navigated, this,
            &OpenPakAccountDialog::OnDirectionalNav);
    connect(controller_navigation, &openpak::qt::Navigation::activated, this,
            &OpenPakAccountDialog::OnControllerActivate, Qt::QueuedConnection);
    connect(controller_navigation, &openpak::qt::Navigation::backPressed, this, [this] {
        if (active_popup) {
            active_popup->deleteLater();
            active_popup = nullptr;
            return;
        }
        reject();
    });
}

void OpenPakAccountDialog::UpdatePageNav() {
    if (!nav_left_arrow || !nav_right_arrow || !pages_stack) {
        return;
    }
    const int last = pages_stack->count() - 1;
    nav_left_arrow->SetState(current_page > 0,
                             current_page > 0 ? page_titles[current_page - 1] : QString{});
    nav_right_arrow->SetState(current_page < last,
                              current_page < last ? page_titles[current_page + 1] : QString{});
}

void OpenPakAccountDialog::OnDirectionalNav(int dx, int dy) {
    QWidget* focused = QApplication::focusWidget();
    if (focused && !isAncestorOf(focused)) {
        focused = nullptr;
    }
    if (auto* combo = qobject_cast<QComboBox*>(focused); combo && dy != 0) {
        combo->setCurrentIndex(std::clamp(combo->currentIndex() + dy, 0, combo->count() - 1));
        return;
    }
    if (dy > 0 || dx > 0) {
        focusNextChild();
    } else if (dy < 0 || dx < 0) {
        focusPreviousChild();
    }
    // Keep what the stick moved to on screen in the long lists.
    if (QWidget* now = QApplication::focusWidget()) {
        for (QWidget* up = now->parentWidget(); up; up = up->parentWidget()) {
            if (auto* scroll = qobject_cast<QScrollArea*>(up)) {
                scroll->ensureWidgetVisible(now);
                break;
            }
        }
    }
}

void OpenPakAccountDialog::OnControllerActivate() {
    QWidget* focused = QApplication::focusWidget();
    if (!focused || !isAncestorOf(focused)) {
        return;
    }
    if (auto* button = qobject_cast<QAbstractButton*>(focused)) {
        button->click();
        return;
    }
    if (auto* combo = qobject_cast<QComboBox*>(focused)) {
        combo->showPopup();
        return;
    }
    QKeyEvent key(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(focused, &key);
}

bool OpenPakAccountDialog::ConfirmAction(const QString& title, const QString& message,
                                          const QString& yes_text, const QString& no_text,
                                          const std::string& icon_base64) {
    auto* scrim = new QWidget(this);
    scrim->setGeometry(rect());
    scrim->setStyleSheet(QStringLiteral("background: rgba(0,0,0,140);"));

    auto* card = new QFrame(scrim);
    card->setObjectName(QStringLiteral("confirmCard"));
    card->setFixedWidth(460);
    card->setStyleSheet(QStringLiteral(
        "QFrame#confirmCard { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, "
        "  stop:0 rgba(32, 32, 38, 250), stop:1 rgba(22, 22, 27, 255)); "
        "  border: 1px solid rgba(255,255,255,40); border-radius: 20px; }"));

    auto* title_label = new QLabel(title);
    QFont title_font = title_label->font();
    title_font.setBold(true);
    title_font.setPointSize(title_font.pointSize() + 2);
    title_label->setFont(title_font);
    title_label->setStyleSheet(QStringLiteral("color: #ffffff; background: transparent;"));
    title_label->setAlignment(Qt::AlignCenter);

    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(32, 24, 32, 24);
    card_layout->setSpacing(14);
    card_layout->addWidget(title_label);

    if (!icon_base64.empty()) {
        QPixmap icon_source;
        icon_source.loadFromData(QByteArray::fromBase64(QByteArray::fromStdString(icon_base64)));
        if (!icon_source.isNull()) {
            auto* icon_label = new QLabel;
            icon_label->setFixedSize(88, 88);
            icon_label->setPixmap(RoundedRectPixmap(icon_source, 88, 12));
            icon_label->setAlignment(Qt::AlignCenter);
            card_layout->addWidget(icon_label, 0, Qt::AlignHCenter);
        }
    }

    auto* message_label = new QLabel(message);
    message_label->setWordWrap(true);
    message_label->setAlignment(Qt::AlignCenter);
    message_label->setStyleSheet(QStringLiteral("color: #ffffff; font-weight: 500; background: transparent;"));
    card_layout->addWidget(message_label);

    const QString button_style =
        QStringLiteral("QPushButton { background: rgba(255,255,255,10); "
                       "border: 1px solid rgba(255,255,255,25); border-radius: 14px; "
                       "color: #ffffff; padding: 10px 24px; }"
                       "QPushButton:hover { background: rgba(255,255,255,20); "
                       "border-color: rgba(255,255,255,60); }"
                       "QPushButton:focus { background: %1; border-color: %1; "
                       "color: #000000; font-weight: bold; }")
            .arg(AccentColor().name());

    auto* no_button = new QPushButton(no_text);
    no_button->setStyleSheet(button_style);
    auto* yes_button = new QPushButton(yes_text);
    yes_button->setStyleSheet(button_style);

    auto* button_row = new QHBoxLayout;
    button_row->setSpacing(12);
    button_row->addWidget(no_button);
    button_row->addWidget(yes_button);
    card_layout->addSpacing(4);
    card_layout->addLayout(button_row);

    card->adjustSize();
    card->move((scrim->width() - card->width()) / 2, (scrim->height() - card->height()) / 2);
    auto* scrim_opacity = new QGraphicsOpacityEffect(scrim);
    scrim_opacity->setOpacity(0.0);
    scrim->setGraphicsEffect(scrim_opacity);
    scrim->show();
    scrim->raise();
    no_button->setFocus();

    auto* fade_in = new QPropertyAnimation(scrim_opacity, "opacity", scrim);
    fade_in->setDuration(150);
    fade_in->setStartValue(0.0);
    fade_in->setEndValue(1.0);
    fade_in->setEasingCurve(QEasingCurve::OutCubic);
    fade_in->start(QAbstractAnimation::DeleteWhenStopped);

    QEventLoop loop;
    bool result = false;
    connect(yes_button, &QPushButton::clicked, &loop, [&] {
        result = true;
        loop.quit();
    });
    connect(no_button, &QPushButton::clicked, &loop, [&] { loop.quit(); });

    disconnect(controller_navigation, nullptr, this, nullptr);
    const auto nav_conn = connect(controller_navigation, &openpak::qt::Navigation::navigated, &loop,
                                  [this](int dx, int) {
                                      if (dx > 0) {
                                          focusNextChild();
                                      } else if (dx < 0) {
                                          focusPreviousChild();
                                      }
                                  });
    const auto activate_conn =
        connect(controller_navigation, &openpak::qt::Navigation::activated, &loop, [] {
            if (auto* button = qobject_cast<QAbstractButton*>(QApplication::focusWidget())) {
                button->click();
            }
        });
    const auto back_conn = connect(controller_navigation, &openpak::qt::Navigation::backPressed,
                                   &loop, [&loop] { loop.quit(); });

    loop.exec();

    disconnect(nav_conn);
    disconnect(activate_conn);
    disconnect(back_conn);
    WireControllerNav();

    QEventLoop fade_loop;
    auto* fade_out = new QPropertyAnimation(scrim_opacity, "opacity", scrim);
    fade_out->setDuration(130);
    fade_out->setStartValue(1.0);
    fade_out->setEndValue(0.0);
    fade_out->setEasingCurve(QEasingCurve::InCubic);
    connect(fade_out, &QPropertyAnimation::finished, &fade_loop, &QEventLoop::quit);
    fade_out->start(QAbstractAnimation::DeleteWhenStopped);
    fade_loop.exec();

    scrim->deleteLater();
    return result;
}

void OpenPakAccountDialog::OnChangeAvatar() {
#ifdef ENABLE_WEB_SERVICE
    const QString path = QFileDialog::getOpenFileName(this, tr("Choose a profile picture"),
                                                       QString{}, tr("Images (*.png *.jpg *.jpeg)"));
    if (path.isEmpty()) {
        return;
    }

    QImage image(path);
    if (image.isNull()) {
        status->setText(tr("Couldn't read that image."));
        return;
    }
    const int side = std::min(image.width(), image.height());
    const QRect crop_rect((image.width() - side) / 2, (image.height() - side) / 2, side, side);
    const QImage square = image.copy(crop_rect).scaled(256, 256, Qt::KeepAspectRatio,
                                                        Qt::SmoothTransformation);

    QByteArray jpeg_bytes;
    QBuffer buffer(&jpeg_bytes);
    buffer.open(QIODevice::WriteOnly);
    square.save(&buffer, "JPG", 85);
    const std::string image_base64 = jpeg_bytes.toBase64().toStdString();

    header_avatar_source = QPixmap::fromImage(square);
    UpdateHeroSizing();
    status->setText(tr("Uploading profile picture..."));

    std::thread{[this, image_base64, guard = QPointer<OpenPakAccountDialog>(this)] {
        const std::string error = WebService::OpenPakApi::PushProfilePicture(image_base64);
        if (!guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, error] {
                if (!guard) {
                    return;
                }
                status->setText(error.empty() ? tr("Profile picture updated.")
                                             : QString::fromStdString(error));
            },
            Qt::QueuedConnection);
    }}.detach();
#else
    status->setText(tr("This build has no web services support."));
#endif
}

void OpenPakAccountDialog::OnChangeBackground() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Choose a background image"),
                                                       QString{}, tr("Images (*.png *.jpg *.jpeg)"));
    if (path.isEmpty()) {
        return;
    }

    QImage image(path);
    if (image.isNull()) {
        status->setText(tr("Couldn't read that image."));
        return;
    }
    if (image.width() > 1600 || image.height() > 900) {
        image = image.scaled(1600, 900, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    const auto out_path = BackgroundImagePath();
    if (!image.save(QString::fromStdString(out_path.string()), "PNG")) {
        status->setText(tr("Couldn't save that background image."));
        return;
    }

    ApplyBackground(QPixmap::fromImage(image));
    status->setText(tr("Background updated."));
}

void OpenPakAccountDialog::OnRemoveBackground() {
    std::error_code ec;
    std::filesystem::remove(BackgroundImagePath(), ec);
    ApplyBackground({});
    status->setText(tr("Background removed."));
}

void OpenPakAccountDialog::ApplyBackground(const QPixmap& pixmap) {
    static_cast<HeaderCard*>(header_card)->SetBackgroundImage(pixmap);
}

void OpenPakAccountDialog::LoadSavedBackground() {
    const auto path = BackgroundImagePath();
    if (!std::filesystem::exists(path)) {
        return;
    }
    QPixmap pixmap(QString::fromStdString(path.string()));
    if (!pixmap.isNull()) {
        ApplyBackground(pixmap);
    }
}

void OpenPakAccountDialog::OnEditUsername() {
#ifdef ENABLE_WEB_SERVICE
    static const QRegularExpression valid_name(QStringLiteral("^[A-Za-z0-9_-]{3,16}$"));

    bool ok = false;
    const QString name = QInputDialog::getText(
                             this, tr("Change Username"),
                             tr("3-16 characters: letters, digits, '_' or '-'"), QLineEdit::Normal,
                             header_name->text(), &ok)
                             .trimmed();
    if (!ok || name.isEmpty() || name == header_name->text()) {
        return;
    }
    if (!valid_name.match(name).hasMatch()) {
        status->setText(tr("Invalid username."));
        return;
    }

    status->setText(tr("Updating username..."));
    const std::string new_name = name.toStdString();

    std::thread{[this, new_name, guard = QPointer<OpenPakAccountDialog>(this)] {
        const std::string error = WebService::OpenPakApi::SetUsername(new_name);
        if (!guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, error, new_name] {
                if (!guard) {
                    return;
                }
                if (error.empty()) {
                    Common::OpenPakAccount::Save(Common::OpenPakAccount::GetPid(), new_name,
                                                 Common::OpenPakAccount::GetFriendCode(),
                                                 Common::OpenPakAccount::GetToken(),
                                                 Common::OpenPakAccount::GetBearer());
                    header_name->setText(QString::fromStdString(new_name));
                    status->setText(tr("Username updated."));
                } else {
                    status->setText(QString::fromStdString(error));
                }
            },
            Qt::QueuedConnection);
    }}.detach();
#else
    status->setText(tr("This build has no web services support."));
#endif
}

