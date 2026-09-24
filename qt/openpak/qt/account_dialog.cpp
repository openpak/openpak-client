// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <utility>

#include <QAbstractButton>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QComboBox>
#include <QEvent>
#include <QEventLoop>
#include <QFileDialog>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QShowEvent>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "openpak/account.h"
#include "openpak/api.h"
#include "openpak/qt/account_dialog.h"
#include "openpak/qt/account_page_p.h"
#include "openpak/qt/avatar_cache.h"
#include "openpak/qt/controller_navigation.h"
#include "openpak/qt/extra_pages.h"
#include "openpak/qt/host.h"
#include "openpak/qt/pages_p.h"
#include "openpak/qt/prompts.h"
#include "openpak/session.h"

using openpak::qt::Tr;

namespace {

constexpr int kAvatarSize = 56; // UX spec §6: the Account card's avatar
constexpr int kBadgeRole = Qt::UserRole + 1;

int g_last_page = OpenPakAccountDialog::kAccountPage;

QColor CardBg() {
    return openpak::qt::Host::Current()->IsDarkTheme() ? QColor(30, 30, 34) : QColor(244, 244, 248);
}

QColor DimColor() {
    return openpak::qt::Host::Current()->IsDarkTheme() ? QColor(150, 150, 158) : QColor(110, 110, 120);
}

QColor AccentColor() {
    const QColor accent(openpak::qt::Host::Current()->AccentColor());
    if (accent.isValid()) {
        return accent;
    }
    const QColor highlight = QApplication::palette().color(QPalette::Highlight);
    return (highlight.isValid() && highlight != Qt::black) ? highlight : QColor(100, 149, 237);
}

QString CardStyle() {
    return QStringLiteral("QFrame { background: %1; border-radius: 12px; }").arg(CardBg().name());
}

// The page list's Friends entry carries the incoming requests as a red count (UX spec §3.6).
class BadgeDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyledItemDelegate::paint(painter, option, index);
        const int count = index.data(kBadgeRole).toInt();
        if (count <= 0) {
            return;
        }
        const QString text = count > 99 ? QStringLiteral("99+") : QString::number(count);
        QFont font = option.font;
        font.setBold(true);
        font.setPointSizeF((std::max)(6.0, font.pointSizeF() - 2));
        const QFontMetrics metrics(font);
        const int height = metrics.height() + 2;
        const int width = (std::max)(height, metrics.horizontalAdvance(text) + 10);
        const QRect badge(option.rect.right() - width - 8,
                          option.rect.center().y() - height / 2, width, height);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0xE0, 0x39, 0x3E));
        painter->drawRoundedRect(badge, height / 2.0, height / 2.0);
        painter->setPen(Qt::white);
        painter->setFont(font);
        painter->drawText(badge, Qt::AlignCenter, text);
        painter->restore();
    }
};

QLabel* Pill(const QString& text) {
    auto* label = new QLabel(text);
    label->setFont(QFont(QStringLiteral("monospace")));
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("QLabel { padding: 2px 8px; border-radius: 8px; "
                                        "background: rgba(127,127,127,50); }"));
    return label;
}

} // Anonymous namespace

QString openpak::qt::style::Card() { return CardStyle(); }
QColor openpak::qt::style::Dim() { return DimColor(); }

int OpenPakAccountDialog::LastPage() {
    return g_last_page;
}

