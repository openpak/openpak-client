// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#include "openpak/qt/pages_p.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include <QApplication>
#include <QDateTime>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QStyle>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include <fmt/format.h>

#include "openpak/account.h"
#include "openpak/compatibility.h"
#include "openpak/qt/account_page_p.h"
#include "openpak/qt/host.h"

namespace openpak::qt {

struct WorkQueue::State {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    bool stop = false;
};

WorkQueue::WorkQueue(QObject* parent) : QObject(parent), state(std::make_shared<State>()) {
    // Detached, holding the state by value: a request still in flight when the window closes
    // finishes on its own and finds nothing to report to, rather than holding the close up.
    std::thread{[state = state] {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock{state->mutex};
                state->wake.wait(lock, [&] { return state->stop || !state->jobs.empty(); });
                if (state->stop) {
                    return;
                }
                job = std::move(state->jobs.front());
                state->jobs.pop_front();
            }
            job();
        }
    }}.detach();
}

WorkQueue::~WorkQueue() {
    {
        std::lock_guard lock{state->mutex};
        state->stop = true;
        state->jobs.clear();
    }
    state->wake.notify_all();
}

void WorkQueue::Post(std::function<void()> job) {
    {
        std::lock_guard lock{state->mutex};
        state->jobs.push_back(std::move(job));
    }
    state->wake.notify_all();
}

void WorkQueue::Started() {
    if (++in_flight == 1 && on_busy) {
        on_busy(true);
    }
}

void WorkQueue::Finished() {
    if (--in_flight == 0 && on_busy) {
        on_busy(false);
    }
}

QString GameName(Host* host, const std::string& title_id_hex) {
    if (title_id_hex.empty()) {
        return {};
    }
    u64 id = 0;
    try {
        id = std::stoull(title_id_hex, nullptr, 16);
    } catch (const std::exception&) {
        return {};
    }
    if (id == 0) {
        return {};
    }
    // The host looks in the library and answers with the hint when the title is not there. A hint
    // that cannot be a name tells "not in the library, not in the catalogue" apart from a real
    // answer, whatever the host would otherwise put in its place.
    static const std::string unknown = "\x01";
    const std::string catalogue = compatibility::CatalogueName(id);
    const QString name = host ? host->ResolveGameName(fmt::format("{:016X}", id),
                                                      catalogue.empty() ? unknown : catalogue)
                              : QString::fromStdString(catalogue);
    const bool is_id = name.size() == 16 &&
                       std::all_of(name.begin(), name.end(), [](QChar c) { return isxdigit(c.toLatin1()); });
    if (name.isEmpty() || name == QString::fromStdString(unknown) || is_id) {
        return {};
    }
    return name;
}

bool SignedIn() {
    return !Common::OpenPakAccount::GetBearer().empty();
}

bool Installed(Host* host, u64 title_id) {
    if (!host || title_id == 0) {
        return false;
    }
    const auto titles = host->InstalledTitles();
    if (!titles.empty()) {
        return std::any_of(titles.begin(), titles.end(),
                           [title_id](const Host::Title& title) { return title.id == title_id; });
    }
    // A host that predates InstalledTitles still finds an installed title's icon.
    return !host->ResolveGameIcon(fmt::format("{:016X}", title_id)).isEmpty();
}

QString ConsoleName(const std::string& name_space) {
    if (name_space.empty()) {
        return {};
    }
    if (name_space == "switch") {
        return QStringLiteral("Nintendo Switch");
    }
    if (name_space == "wiiu") {
        return QStringLiteral("Wii U");
    }
    if (name_space == "3ds") {
        return QStringLiteral("Nintendo 3DS");
    }
    QString name = QString::fromStdString(name_space);
    name[0] = name[0].toUpper();
    return name;
}

QString LocalTime(s64 unix_seconds) {
    if (unix_seconds <= 0) {
        return {};
    }
    return QLocale().toString(QDateTime::fromSecsSinceEpoch(unix_seconds).toLocalTime(),
                              QLocale::ShortFormat);
}

namespace {
QString Span(s64 seconds) {
    if (seconds < 60) {
        return Tr("less than a minute");
    }
    if (seconds < 3600) {
        return QCoreApplication::translate("OpenPakAccountDialog", "%n minute(s)", nullptr,
                                           static_cast<int>(seconds / 60));
    }
    if (seconds < 86400) {
        return QCoreApplication::translate("OpenPakAccountDialog", "%n hour(s)", nullptr,
                                           static_cast<int>(seconds / 3600));
    }
    return QCoreApplication::translate("OpenPakAccountDialog", "%n day(s)", nullptr,
                                       static_cast<int>(seconds / 86400));
}
} // namespace

