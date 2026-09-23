// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#include "openpak/qt/prompts.h"

#include <algorithm>
#include <functional>

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include "openpak/api.h"
#include "openpak/qt/controller_navigation.h"
#include "openpak/qt/pages_p.h"

namespace openpak::qt {

namespace {

// Create an account, as a link (UX spec §2): it opens the website's registration, focusable so
// the controller's A follows it too.
QLabel* CreateAccountLink(QWidget* owner, std::function<void()> after = {}) {
    auto* link = new QLabel(QStringLiteral("<a href=\"register\">%1</a>")
                                .arg(Tr("Create an account").toHtmlEscaped()));
    link->setFocusPolicy(Qt::StrongFocus);
    link->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    QObject::connect(link, &QLabel::linkActivated, owner, [after] {
        QDesktopServices::openUrl(QUrl(QString::fromStdString(WebService::OpenPakApi::BaseUrl()) +
                                       QStringLiteral("/register")));
        if (after) {
            after();
        }
    });
    return link;
}

QString When(s64 unix_seconds) {
    return unix_seconds > 0 ? LocalTime(unix_seconds) : Tr("—");
}

// Runs a prompt modally with the controller driving it: the stick or D-pad moves the focus (up
// and down through a list when there is one), A presses what has the focus or toggles a row, B is
// Escape. The game's input is suspended while it is up (UX spec §5.6).
int Run(QDialog& dialog, QListWidget* list = nullptr) {
    Host* host = Host::Current();
    Navigation* navigation = host ? host->CreateNavigation(&dialog) : nullptr;
    if (navigation) {
        QObject::connect(navigation, &Navigation::navigated, &dialog, [&dialog, list](int dx, int dy) {
            QWidget* focused = QApplication::focusWidget();
            if (list && focused == list && dy != 0) {
                const int row = list->currentRow() + dy;
                if (row >= 0 && row < list->count()) {
                    list->setCurrentRow(row);
                    return;
                }
            }
            // Tab and Backtab, which move the focus as they do with a keyboard.
            QWidget* target = focused && dialog.isAncestorOf(focused) ? focused : &dialog;
            if (dx > 0 || dy > 0) {
                QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
                QApplication::sendEvent(target, &tab);
            } else if (dx < 0 || dy < 0) {
                QKeyEvent backtab(QEvent::KeyPress, Qt::Key_Backtab, Qt::ShiftModifier);
                QApplication::sendEvent(target, &backtab);
            }
        });
        QObject::connect(
            navigation, &Navigation::activated, &dialog,
            [&dialog, list] {
                QWidget* focused = QApplication::focusWidget();
                if (!focused || !dialog.isAncestorOf(focused)) {
                    return;
                }
                if (auto* button = qobject_cast<QAbstractButton*>(focused)) {
                    button->click();
                } else if (list && focused == list) {
                    dialog.accept();
                } else if (auto* label = qobject_cast<QLabel*>(focused)) {
                    // A focused link: the one link these prompts carry.
                    emit label->linkActivated(QStringLiteral("register"));
                }
            },
            Qt::QueuedConnection);
        QObject::connect(navigation, &Navigation::backPressed, &dialog, &QDialog::reject);
    }
    if (host) {
        host->SetGuestInputSuspended(true);
    }
    const int result = dialog.exec();
    if (host) {
        host->SetGuestInputSuspended(false);
    }
    return result;
}

QLabel* Body(const QString& text) {
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    label->setMinimumWidth(380);
    return label;
}

QPixmap Shaped(const QPixmap& picture, const QString& name, int size, bool round) {
    const QPixmap source = picture.isNull() ? Initials(name, size) : picture;
    return round ? Round(source, size) : RoundedRect(source, size, 6);
}

QLabel* PictureLabel(const QPixmap& picture, const QString& name, int size, bool round) {
    auto* label = new QLabel;
    label->setFixedSize(size, size);
    label->setPixmap(Shaped(picture, name, size, round));
    return label;
}

} // namespace

bool ConfirmSignOut(QWidget* parent, const QString& profile) {
    QDialog dialog(parent);
    dialog.setWindowTitle(Tr("Sign out of OpenPak?"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(12);
    layout->addWidget(BoldLabel(Tr("Sign out of OpenPak?"), 2));
    layout->addWidget(Body(
        profile.isEmpty()
            ? Tr("This emulator goes offline. Your friends and cloud saves stay on your account, "
                 "and you can sign in again at any time.")
            : Tr("%1 goes offline on this emulator. Your friends, invitations and cloud saves "
                 "stay on your account, and you can sign in again at any time.")
                  .arg(profile)));
    auto* buttons = new QDialogButtonBox;
    QPushButton* sign_out = buttons->addButton(Tr("Sign out"), QDialogButtonBox::DestructiveRole);
    QPushButton* cancel = buttons->addButton(Tr("Cancel"), QDialogButtonBox::RejectRole);
    sign_out->setAutoDefault(false);
    cancel->setDefault(true);
    cancel->setFocus();
    QObject::connect(sign_out, &QPushButton::clicked, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    return Run(dialog) == QDialog::Accepted;
}

bool AskToConnect(QWidget* parent, Family family) {
    QString body;
    switch (family) {
    case Family::WiiU:
        body = Tr("Sign in to OpenPak to play Wii U games online without a console dump: friends "
                  "and cloud saves come with it. You can sign in later from the OpenPak menu.");
        break;
    case Family::N3DS:
        body = Tr("Sign in to OpenPak for your friend list and cloud saves. You can sign in later "
                  "from the OpenPak menu.");
        break;
    default:
        body = Tr("Sign in to OpenPak to play Nintendo Wi-Fi Connection games online and keep your "
                  "saves in the cloud. Games keep their own friend codes. You can sign in later "
                  "from the OpenPak menu.");
        break;
    }

    QDialog dialog(parent);
    dialog.setWindowTitle(Tr("Connect to OpenPak?"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(12);
    layout->addWidget(BoldLabel(Tr("Connect to OpenPak?"), 2));
    layout->addWidget(Body(body));
    layout->addWidget(CreateAccountLink(&dialog));

    auto* buttons = new QDialogButtonBox;
    QPushButton* sign_in = buttons->addButton(Tr("Sign in"), QDialogButtonBox::AcceptRole);
    buttons->addButton(Tr("Not now"), QDialogButtonBox::RejectRole);
    sign_in->setDefault(true);
    sign_in->setFocus();
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    return Run(dialog) == QDialog::Accepted;
}

SetupChoice AskSetup(QWidget* parent, bool add_mode, bool keychain) {
    QDialog dialog(parent);
    const QString title = add_mode ? Tr("Add an account") : Tr("Set up this profile");
    dialog.setWindowTitle(title);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(12);
    layout->addWidget(BoldLabel(title, 2));
    layout->addWidget(Body(Tr("Sign in to OpenPak to play online: friends, invitations and cloud "
                              "saves follow this profile. Or keep it offline. You can sign in "
                              "later from the OpenPak menu.")));
    if (!keychain) {
        layout->addWidget(Body(Tr("There is no password store on this machine, so there is nowhere "
                                  "safe to keep the account token. Install and unlock one, then "
                                  "sign in again.")));
    }

    SetupChoice choice = add_mode ? SetupChoice::Cancel : SetupChoice::Offline;
    // The link keeps the dialog at two buttons (three adding one), as every toolkit can show it.
    layout->addWidget(CreateAccountLink(&dialog, [&dialog, &choice] {
        choice = SetupChoice::Create;
        dialog.accept();
    }));

    auto* buttons = new QDialogButtonBox;
    QPushButton* sign_in = buttons->addButton(Tr("Sign in with OpenPak"), QDialogButtonBox::AcceptRole);
    QPushButton* offline = buttons->addButton(Tr("Play offline"),
                                              add_mode ? QDialogButtonBox::ActionRole
                                                       : QDialogButtonBox::RejectRole);
    QPushButton* cancel =
        add_mode ? buttons->addButton(Tr("Cancel"), QDialogButtonBox::RejectRole) : nullptr;
    sign_in->setEnabled(keychain);
    sign_in->setDefault(keychain);
    (keychain ? sign_in : offline)->setFocus();
    QObject::connect(sign_in, &QPushButton::clicked, &dialog, [&dialog, &choice] {
        choice = SetupChoice::SignIn;
        dialog.accept();
    });
    QObject::connect(offline, &QPushButton::clicked, &dialog, [&dialog, &choice] {
        choice = SetupChoice::Offline;
        dialog.accept();
    });
    if (cancel) {
        QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    }
    layout->addWidget(buttons);

    // Escape: Play offline on first run, Cancel adding one -- the initial choice above.
    if (Run(dialog) != QDialog::Accepted) {
        return add_mode ? SetupChoice::Cancel : SetupChoice::Offline;
    }
    return choice;
}

QString AskProfileName(QWidget* parent, const QString& current) {
    QDialog dialog(parent);
    dialog.setWindowTitle(Tr("Profile name"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(12);
    auto* field = new QLineEdit(current);
    field->setAccessibleName(Tr("Profile name"));
    field->setMaxLength(31); // what a Switch profile keeps
    field->selectAll();
    layout->addWidget(BoldLabel(Tr("Profile name")));
    layout->addWidget(field);
    auto* buttons = new QDialogButtonBox;
    QPushButton* offline = buttons->addButton(Tr("Play offline"), QDialogButtonBox::AcceptRole);
    buttons->addButton(Tr("Cancel"), QDialogButtonBox::RejectRole);
    offline->setDefault(true);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    field->setFocus();
    if (Run(dialog) != QDialog::Accepted) {
        return {};
    }
    return field->text().trimmed();
}

ProfileChoice PickProfile(QWidget* parent, const std::vector<ProfileRow>& rows,
                          const QString& current_key) {
    QDialog dialog(parent);
    dialog.setWindowTitle(Tr("Who is playing?"));
    dialog.setMinimumWidth(380);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(12);
    layout->addWidget(BoldLabel(Tr("Who is playing?"), 2));
    auto* list = new QListWidget;
    list->setIconSize(QSize(48, 48));
    list->setSpacing(2);
    for (const ProfileRow& row : rows) {
        auto* item = new QListWidgetItem(
            QIcon(Shaped(row.picture, row.name, 48, true)),
            QStringLiteral("%1\n%2").arg(row.name,
                                         row.account.isEmpty() ? Tr("Offline") : row.account));
        item->setData(Qt::UserRole, row.key);
        list->addItem(item);
        if (row.key == current_key) {
            list->setCurrentItem(item);
        }
    }
    if (!list->currentItem() && list->count() > 0) {
        list->setCurrentRow(0);
    }
    layout->addWidget(list, 1);
    auto* remember = new QCheckBox(Tr("Remember my choice"));
    layout->addWidget(remember);

    ProfileChoice choice;
    auto* buttons = new QDialogButtonBox;
    QPushButton* go = buttons->addButton(Tr("Continue"), QDialogButtonBox::AcceptRole);
    QPushButton* add = buttons->addButton(Tr("Add account"), QDialogButtonBox::ActionRole);
    buttons->addButton(Tr("Cancel"), QDialogButtonBox::RejectRole);
    go->setDefault(true);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
    QObject::connect(add, &QPushButton::clicked, &dialog, [&dialog, &choice] {
        choice.action = ProfileChoice::Action::Add;
        dialog.done(QDialog::Accepted + 1);
    });
    layout->addWidget(buttons);
    list->setFocus();

    const int result = Run(dialog, list);
    if (choice.action == ProfileChoice::Action::Add) {
        return choice;
    }
    if (result != QDialog::Accepted || !list->currentItem()) {
        return {}; // Cancel keeps the last-used profile
    }
    choice.action = ProfileChoice::Action::Continue;
    choice.key = list->currentItem()->data(Qt::UserRole).toString();
    choice.remember = remember->isChecked();
    return choice;
}

bool OfferInvitation(QWidget* parent, const InvitationCard& card) {
    QDialog dialog(parent);
    dialog.setWindowTitle(Tr("Invitation"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(12);

    auto* who = new QHBoxLayout;
    who->setSpacing(12);
    who->addWidget(PictureLabel(card.sender_picture, card.sender, 48, true));
    auto* who_text = new QVBoxLayout;
    who_text->setSpacing(0);
    QLabel* name = BoldLabel(card.sender.isEmpty() ? Tr("—") : card.sender, 1);
    name->setWordWrap(true);
    who_text->addWidget(name);
    who_text->addWidget(DimLabel(Tr("invited you to play")));
    who->addLayout(who_text, 1);
    layout->addLayout(who);

    auto* what = new QHBoxLayout;
    what->setSpacing(12);
    what->addWidget(PictureLabel(card.game_icon, card.game, 48, false));
    auto* what_text = new QVBoxLayout;
    what_text->setSpacing(0);
    QLabel* game = BoldLabel(card.game.isEmpty() ? Tr("Unknown game") : card.game);
    game->setWordWrap(true);
    what_text->addWidget(game);
    if (card.sent > 0) {
        what_text->addWidget(DimLabel(Tr("Sent %1").arg(LocalTime(card.sent))));
    }
    what->addLayout(what_text, 1);
    layout->addLayout(what);

    if (!card.message.isEmpty()) {
        auto* quote = new QLabel(QStringLiteral("“%1”").arg(card.message));
        quote->setWordWrap(true);
        quote->setFrameShape(QFrame::StyledPanel);
        quote->setContentsMargins(10, 8, 10, 8);
        layout->addWidget(quote);
    }

    auto* buttons = new QDialogButtonBox;
    QPushButton* join = buttons->addButton(Tr("Join"), QDialogButtonBox::AcceptRole);
    buttons->addButton(Tr("Ignore"), QDialogButtonBox::RejectRole);
    join->setDefault(true);
    join->setFocus();
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    return Run(dialog) == QDialog::Accepted;
}

SaveChoice ChooseSave(QWidget* parent, const ConflictSides& sides) {
    QDialog dialog(parent);
    dialog.setWindowTitle(Tr("Choose a save"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(12);
    layout->addWidget(Body(Tr("%1 has a save on this machine and a different one in the cloud. "
                              "Choose which one to keep. The other is kept as a backup.")
                               .arg(sides.game)));

    auto* columns = new QGridLayout;
    columns->setHorizontalSpacing(24);
    columns->addWidget(BoldLabel(Tr("On this machine")), 0, 0);
    columns->addWidget(DimLabel(Tr("Last written %1").arg(When(sides.local_written))), 1, 0);
    columns->addWidget(BoldLabel(Tr("In the cloud")), 0, 1);
    columns->addWidget(DimLabel(Tr("Version %1 from %2, %3")
                                    .arg(sides.cloud_version)
                                    .arg(sides.cloud_device.isEmpty() ? Tr("—") : sides.cloud_device,
                                         When(sides.cloud_saved))),
                       1, 1);
    layout->addLayout(columns);

    SaveChoice choice = SaveChoice::Later;
    auto* buttons = new QDialogButtonBox;
    QPushButton* keep = buttons->addButton(Tr("Keep this machine's"), QDialogButtonBox::ActionRole);
    QPushButton* take = buttons->addButton(Tr("Take the cloud's"), QDialogButtonBox::ActionRole);
    QPushButton* later = buttons->addButton(Tr("Decide later"), QDialogButtonBox::RejectRole);
    keep->setAutoDefault(false);
    take->setAutoDefault(false);
    later->setDefault(true);
    later->setFocus();
    take->setEnabled(!sides.game_running);
    if (sides.game_running) {
        take->setToolTip(Tr("Stop the running game first."));
    }
    QObject::connect(keep, &QPushButton::clicked, &dialog, [&] {
        choice = SaveChoice::KeepLocal;
        dialog.accept();
    });
    QObject::connect(take, &QPushButton::clicked, &dialog, [&] {
        choice = SaveChoice::TakeCloud;
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    Run(dialog);
    return choice;
}

} // namespace openpak::qt