OpenPakAccountDialog::OpenPakAccountDialog(openpak::qt::Host* controller_, QWidget* parent,
                                           int initial_page)
    : QDialog(parent), controller(controller_),
      friends_snapshot(std::make_shared<WebService::OpenPakApi::AccountFriends>()) {
    setWindowTitle(Tr("OpenPak"));
    // UX spec §3.6: 880x600, resizable down to 760x480.
    setMinimumSize(760, 480);
    resize(880, 600);

    // Otherwise the same physical controller drives both this window and the running game;
    // restored in the destructor.
    controller->SetGuestInputSuspended(true);

    controller_navigation = controller->CreateNavigation(this);
    if (!controller_navigation) {
        controller_navigation = new openpak::qt::Navigation(this);
    }
    WireControllerNav();

    queue = new openpak::qt::WorkQueue(this);

    status = new QLabel;
    status->setWordWrap(true);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    busy = new QProgressBar;
    busy->setRange(0, 0); // indeterminate: how long a request takes is the server's business
    busy->setFixedSize(90, 4);
    busy->setTextVisible(false);
    busy->hide();

    openpak::qt::PageContext context;
    context.host = controller;
    context.queue = queue;
    context.status = [this](const QString& message) { status->setText(message); };
    context.confirm = [this](const QString& title, const QString& message, const QString& yes) {
        return ConfirmAction(title, message, yes, Tr("Cancel"));
    };
    context.close = [this] { accept(); };
    context.go_to = [this](int page) { GoToPage(page); };
    context.friends = friends_snapshot;
    context.friends_changed = [this] { UpdateBadge(); };
    context.invite_to_chat = [this](u64 pid, const QString& friend_name) {
        emit InviteToChatRequested(pid, friend_name);
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

    // The left list, with the page icons of UX spec §6.
    nav = new QListWidget;
    nav->setItemDelegate(new BadgeDelegate(nav));
    nav->setIconSize(QSize(20, 20));
    nav->setFixedWidth(190);
    nav->setFrameShape(QFrame::NoFrame);
    nav->setSpacing(2);
    struct Entry {
        const char* icon;
        QString title;
    };
    const Entry entries[] = {
        {"account", Tr("Account")},         {"friends", Tr("Friends")}, {"invitations", Tr("Invitations")},
        {"saves", Tr("Cloud saves")},       {"mods", Tr("Mods")},       {"news", Tr("News")},
        {"status", Tr("Status")},
    };
    for (const Entry& entry : entries) {
        auto* item = new QListWidgetItem(openpak::qt::Icon(entry.icon), entry.title);
        item->setSizeHint(QSize(0, 36));
        nav->addItem(item);
    }
    connect(nav, &QListWidget::currentRowChanged, this, &OpenPakAccountDialog::GoToPage);

    // The page header: its title on the left, Refresh on the right.
    page_title = openpak::qt::BoldLabel({}, 3);
    refresh_button = openpak::qt::TextButton(Tr("Refresh"));
    connect(refresh_button, &QPushButton::clicked, this, [this] { ReloadPage(current_page, true); });
    auto* header = new QHBoxLayout;
    header->addWidget(page_title, 1);
    header->addWidget(refresh_button);

    queue->on_busy = [this](bool working) {
        busy->setVisible(working);
        refresh_button->setEnabled(!working);
    };

    auto* right = new QVBoxLayout;
    right->setSpacing(10);
    right->addLayout(header);
    right->addWidget(pages_stack, 1);

    auto* body = new QHBoxLayout;
    body->setSpacing(14);
    body->addWidget(nav);
    body->addLayout(right, 1);

    // The footer, the same on every page. The window saves nothing, so its one button closes it.
    auto* close_button = new QPushButton(Tr("Close"));
    close_button->setDefault(true);
    close_button->setAutoDefault(true);
    connect(close_button, &QPushButton::clicked, this, &QDialog::accept);
    auto* footer = new QHBoxLayout;
    footer->setSpacing(10);
    footer->addWidget(busy, 0, Qt::AlignVCenter);
    footer->addWidget(status, 1);
    footer->addWidget(close_button);

    setStyleSheet(QStringLiteral("QPushButton:focus, QToolButton:focus, QCheckBox:focus, "
                                 "QComboBox:focus, QLineEdit:focus, QListWidget:focus { "
                                 "outline: none; border: 2px solid %1; border-radius: 6px; }")
                      .arg(AccentColor().name()));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);
    layout->addLayout(body, 1);
    layout->addLayout(footer);

    GoToPage(std::clamp(initial_page, 0, pages_stack->count() - 1));

    // Every page loads when the window opens; the queue runs them one after another, the page
    // that is showing first.
    ReloadAll();

    // Friends and Invitations are drawn again every 30 seconds while the window is open.
    refresh_timer.setInterval(30000);
    connect(&refresh_timer, &QTimer::timeout, this, [this] {
        ReloadPage(kFriendsPage, false);
        ReloadPage(kInvitationsPage, false);
    });
    refresh_timer.start();

    // Signing in or out changes what every page can show.
    connect(controller, &openpak::qt::Host::AccountLinked, this, &OpenPakAccountDialog::ReloadAll);
    connect(controller, &openpak::qt::Host::AccountUnlinked, this, [this] {
        *friends_snapshot = {};
        UpdateBadge();
        ReloadAll();
    });
    connect(controller, &openpak::qt::Host::StatusChanged, this,
            [this](const QString& message) { status->setText(message); });
}

