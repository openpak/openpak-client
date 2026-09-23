// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

// The Friends page, as the reference's: requests in both directions above the friends, a friend
// as a card that opens to show what they are playing and since when, and every action by the
// account id the website keys friends on.

#include <memory>
#include <set>

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QToolButton>
#include <QVBoxLayout>

#include "openpak/account.h"
#include "openpak/api.h"
#include "openpak/compatibility.h"
#include "openpak/qt/account_page_p.h"
#include "openpak/qt/extra_pages.h"
#include "openpak/qt/host.h"
#include "openpak/qt/pages_p.h"

namespace openpak::qt {

namespace Api = WebService::OpenPakApi;

namespace {

constexpr int kAvatar = 34;

// The avatar with its presence dot in the corner, green when online.
QPixmap WithDot(const QPixmap& avatar, bool online) {
    QPixmap out = avatar;
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(24, 24, 28), 2));
    painter.setBrush(online ? QColor(0x3F, 0xB9, 0x50) : QColor(0x7A, 0x7A, 0x7A));
    painter.drawEllipse(QRectF(kAvatar - 12, kAvatar - 12, 11, 11));
    return out;
}

QString StatusOf(const Api::AccountFriend& entry, const QString& game) {
    if (!entry.online) {
        return Tr("Offline");
    }
    if (!entry.title_id.empty()) {
        return game.isEmpty() ? Tr("Playing") : Tr("Playing %1").arg(game);
    }
    const QString console = ConsoleName(entry.console_namespace);
    const bool elsewhere = !console.isEmpty() && console != QStringLiteral("Nintendo Switch");
    return elsewhere ? Tr("On %1").arg(console) : Tr("Online");
}

// A friend's picture on a label: initials at once, the site's avatar when it arrives.
void ShowAvatar(QLabel* label, const Api::AccountFriend& entry, int size, bool dot) {
    const QString name = QString::fromStdString(entry.display_name);
    const auto draw = [label, size, dot, online = entry.online](const QPixmap& pixmap) {
        const QPixmap round = Round(pixmap, size);
        label->setPixmap(dot ? WithDot(round, online) : round);
    };
    const QPixmap initials = Initials(name, size);
    label->setPixmap(dot ? WithDot(initials, entry.online) : initials);
    if (!entry.account_id.empty()) {
        Picture(label, "avatar:" + entry.account_id,
                [id = entry.account_id] { return Api::GetAccountAvatar(id); }, draw);
    }
}

} // namespace

