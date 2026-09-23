// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#include "openpak/qt/host_kit.h"

#include <memory>
#include <thread>

#include <QAction>
#include <QDesktopServices>
#include <QIcon>
#include <QMenu>
#include <QMenuBar>
#include <QSysInfo>
#include <QUrl>

#include "openpak/account.h"
#include "openpak/api.h"
#include "openpak/qt/account_dialog.h"
#include "openpak/qt/pages_p.h"
#include "openpak/qt/prompts.h"

namespace openpak::qt {

namespace Api = WebService::OpenPakApi;

QMenu* AddOpenPakMenu(QMenuBar* menu_bar, MenuHooks hooks_) {
    auto* menu = new QMenu(Tr("OpenPak"), menu_bar);
    menu->setToolTipsVisible(true);
    const QList<QAction*> existing = menu_bar->actions();
    if (existing.isEmpty()) {
        menu_bar->addMenu(menu);
    } else {
        menu_bar->insertMenu(existing.back(), menu);
    }

    auto hooks = std::make_shared<MenuHooks>(std::move(hooks_));
    QObject::connect(menu, &QMenu::aboutToShow, menu, [menu, hooks] {
        menu->clear();
        const bool running = hooks->game_running && hooks->game_running();
        const bool on = !hooks->openpak_on || hooks->openpak_on();
        const QString name = hooks->signed_in_as ? hooks->signed_in_as() : QString{};
        const bool signed_in = on && !name.isEmpty();
        const QWidget* parent = menu->parentWidget();
        (void)parent;

        // 1. The header: who is signed in, or the way to sign in.
        if (signed_in) {
            QAction* header = menu->addAction(Tr("Signed in as %1").arg(name));
            if (hooks->avatar) {
                const QPixmap avatar = hooks->avatar();
                if (!avatar.isNull()) {
                    header->setIcon(QIcon(avatar));
                }
            }
            QObject::connect(header, &QAction::triggered, menu, [hooks] {
                hooks->open_window(OpenPakAccountDialog::kAccountPage);
            });
        } else {
            QAction* header = menu->addAction(Tr("Sign in to OpenPak..."));
            if (!on) {
                QObject::connect(header, &QAction::triggered, menu, [hooks] { hooks->open_settings(); });
            } else {
                header->setEnabled(!running);
                if (running) {
                    header->setToolTip(Tr("Stop the running game first."));
                }
                QObject::connect(header, &QAction::triggered, menu, [hooks] { hooks->sign_in(); });
            }
        }
        menu->addSeparator();

        // 2-7. The window's pages, in its order.
        struct Item {
            const char* title;
            int page;
            bool needs_account;
        };
        static constexpr Item kItems[] = {
            {"Friends", OpenPakAccountDialog::kFriendsPage, true},
            {"Invitations", OpenPakAccountDialog::kInvitationsPage, true},
            {"Cloud saves", OpenPakAccountDialog::kCloudSavesPage, true},
            {"Mods", OpenPakAccountDialog::kModsPage, false},
            {"News", OpenPakAccountDialog::kNewsPage, false},
            {"Status", OpenPakAccountDialog::kStatusPage, false},
        };
        for (const Item& item : kItems) {
            QAction* action = menu->addAction(Tr(item.title));
            action->setEnabled(!item.needs_account || signed_in);
            QObject::connect(action, &QAction::triggered, menu,
                             [hooks, page = item.page] { hooks->open_window(page); });
        }
        menu->addSeparator();

        // 8-10.
        QObject::connect(menu->addAction(Tr("OpenPak settings...")), &QAction::triggered, menu,
                         [hooks] { hooks->open_settings(); });
        QObject::connect(menu->addAction(Tr("OpenPak website")), &QAction::triggered, menu, [] {
            QDesktopServices::openUrl(QUrl(QString::fromStdString(Api::BaseUrl())));
        });
        QAction* sign_out = menu->addAction(Tr("Sign out..."));
        sign_out->setEnabled(signed_in && !running);
        if (signed_in && running) {
            sign_out->setToolTip(Tr("Stop the running game first."));
        }
        QObject::connect(sign_out, &QAction::triggered, menu, [menu, hooks] {
            if (ConfirmSignOut(menu->parentWidget() ? menu->parentWidget()->window() : nullptr,
                               hooks->profile)) {
                hooks->sign_out();
            }
        });
    });
    return menu;
}

QString SignInAccountOnly(const QString& email, const QString& password, const QString& device) {
    const auto result = Api::SignInAccountOnly(email.toStdString(), password.toStdString());
    if (!result.ok) {
        return QString::fromStdString(result.error);
    }
    // Stored first under the email, so the profile can be asked for with the new token; then
    // under the account's own name, which is what the menu and the window show.
    Common::OpenPakAccount::SaveBearerOnly(email.toStdString(), result.bearer);
    const auto profile = Api::GetProfile();
    if (profile.ok && !profile.name.empty()) {
        Common::OpenPakAccount::SaveBearerOnly(profile.name, result.bearer);
    }
    if (!device.isEmpty()) {
        Api::SetSaveDevice(device.toStdString());
    }
    return {};
}

void SignOutAndRevoke() {
    const std::string bearer = Common::OpenPakAccount::GetBearer();
    Common::OpenPakAccount::Clear();
    if (!bearer.empty()) {
        std::thread([bearer] { Api::RevokeToken(bearer); }).detach();
    }
}

QString DefaultDeviceName(const QString& emulator) {
    QString machine = QSysInfo::machineHostName();
    if (machine.isEmpty()) {
        machine = QSysInfo::prettyProductName();
    }
    return Tr("%1 on %2").arg(emulator, machine);
}

} // namespace openpak::qt