OpenPakAccountDialog::~OpenPakAccountDialog() {
    controller->SetGuestInputSuspended(false);
}

QWidget* OpenPakAccountDialog::MakeAccountPage() {
    // 1. The identity card: picture, name, friend code, sign out.
    auto* identity = new QFrame;
    identity->setObjectName(QStringLiteral("row"));
    identity->setStyleSheet(CardStyle());
    auto* identity_layout = new QHBoxLayout(identity);
    identity_layout->setContentsMargins(18, 14, 18, 14);
    identity_layout->setSpacing(14);

    avatar = new QLabel;
    avatar->setFixedSize(kAvatarSize, kAvatarSize);
    avatar->setCursor(Qt::PointingHandCursor);
    avatar->setToolTip(Tr("Change picture..."));
    avatar->setAccessibleName(Tr("Change picture..."));
    avatar->setFocusPolicy(Qt::StrongFocus);
    avatar->setContextMenuPolicy(Qt::CustomContextMenu);
    avatar->installEventFilter(this);
    connect(avatar, &QWidget::customContextMenuRequested, this, [this](const QPoint& at) {
        if (!openpak::qt::SignedIn()) {
            return;
        }
        QMenu menu(avatar);
        menu.addAction(openpak::qt::Icon("picture"), Tr("Change picture..."), this,
                       &OpenPakAccountDialog::OnChangeAvatar);
        menu.exec(avatar->mapToGlobal(at));
    });
    identity_layout->addWidget(avatar, 0, Qt::AlignTop);

    auto* text = new QVBoxLayout;
    text->setSpacing(6);
    auto* name_row = new QHBoxLayout;
    name_row->setSpacing(8);
    name = openpak::qt::BoldLabel({}, 4);
    name->setWordWrap(true);
    name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* change_name = openpak::qt::TextButton(Tr("Change name..."));
    change_name->setIcon(openpak::qt::Icon("edit"));
    connect(change_name, &QPushButton::clicked, this, &OpenPakAccountDialog::OnEditUsername);
    name_row->addWidget(name, 1);
    name_row->addWidget(change_name);
    text->addLayout(name_row);

    auto* code_row = new QHBoxLayout;
    code_row->setSpacing(8);
    code_pill = Pill({});
    auto* copy = openpak::qt::TextButton(Tr("Copy"));
    copy->setIcon(openpak::qt::Icon("copy"));
    connect(copy, &QPushButton::clicked, this, [this] {
        QClipboard* clipboard = QApplication::clipboard();
        if (!clipboard) {
            status->setText(Tr("The clipboard is not available here. Your friend code is %1.")
                                .arg(code_pill->text()));
            return;
        }
        clipboard->setText(code_pill->text());
        status->setText(Tr("Copied."));
    });
    code_row->addWidget(code_pill);
    code_row->addWidget(copy);
    code_row->addStretch(1);
    text->addLayout(code_row);
    identity_layout->addLayout(text, 1);

    auto* sign_out = openpak::qt::TextButton(Tr("Sign out..."));
    sign_out->setIcon(openpak::qt::Icon("sign-out"));
    connect(sign_out, &QPushButton::clicked, this, &OpenPakAccountDialog::OnSignOut);
    identity_layout->addWidget(sign_out, 0, Qt::AlignTop);
    first_focus = change_name;

    // 2. The details, as label and value rows; an empty value shows "—".
    auto* details = new QFrame;
    details->setObjectName(QStringLiteral("row"));
    details->setStyleSheet(CardStyle());
    auto* grid = new QGridLayout(details);
    grid->setContentsMargins(18, 14, 18, 14);
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(8);
    grid->setColumnMinimumWidth(0, 150);
    grid->setColumnStretch(1, 1);
    int row = 0;
    const auto add_detail = [&](const QString& label, QLabel*& value) {
        grid->addWidget(openpak::qt::DimLabel(label), row, 0, Qt::AlignTop);
        value = new QLabel(Tr("—"));
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setWordWrap(true);
        grid->addWidget(value, row, 1);
        ++row;
    };
    const openpak::qt::Family family = controller->GetFamily();
    const QString console = family == openpak::qt::Family::WiiU   ? QStringLiteral("Wii U")
                            : family == openpak::qt::Family::N3DS ? QStringLiteral("3DS")
                            : family == openpak::qt::Family::Wii  ? QStringLiteral("Wii")
                            : family == openpak::qt::Family::DS   ? QStringLiteral("DS")
                                                                  : QStringLiteral("Switch");
    add_detail(Tr("Friend code"), account_code);
    account_code->setFont(QFont(QStringLiteral("monospace")));
    add_detail(Tr("%1 identity").arg(console), account_identity);
    add_detail(Tr("Linked consoles"), account_consoles);
    add_detail(Tr("Console link"), account_link);
    // A failed link is tried again with the password, which only the sign-in knows.
    link_retry = openpak::qt::TextButton(Tr("Try again"));
    connect(link_retry, &QPushButton::clicked, this, [this] { controller->SignIn(); });
    grid->addWidget(link_retry, row - 1, 2, Qt::AlignTop);
    link_retry->hide();
    if (openpak::qt::IsWfc(family)) {
        // WFC minted no console accounts: OpenPak names the player and keeps the saves.
        grid->itemAtPosition(row - 1, 0)->widget()->hide();
        account_link->hide();
    }

    account_signed_in = new QWidget;
    auto* signed_in_layout = new QVBoxLayout(account_signed_in);
    signed_in_layout->setContentsMargins(0, 0, 0, 0);
    signed_in_layout->setSpacing(12);
    signed_in_layout->addWidget(identity);
    signed_in_layout->addWidget(details);
    signed_in_layout->addStretch(1);

    account_signed_out = openpak::qt::NeedSignIn(controller);

    auto* page = new QWidget;
    auto* page_layout = new QVBoxLayout(page);
    page_layout->setContentsMargins(0, 0, 0, 0);
    page_layout->addWidget(account_signed_in, 1);
    page_layout->addWidget(account_signed_out, 1);
    return page;
}