Page* MakeFriendsPage(const PageContext& context) {
    auto* page = new Page;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(10);

    auto* header = new QHBoxLayout;
    auto* code = new QLineEdit;
    code->setPlaceholderText(QStringLiteral("SW-0000-0000-0000"));
    QPushButton* add = TextButton(Tr("Add"));
    add->setMinimumWidth(90);
    QPushButton* refresh = TextButton(Tr("Refresh"));
    header->addWidget(code, 1);
    header->addWidget(add);
    header->addWidget(refresh);
    layout->addLayout(header);

    // Wii and DS games keep their own friend codes: OpenPak's list is shown for presence only,
    // and managed on the website (UX spec §3.6).
    const bool read_only = IsWfc(context.host->GetFamily());
    if (read_only) {
        code->hide();
        add->hide();
        auto* note = DimLabel(Tr("These are your OpenPak friends. Wii and DS games keep their own "
                                 "friend codes."));
        note->setWordWrap(true);
        auto* manage = new QLabel(QStringLiteral("<a href=\"%1/friends\">%2</a>")
                                      .arg(QString::fromStdString(Api::BaseUrl()),
                                           Tr("Manage friends on openpak.org")));
        manage->setOpenExternalLinks(true);
        header->insertWidget(0, note, 1);
        header->insertWidget(1, manage);
    }

    auto* list = new RowList;
    layout->addWidget(list, 1);

    // Which cards are open, by account id (or pid for a row without one), so a refresh -- the
    // automatic one every 30 seconds included -- does not fold up what somebody is reading.
    auto expanded = std::make_shared<std::set<std::string>>();
    const auto key_of = [](const Api::AccountFriend& entry) {
        return entry.account_id.empty() ? std::to_string(entry.pid) : entry.account_id;
    };

    // A change to the friend graph, as the reference's MutateAsync: the server's reason or
    // nothing in the status line, then everything reloaded. Friends of the guest's friend
    // service come from the host's cache, so that is refreshed too.
    const auto mutate = [page, context](std::function<std::string()> work, QString done_text = {}) {
        context.queue->Run(page, std::move(work), [page, context, done_text](std::string failure) {
            context.status(failure.empty() ? done_text : QString::fromStdString(failure));
            if (failure.empty() && context.host) {
                context.host->RefreshFriendCache();
            }
            page->reload(true);
        });
    };

    const auto add_friend = [=] {
        const std::string wanted = code->text().trimmed().toStdString();
        if (wanted.empty()) {
            context.status(Tr("Type a friend code first."));
            return;
        }
        context.queue->Run(page, [wanted] { return Api::SendFriendRequest(wanted); },
                           [=](std::string failure) {
                               if (!failure.empty()) {
                                   context.status(QString::fromStdString(failure));
                                   return;
                               }
                               context.status(Tr("Asked %1 to be your friend.")
                                                  .arg(QString::fromStdString(wanted)));
                               code->clear();
                               if (context.host) {
                                   context.host->NotifyFriendRequestSent(QString::fromStdString(wanted));
                               }
                               page->reload(true);
                           });
    };
    QObject::connect(add, &QPushButton::clicked, page, add_friend);
    QObject::connect(code, &QLineEdit::returnPressed, page, add_friend);

    const auto request_row = [=](const Api::AccountFriend& request) {
        std::vector<QWidget*> buttons;
        if (request.incoming) {
            QToolButton* accept = IconButton(QStringLiteral("\xE2\x9C\x93"), Tr("Accept"));
            QObject::connect(accept, &QToolButton::clicked, page, [=] {
                mutate([request] { return Api::AcceptFriendRequest(request); });
            });
            buttons.push_back(accept);
        }
        QToolButton* dismiss = IconButton(QStringLiteral("\xE2\x9C\x95"),
                                          request.incoming ? Tr("Decline") : Tr("Cancel request"));
        QObject::connect(dismiss, &QToolButton::clicked, page, [=] {
            mutate([request] { return Api::DeclineFriendRequest(request); });
        });
        buttons.push_back(dismiss);

        QLabel* picture = nullptr;
        list->AddRow(QString::fromStdString(request.display_name),
                     {request.incoming ? Tr("Wants to be your friend")
                                       : Tr("Waiting for them to accept")},
                     buttons, {}, &picture);
        picture->setFixedSize(kAvatar, kAvatar);
        ShowAvatar(picture, request, kAvatar, false);
    };

    const auto friend_card = [=](const Api::AccountFriend& entry) {
        const QString name = QString::fromStdString(entry.display_name);
        const QString game = GameName(context.host, entry.title_id);

        auto* card = new ClickCard;
        card->setObjectName(QStringLiteral("row"));
        card->setStyleSheet(style::Card());
        auto* card_layout = new QVBoxLayout(card);
        card_layout->setContentsMargins(14, 8, 14, 8);
        card_layout->setSpacing(6);

        auto* top = new QHBoxLayout;
        top->setSpacing(10);
        auto* avatar = new QLabel;
        avatar->setFixedSize(kAvatar, kAvatar);
        ShowAvatar(avatar, entry, kAvatar, true);
        top->addWidget(avatar);
        auto* text = new QVBoxLayout;
        text->setSpacing(0);
        text->addWidget(BoldLabel(name));
        text->addWidget(DimLabel(StatusOf(entry, game)));
        top->addLayout(text, 1);

        QToolButton* remove = IconButton(QStringLiteral("\xF0\x9F\x97\x91"), Tr("Remove"));
        QToolButton* block = IconButton(QStringLiteral("\xE2\x9B\x94"), Tr("Block"));
        remove->setEnabled(!entry.account_id.empty());
        block->setEnabled(!entry.account_id.empty());
        QObject::connect(remove, &QToolButton::clicked, page, [=] {
            if (context.confirm(Tr("Remove Friend"), Tr("Remove %1 from your friends?").arg(name),
                                Tr("Remove"))) {
                mutate([id = entry.account_id] { return Api::RemoveAccountFriend(id); });
            }
        });
        QObject::connect(block, &QToolButton::clicked, page, [=] {
            if (context.confirm(Tr("Block"),
                                Tr("Block %1? They will not appear on your list, cannot send you "
                                   "anything, and cannot see you.")
                                    .arg(name),
                                Tr("Block"))) {
                mutate([id = entry.account_id] { return Api::BlockAccount(id); });
            }
        });
        top->addWidget(remove);
        top->addWidget(block);
        remove->setVisible(!read_only);
        block->setVisible(!read_only);
        auto* chevron = DimLabel({});
        top->addWidget(chevron);
        card_layout->addLayout(top);

        // What the card opens to: the game with its box art, where and since when, and the
        // friendship itself.
        auto* details = new QWidget;
        auto* details_layout = new QHBoxLayout(details);
        details_layout->setContentsMargins(kAvatar + 12, 0, 0, 0);
        details_layout->setSpacing(10);
        if (!game.isEmpty()) {
            auto* art = new QLabel;
            art->setFixedSize(48, 48);
            details_layout->addWidget(art, 0, Qt::AlignTop);
            TitleIcon(art, context.host, entry.title_id,
                      [art](const QPixmap& pixmap) { art->setPixmap(RoundedRect(pixmap, 48, 6)); });
        }
        auto* lines = new QVBoxLayout;
        lines->setSpacing(2);
        if (!game.isEmpty()) {
            lines->addWidget(BoldLabel(game));
        }
        const QString console = ConsoleName(entry.console_namespace);
        if (!console.isEmpty()) {
            lines->addWidget(DimLabel(Tr("On %1").arg(console)));
        }
        if (entry.online_since > 0) {
            lines->addWidget(DimLabel(Tr("Online since %1").arg(LocalTime(entry.online_since))));
        }
        if (!entry.friend_code.empty()) {
            auto* code_line = DimLabel(Tr("Friend code: %1").arg(QString::fromStdString(entry.friend_code)));
            code_line->setTextInteractionFlags(Qt::TextSelectableByMouse);
            lines->addWidget(code_line);
        }
        if (entry.friends_since > 0) {
            lines->addWidget(DimLabel(Tr("Friends since %1").arg(LocalTime(entry.friends_since))));
        }
        if (lines->count() == 0) {
            lines->addWidget(DimLabel(Tr("Nothing more to show.")));
        }
        details_layout->addLayout(lines, 1);
        card_layout->addWidget(details);

        const std::string key = key_of(entry);
        const auto show_open = [details, chevron](bool open) {
            details->setVisible(open);
            chevron->setText(open ? QStringLiteral("\xE2\x96\xB4") : QStringLiteral("\xE2\x96\xBE"));
        };
        show_open(expanded->contains(key));
        card->on_click = [=] {
            const bool open = !expanded->contains(key);
            if (open) {
                expanded->insert(key);
            } else {
                expanded->erase(key);
            }
            show_open(open);
        };

        // What the old list offered beside the reference's actions: joining a friend's session
        // and a chat invitation, both keyed by the pid only the adapter's rows carry.
        card->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(card, &QWidget::customContextMenuRequested, card, [=](const QPoint& at) {
            if (read_only || entry.pid == 0 || !context.host) {
                return;
            }
            QMenu menu(card);
            if (entry.online && !entry.title_id.empty()) {
                menu.addAction(Tr("Join Game"), card, [=] {
                    QMessageBox::information(card, Tr("Join Game"),
                                             context.host->JoinFriendSession(entry.pid));
                });
            }
            if (context.invite_to_chat) {
                menu.addAction(Tr("Invite to Chat Room"), card,
                               [=] { context.invite_to_chat(entry.pid, name); });
            }
            if (!menu.isEmpty()) {
                menu.exec(card->mapToGlobal(at));
            }
        });
        list->Add(card);
    };

    page->reload = [=](bool) {
        if (!SignedIn()) {
            list->Clear();
            list->AddEmpty(Tr("Sign in to OpenPak to use this."));
            *context.friends = {};
            if (context.friends_changed) {
                context.friends_changed();
            }
            return;
        }
        context.queue->Run(
            page,
            [] {
                compatibility::LoadNames();
                return Api::GetAccountFriends();
            },
            [=](Api::AccountFriends fetched) {
                if (!fetched.ok) {
                    context.status(QString::fromStdString(fetched.error));
                    return;
                }
                const int scrolled = list->verticalScrollBar()->value();
                list->Clear();
                if (!read_only && !fetched.requests.empty()) {
                    list->AddHeading(Tr("Requests"));
                    for (const auto& request : fetched.requests) {
                        request_row(request);
                    }
                }
                list->AddHeading(Tr("Friends"));
                if (fetched.friends.empty()) {
                    list->AddEmpty(read_only ? Tr("No friends yet.")
                                             : Tr("No friends yet. Add someone by their friend code."));
                }
                for (const auto& entry : fetched.friends) {
                    friend_card(entry);
                }
                list->verticalScrollBar()->setValue(scrolled);

                *context.friends = std::move(fetched);
                if (context.friends_changed) {
                    context.friends_changed();
                }
            });
    };
    QObject::connect(refresh, &QPushButton::clicked, page, [page] { page->reload(true); });
    return page;
}

} // namespace openpak::qt