QString Ago(s64 unix_seconds) {
    if (unix_seconds <= 0) {
        return {};
    }
    const s64 now = QDateTime::currentSecsSinceEpoch();
    return Tr("%1 ago").arg(Span(std::max<s64>(0, now - unix_seconds)));
}

QString Until(s64 unix_seconds) {
    if (unix_seconds <= 0) {
        return {};
    }
    const s64 left = unix_seconds - QDateTime::currentSecsSinceEpoch();
    return left <= 0 ? Tr("expired") : Tr("in %1").arg(Span(left));
}

QString Bytes(u64 size) {
    static const char* const units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(size);
    int unit = 0;
    while (value >= 1024 && unit < 4) {
        value /= 1024;
        ++unit;
    }
    return QStringLiteral("%1 %2").arg(QLocale().toString(value, 'f', value < 10 && unit > 0 ? 1 : 0),
                                       QString::fromLatin1(units[unit]));
}

QPixmap Round(const QPixmap& source, int size) {
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
    // Centre-cropped to a square, so a picture that is not one is not squashed.
    const int side = std::min(source.width(), source.height());
    painter.drawPixmap(QRect(0, 0, size, size), source,
                       QRect((source.width() - side) / 2, (source.height() - side) / 2, side, side));
    return out;
}

QPixmap RoundedRect(const QPixmap& source, int size, int radius) {
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

QPixmap Initials(const QString& name, int size) {
    const qreal dpr = qApp->devicePixelRatio();
    QPixmap out(qMax(1, static_cast<int>(size * dpr)), qMax(1, static_cast<int>(size * dpr)));
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    // A colour per name, so two friends without pictures still look different at a glance.
    const QColor fill = QColor::fromHsl(static_cast<int>(qHash(name) % 360), 110, 110);
    painter.setBrush(fill);
    painter.drawEllipse(0, 0, size, size);
    const QString trimmed = name.trimmed();
    const QString letter = trimmed.isEmpty() ? QStringLiteral("?") : trimmed.left(1).toUpper();
    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(std::max(8, size * 4 / 9));
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(QRect(0, 0, size, size), Qt::AlignCenter, letter);
    return out;
}

void Picture(QWidget* owner, const std::string& key,
             std::function<std::optional<std::vector<u8>>()> fetch,
             std::function<void(const QPixmap&)> show) {
    // UI thread only. A null entry is a miss, remembered so a row without a picture is not asked
    // for again on every refresh.
    static std::map<std::string, QPixmap> cache;
    static std::map<std::string, std::vector<std::function<void(const QPixmap&)>>> waiting;
    if (const auto it = cache.find(key); it != cache.end()) {
        if (!it->second.isNull()) {
            show(it->second);
        }
        return;
    }
    QPointer<QWidget> guard{owner};
    auto shown = [guard, show](const QPixmap& pixmap) {
        if (guard) {
            show(pixmap);
        }
    };
    if (auto it = waiting.find(key); it != waiting.end()) {
        it->second.push_back(std::move(shown));
        return;
    }
    waiting[key].push_back(std::move(shown));
    std::thread{[key, fetch = std::move(fetch)] {
        auto bytes = std::make_shared<std::optional<std::vector<u8>>>(fetch());
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [key, bytes] {
                QPixmap pixmap;
                if (*bytes && !(*bytes)->empty()) {
                    pixmap.loadFromData((*bytes)->data(), static_cast<uint>((*bytes)->size()));
                }
                cache[key] = pixmap;
                auto callbacks = std::move(waiting[key]);
                waiting.erase(key);
                if (!pixmap.isNull()) {
                    for (const auto& callback : callbacks) {
                        callback(pixmap);
                    }
                }
            },
            Qt::QueuedConnection);
    }}.detach();
}

void TitleIcon(QWidget* owner, Host* host, const std::string& title_id_hex,
               std::function<void(const QPixmap&)> show) {
    if (title_id_hex.empty()) {
        return;
    }
    std::string upper = title_id_hex;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (host) {
        const QString local = host->ResolveGameIcon(upper);
        if (!local.isEmpty()) {
            QPixmap pixmap;
            if (pixmap.loadFromData(QByteArray::fromBase64(local.toUtf8()))) {
                show(pixmap);
                return;
            }
        }
    }
    Picture(owner, "title:" + upper, [upper] { return WebService::OpenPakApi::GetTitleIcon(upper); },
            std::move(show));
}

QLabel* DimLabel(const QString& text) {
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, style::Dim());
    label->setPalette(palette);
    return label;
}

QLabel* BoldLabel(const QString& text, int grow) {
    auto* label = new QLabel(text);
    QFont font = label->font();
    font.setBold(true);
    font.setPointSize(font.pointSize() + grow);
    label->setFont(font);
    return label;
}