void OpenPakAccountDialog::ShowAvatar() {
    const QString who = name->text();
    avatar->setPixmap(avatar_source.isNull() ? openpak::qt::Initials(who, kAvatarSize)
                                             : openpak::qt::Round(avatar_source, kAvatarSize));
}

void OpenPakAccountDialog::RefreshAccount() {
    const bool signed_in = openpak::qt::SignedIn();
    account_signed_in->setVisible(signed_in);
    account_signed_out->setVisible(!signed_in);
    if (!signed_in) {
        avatar_source = {};
        return;
    }

    // What the library already holds, drawn at once; the site's answer fills in the rest.
    const std::string username = Common::OpenPakAccount::GetUsername();
    const std::string code = Common::OpenPakAccount::GetFriendCode();
    name->setText(username.empty() ? Tr("—") : QString::fromStdString(username));
    code_pill->setText(QString::fromStdString(code));
    code_pill->setVisible(!code.empty());
    account_code->setText(code.empty() ? Tr("—") : QString::fromStdString(code));
    ShowAvatar();

    const u64 pid = Common::OpenPakAccount::GetPid();
    const openpak::qt::Family family = controller->GetFamily();
    link_retry->hide();
    if (family == openpak::qt::Family::WiiU || family == openpak::qt::Family::N3DS) {
        // The host's own identity call minted these; it words them.
        const QString identity = controller->ConsoleIdentity();
        const QString link = controller->ConsoleLink();
        account_identity->setText(identity.isEmpty() ? Tr("—") : identity);
        account_link->setText(link.isEmpty() ? Tr("—") : link);
    } else if (openpak::qt::IsWfc(family)) {
        account_identity->setText(Tr("Wii and DS games have no console account; OpenPak names you "
                                     "and keeps your saves."));
    } else {
        account_identity->setText(pid != 0 ? Tr("PID %1").arg(pid) : Tr("—"));
        const bool linked = openpak::client::session::Linked();
        if (linked) {
            account_link->setText(Tr("Linked as %1")
                                      .arg(QString::fromStdString(openpak::client::session::Nickname())));
        } else if (openpak::client::session::Enabled()) {
            account_link->setText(Tr("Signed in, but this emulator's console account did not link."));
            link_retry->show();
        } else {
            account_link->setText(Tr("—"));
        }
    }

    queue->Run(
        this, [] { return WebService::OpenPakApi::GetProfile(); },
        [this](WebService::OpenPakApi::Profile profile) {
            if (!profile.ok) {
                status->setText(openpak::qt::ErrorText(profile.error));
                return;
            }
            if (!profile.name.empty()) {
                name->setText(QString::fromStdString(profile.name));
            }
            // The Switch identity's code wins, as in the reference; the site's fills in for an
            // account without one.
            if (Common::OpenPakAccount::GetFriendCode().empty() && !profile.friend_code.empty()) {
                account_code->setText(QString::fromStdString(profile.friend_code));
                code_pill->setText(QString::fromStdString(profile.friend_code));
                code_pill->show();
            }
            QStringList consoles;
            for (const std::string& name_space : profile.linked_platforms) {
                consoles << openpak::qt::ConsoleName(name_space);
            }
            account_consoles->setText(consoles.isEmpty() ? Tr("—")
                                                         : consoles.join(QStringLiteral(", ")));
            avatar_source = OpenPak::AvatarCache::Get("self", profile.image_base64, 256);
            ShowAvatar();
        });
}

