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
#include "openpak/qt/strings.h"
#include "openpak/types.h"

class QIcon;
class QLabel;
class QPushButton;
class QToolButton;
class QVBoxLayout;

namespace openpak::qt {

class Host;

// The window's network work, one job at a time in the order it was asked for, as the earlier port's
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
    // Closes the window, which the earlier port does before it launches a game.
    std::function<void()> close;
    // Opens the window at another page (OpenPakAccountDialog::k*Page).
    std::function<void(int page)> go_to;
    // What the Friends page last drew, for the pages that name or picture a sender and for the
    // Account page's counts.
    std::shared_ptr<WebService::OpenPakApi::AccountFriends> friends;
    // The Friends page reports what it drew: the dashboard counts and the requests badge.
    std::function<void()> friends_changed;
    std::function<void(u64 pid, const QString& name)> invite_to_chat;
    // The title Cloud saves, Mods and News have picked, as 16 lower-case hex digits: one choice
    // shared by the three, as the earlier port shares its SelectedTitle.
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

// A page that cannot apply to the host's console family (UX spec §3.6): the reason, centred, with
// a link under it when there is somewhere to go instead. The page count and order stay the same
// on every family, so this stands where the page would be.
Page* NotHerePage(const QString& text, const QString& link_text = {}, const QString& url = {});

// A title's name for people: the local library's, then the site catalogue's; empty when neither
// knows it. Never the title id. The catalogue's names come from compatibility::LoadNames, which
// the page's work calls before it names anything.
QString GameName(Host* host, const std::string& title_id_hex);

// "Nintendo Switch", "Wii U"... for an adapter namespace; empty for none.
QString ConsoleName(const std::string& name_space);

// Local time in the locale's short date and time format, the one way every time is shown (UX
// spec §5.4); empty for none. LocalTimeIso reads the ISO 8601 text the website sends.
QString LocalTime(s64 unix_seconds);
QString LocalTimeIso(const std::string& iso8601);
s64 SecondsIso(const std::string& iso8601);
QString Bytes(u64 size);

// The icon a page, a setting or an action wears everywhere (UX spec §6): the desktop theme's,
// else the closest one the style has. Names: account, friends, invitations, saves, mods, news,
// status, settings, website, sign-out, accept, decline, remove, block, edit, picture, copy.
QIcon Icon(const char* name);

// A circle with a name's first letter, for anyone without a picture.
QPixmap Initials(const QString& name, int size);
QPixmap Round(const QPixmap& source, int size);
QPixmap RoundedRect(const QPixmap& source, int size, int radius);

// A picture fetched once per run and remembered (misses too), shown when it arrives. Pictures
// load in parallel, beside the queue rather than through it, as the earlier port loads them: a row
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
// An icon button whose tooltip and accessible name are its words (never an emoji glyph).
QToolButton* IconButton(const char* icon, const QString& tooltip);

// The page body a signed-out person sees where a page needs an account: the words, centred, and
// Sign in... (UX spec §3.6).
QWidget* NeedSignIn(Host* host);

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
    // common.need_sign_in with Sign in..., in place of a page's rows.
    void AddNeedSignIn(Host* host);
    void AddHeading(const QString& text);
    // A card row: a bold title, dim lines under it, a trailing note and buttons on the right.
    QFrame* AddRow(const QString& title, const QStringList& lines,
                   const std::vector<QWidget*>& buttons, const QString& trailing = {},
                   QLabel** picture = nullptr);

private:
    QVBoxLayout* rows;
};

} // namespace openpak::qt