QPushButton* TextButton(const QString& text) {
    auto* button = new QPushButton(text);
    button->setCursor(Qt::PointingHandCursor);
    button->setMinimumHeight(30);
    return button;
}

QToolButton* IconButton(const QString& glyph, const QString& tooltip) {
    auto* button = new QToolButton;
    button->setText(glyph);
    button->setToolTip(tooltip);
    button->setAccessibleName(tooltip);
    button->setCursor(Qt::PointingHandCursor);
    button->setAutoRaise(true);
    button->setMinimumSize(30, 30);
    return button;
}

ClickCard::ClickCard(QWidget* parent) : QFrame(parent) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
}

void ClickCard::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && on_click && rect().contains(event->pos())) {
        on_click();
    }
    QFrame::mouseReleaseEvent(event);
}

void ClickCard::keyPressEvent(QKeyEvent* event) {
    if ((event->key() == Qt::Key_Space || event->key() == Qt::Key_Return ||
         event->key() == Qt::Key_Enter) &&
        on_click) {
        on_click();
        return;
    }
    QFrame::keyPressEvent(event);
}

void ClickCard::paintEvent(QPaintEvent* event) {
    QFrame::paintEvent(event);
    if (!hasFocus()) {
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QApplication::palette().color(QPalette::Highlight), 2));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 10, 10);
}

RowList::RowList() {
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setStyleSheet(QStringLiteral("QScrollArea { background: transparent; }"));
    auto* inner = new QWidget;
    inner->setObjectName(QStringLiteral("rows"));
    inner->setStyleSheet(QStringLiteral("QWidget#rows { background: transparent; }"));
    rows = new QVBoxLayout(inner);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(6);
    rows->addStretch(1);
    setWidget(inner);
}

void RowList::Clear() {
    while (rows->count() > 1) {
        QLayoutItem* item = rows->takeAt(0);
        delete item->widget();
        delete item;
    }
}

void RowList::Add(QWidget* row) {
    rows->insertWidget(rows->count() - 1, row);
}

Page* NotHerePage(const QString& text, const QString& link_text, const QString& url) {
    auto* page = new Page;
    auto* layout = new QVBoxLayout(page);
    layout->addStretch(1);
    auto* icon = new QLabel;
    icon->setPixmap(page->style()->standardIcon(QStyle::SP_MessageBoxInformation).pixmap(40, 40));
    icon->setAlignment(Qt::AlignCenter);
    layout->addWidget(icon);
    QLabel* words = DimLabel(text);
    words->setAlignment(Qt::AlignCenter);
    words->setWordWrap(true);
    layout->addWidget(words);
    if (!link_text.isEmpty() && !url.isEmpty()) {
        auto* link = new QLabel(QStringLiteral("<a href=\"%1\">%2</a>").arg(url, link_text.toHtmlEscaped()));
        link->setAlignment(Qt::AlignCenter);
        link->setOpenExternalLinks(true);
        layout->addWidget(link);
    }
    layout->addStretch(1);
    page->reload = [](bool) {};
    return page;
}

void RowList::AddEmpty(const QString& text) {
    QLabel* label = DimLabel(text);
    label->setAlignment(Qt::AlignCenter);
    label->setContentsMargins(0, 24, 0, 24);
    Add(label);
}

void RowList::AddHeading(const QString& text) {
    QLabel* label = BoldLabel(text, 1);
    label->setContentsMargins(2, 8, 0, 2);
    Add(label);
}

QFrame* RowList::AddRow(const QString& title, const QStringList& lines,
                        const std::vector<QWidget*>& buttons, const QString& trailing,
                        QLabel** picture) {
    auto* row = new QFrame;
    row->setObjectName(QStringLiteral("row"));
    row->setStyleSheet(style::Card());
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(14, 8, 14, 8);
    layout->setSpacing(10);

    if (picture) {
        *picture = new QLabel;
        (*picture)->setFixedSize(42, 42);
        (*picture)->setAlignment(Qt::AlignCenter);
        layout->addWidget(*picture, 0, Qt::AlignTop);
    }

    auto* text = new QVBoxLayout;
    text->setSpacing(2);
    QLabel* name = BoldLabel(title);
    name->setWordWrap(true);
    text->addWidget(name);
    for (const QString& line : lines) {
        if (!line.isEmpty()) {
            text->addWidget(DimLabel(line));
        }
    }
    layout->addLayout(text, 1);

    if (!trailing.isEmpty()) {
        layout->addWidget(DimLabel(trailing));
    }
    for (QWidget* button : buttons) {
        layout->addWidget(button);
    }
    Add(row);
    return row;
}

} // namespace openpak::qt
