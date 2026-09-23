// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

// What the account window's pages share: the one queue their network work goes through, the
// context the dialog hands each of them, and the small pieces every page draws with (names,
// pictures, rows). Private to the library.

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QFrame>
#include <QPixmap>
#include <QPointer>
#include <QScrollArea>
#include <QString>
#include <QWidget>

#include "openpak/api.h"
#include "openpak/types.h"

class QLabel;
class QPushButton;
class QToolButton;
class QVBoxLayout;

namespace openpak::qt {

class Host;

// The window's network work, one job at a time in the order it was asked for, as the reference's
// Guarded gate runs it: a page that asks for two things on the way in gets both, and a refresh
// asked for while another is running waits its turn instead of being dropped. The work runs on
// the queue's own thread; its result comes back on the UI thread, and only while both the owner
// and the queue are still alive. Closing the window drops what has not started.
class WorkQueue : public QObject {
public:
    explicit WorkQueue(QObject* parent);
    ~WorkQueue() override;

    template <typename Work, typename Done>
    void Run(QObject* owner, Work work, Done done) {
        Started();
        QPointer<WorkQueue> self{this};
        QPointer<QObject> guard{owner};
        Post([self, guard, work = std::move(work), done = std::move(done)]() mutable {
            auto result = std::make_shared<decltype(work())>(work());
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [self, guard, done, result]() mutable {
                    if (!self) {
                        return;
                    }
                    if (guard) {
                        done(std::move(*result));
                    }
                    self->Finished();
                },
                Qt::QueuedConnection);
        });
    }

    // Told true when the first job is queued and false when the last one is done.
    std::function<void(bool)> on_busy;

private:
    struct State;
    void Post(std::function<void()> job);
    void Started();
    void Finished();

    std::shared_ptr<State> state;
    int in_flight = 0;
};

// Everything a page needs from the window around it.
struct PageContext {
    Host* host = nullptr;
    WorkQueue* queue = nullptr;
    std::function<void(const QString&)> status;
    // The window's own confirmation card; true when the person said yes.
    std::function<bool(const QString& title, const QString& message, const QString& yes)> confirm;
    // Closes the window, which the reference does before it launches a game.
    std::function<void()> close;
    // What the Friends page last drew, for the pages that name or picture a sender and for the
    // Account page's counts.
    std::shared_ptr<WebService::OpenPakApi::AccountFriends> friends;
    // The Friends page reports what it drew: the dashboard counts and the requests badge.
    std::function<void()> friends_changed;
    std::function<void(u64 pid, const QString& name)> invite_to_chat;
    // The title Cloud saves, Mods and News have picked, as 16 lower-case hex digits: one choice
    // shared by the three, as the reference shares its SelectedTitle.
    std::shared_ptr<std::string> title;
};

// Signed in to the website, which is what every page but Status needs; a console link is not.
bool SignedIn();

// Whether the host's library has the title (the host's list, else an icon it can find).
bool Installed(Host* host, u64 title_id);

// A page reloads itself when asked; force refetches even what it already shows (the Refresh
// button), otherwise a page may skip a fetch that would change nothing.
class Page : public QWidget {
public:
    std::function<void(bool force)> reload;
};

QString Tr(const char* text);

// A title's name for people: the local library's, then the site catalogue's; empty when neither
// knows it. Never the title id. The catalogue's names come from compatibility::LoadNames, which
// the page's work calls before it names anything.
QString GameName(Host* host, const std::string& title_id_hex);

// "Nintendo Switch", "Wii U"... for an adapter namespace; empty for none.
QString ConsoleName(const std::string& name_space);

// Local time as the reference words it, and how long ago or until.
QString LocalTime(s64 unix_seconds);
QString Ago(s64 unix_seconds);
QString Until(s64 unix_seconds);
QString Bytes(u64 size);

// A circle with a name's first letter, for anyone without a picture.
QPixmap Initials(const QString& name, int size);
QPixmap Round(const QPixmap& source, int size);
QPixmap RoundedRect(const QPixmap& source, int size, int radius);

// A picture fetched once per run and remembered (misses too), shown when it arrives. Pictures
// load in parallel, beside the queue rather than through it, as the reference loads them: a row
// never waits on another row's avatar. fetch runs off the UI thread; show runs on it, only while
// owner is alive.
void Picture(QWidget* owner, const std::string& key,
             std::function<std::optional<std::vector<u8>>()> fetch,
             std::function<void(const QPixmap&)> show);

// A title's icon: the library's, else the site's; show is called with it when there is one.
void TitleIcon(QWidget* owner, Host* host, const std::string& title_id_hex,
               std::function<void(const QPixmap&)> show);

QLabel* DimLabel(const QString& text);
QLabel* BoldLabel(const QString& text, int grow = 0);
QPushButton* TextButton(const QString& text);
QToolButton* IconButton(const QString& glyph, const QString& tooltip);

// A frame that toggles or activates on a click, Space or Enter, for the controller's A button.
class ClickCard : public QFrame {
public:
    explicit ClickCard(QWidget* parent = nullptr);
    std::function<void()> on_click;

protected:
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
};

// A scrolling column of widgets with a stretch at the bottom.
class RowList : public QScrollArea {
public:
    RowList();
    void Clear();
    void Add(QWidget* row);
    void AddEmpty(const QString& text);
    void AddHeading(const QString& text);
    // A card row: a bold title, dim lines under it, a trailing note and buttons on the right.
    QFrame* AddRow(const QString& title, const QStringList& lines,
                   const std::vector<QWidget*>& buttons, const QString& trailing = {},
                   QLabel** picture = nullptr);

private:
    QVBoxLayout* rows;
};

} // namespace openpak::qt