void OpenPakAccountDialog::OnSignOut() {
    // Signing out revokes the token; undoing it means the password again (UX spec §3.5).
    if (!openpak::qt::ConfirmSignOut(this, controller->ProfileName())) {
        return;
    }
    controller->SignOut();
    *friends_snapshot = {};
    UpdateBadge();
    status->setText(Tr("Signed out of OpenPak."));
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

void OpenPakAccountDialog::UpdateBadge() {
    const auto incoming = std::count_if(friends_snapshot->requests.begin(),
                                        friends_snapshot->requests.end(),
                                        [](const auto& request) { return request.incoming; });
    if (QListWidgetItem* friends = nav->item(kFriendsPage)) {
        friends->setData(kBadgeRole, static_cast<int>(incoming));
    }
}

bool OpenPakAccountDialog::eventFilter(QObject* watched, QEvent* event) {
    const bool activate =
        event->type() == QEvent::MouseButtonRelease ||
        (event->type() == QEvent::KeyPress &&
         (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Space ||
          static_cast<QKeyEvent*>(event)->key() == Qt::Key_Return ||
          static_cast<QKeyEvent*>(event)->key() == Qt::Key_Enter));
    if (watched == avatar && activate && openpak::qt::SignedIn()) {
        if (event->type() == QEvent::MouseButtonRelease &&
            static_cast<QMouseEvent*>(event)->button() != Qt::LeftButton) {
            return QDialog::eventFilter(watched, event);
        }
        OnChangeAvatar();
        return true;
    }
    return QDialog::eventFilter(watched, event);
}

void OpenPakAccountDialog::keyPressEvent(QKeyEvent* event) {
    // Ctrl+PgUp/PgDn switch pages, as tabs do everywhere; plain PgUp/PgDn stay the lists'.
    if (event->modifiers() & Qt::ControlModifier) {
        if (event->key() == Qt::Key_PageDown) {
            GoToPage(current_page + 1);
            return;
        }
        if (event->key() == Qt::Key_PageUp) {
            GoToPage(current_page - 1);
            return;
        }
    }
    QDialog::keyPressEvent(event);
}

void OpenPakAccountDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (QApplication::focusWidget() && isAncestorOf(QApplication::focusWidget())) {
        return;
    }
    nav->setFocus();
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
    g_last_page = index;
    pages_stack->setCurrentIndex(index);
    {
        const QSignalBlocker blocker{nav};
        nav->setCurrentRow(index);
    }
    if (QListWidgetItem* item = nav->item(index)) {
        page_title->setText(item->text());
    }
    // A page is refreshed each time it is navigated to; the window's opening loads are
    // ReloadAll's.
    if (!first) {
        ReloadPage(index, false);
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
    connect(controller_navigation, &openpak::qt::Navigation::backPressed, this, &QDialog::reject);
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
    // Up and down on the page list move between pages, as the stick does in any list.
    if (focused == nav && dy != 0) {
        GoToPage(current_page + dy);
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
    if (focused == nav) {
        // A on the page list steps into the page.
        focusNextChild();
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
                                         const QString& yes_text, const QString& no_text) {
    // A confirmation of a destructive action (UX spec §3.12): the words, the action and Cancel,
    // Cancel the default and what Escape and B do. Driven by the controller while it is up.
    QDialog box(this);
    box.setWindowTitle(title);
    box.setMinimumWidth(380);
    auto* layout = new QVBoxLayout(&box);
    layout->setSpacing(12);
    auto* words = new QLabel(message);
    words->setWordWrap(true);
    layout->addWidget(words);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto* yes = new QPushButton(yes_text);
    auto* no = new QPushButton(no_text);
    yes->setAutoDefault(false);
    no->setDefault(true);
    buttons->addWidget(yes);
    buttons->addWidget(no);
    layout->addLayout(buttons);
    connect(yes, &QPushButton::clicked, &box, &QDialog::accept);
    connect(no, &QPushButton::clicked, &box, &QDialog::reject);
    no->setFocus();

    disconnect(controller_navigation, nullptr, this, nullptr);
    const auto nav_conn = connect(controller_navigation, &openpak::qt::Navigation::navigated, &box,
                                  [yes, no](int dx, int) {
                                      if (dx > 0) {
                                          no->setFocus();
                                      } else if (dx < 0) {
                                          yes->setFocus();
                                      }
                                  });
    const auto activate_conn = connect(
        controller_navigation, &openpak::qt::Navigation::activated, &box,
        [] {
            if (auto* button = qobject_cast<QAbstractButton*>(QApplication::focusWidget())) {
                button->click();
            }
        },
        Qt::QueuedConnection);
    const auto back_conn = connect(controller_navigation, &openpak::qt::Navigation::backPressed,
                                   &box, &QDialog::reject);
    const bool result = box.exec() == QDialog::Accepted;
    disconnect(nav_conn);
    disconnect(activate_conn);
    disconnect(back_conn);
    WireControllerNav();
    return result;
}

void OpenPakAccountDialog::OnChangeAvatar() {
    const QString path = QFileDialog::getOpenFileName(this, Tr("Change picture..."), QString{},
                                                      QStringLiteral("*.png *.jpg *.jpeg"));
    if (path.isEmpty()) {
        return;
    }
    QImage image(path);
    if (image.isNull()) {
        status->setText(Tr("Couldn't read that image."));
        return;
    }
    const int side = (std::min)(image.width(), image.height());
    const QRect crop((image.width() - side) / 2, (image.height() - side) / 2, side, side);
    const QImage square =
        image.copy(crop).scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    square.save(&buffer, "JPG", 85);
    const std::string image_base64 = jpeg.toBase64().toStdString();

    avatar_source = QPixmap::fromImage(square);
    ShowAvatar();
    queue->Run(
        this, [image_base64] { return WebService::OpenPakApi::PushProfilePicture(image_base64); },
        [this](std::string error) {
            status->setText(error.empty() ? Tr("Profile picture updated.")
                                          : openpak::qt::ErrorText(error));
        });
}

void OpenPakAccountDialog::OnEditUsername() {
    static const QRegularExpression valid_name(QStringLiteral("^[A-Za-z0-9_-]{3,16}$"));
    bool ok = false;
    const QString wanted =
        QInputDialog::getText(this, Tr("Change name"), Tr("3-16 characters: letters, digits, '_' or '-'"),
                              QLineEdit::Normal, name->text(), &ok)
            .trimmed();
    if (!ok || wanted.isEmpty() || wanted == name->text()) {
        return;
    }
    if (!valid_name.match(wanted).hasMatch()) {
        status->setText(Tr("3-16 characters: letters, digits, '_' or '-'"));
        return;
    }
    const std::string new_name = wanted.toStdString();
    queue->Run(
        this, [new_name] { return WebService::OpenPakApi::SetUsername(new_name); },
        [this, new_name](std::string error) {
            if (!error.empty()) {
                status->setText(openpak::qt::ErrorText(error));
                return;
            }
            Common::OpenPakAccount::Save(Common::OpenPakAccount::GetPid(), new_name,
                                         Common::OpenPakAccount::GetFriendCode(),
                                         Common::OpenPakAccount::GetToken(),
                                         Common::OpenPakAccount::GetBearer());
            name->setText(QString::fromStdString(new_name));
            status->setText(Tr("Name updated."));
        });
}
