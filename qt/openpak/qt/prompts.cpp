// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#include "openpak/qt/prompts.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "openpak/api.h"
#include "openpak/qt/pages_p.h"

namespace openpak::qt {

namespace {

QString WebsiteLink(const QString& path, const QString& text) {
    return QStringLiteral("<a href=\"%1%2\">%3</a>")
        .arg(QString::fromStdString(WebService::OpenPakApi::BaseUrl()), path, text.toHtmlEscaped());
}

QString When(s64 unix_seconds) {
    return unix_seconds > 0 ? LocalTime(unix_seconds) : Tr("—");
}

} // namespace

bool ConfirmSignOut(QWidget* parent, const QString& profile) {
    QMessageBox box(parent);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(Tr("Sign out of OpenPak?"));
    box.setText(Tr("Sign out of OpenPak?"));
    box.setInformativeText(
        profile.isEmpty()
            ? Tr("This emulator goes offline. Your friends and cloud saves stay on your account, "
                 "and you can sign in again at any time.")
            : Tr("%1 goes offline on this emulator. Your friends, invitations and cloud saves "
                 "stay on your account, and you can sign in again at any time.")
                  .arg(profile));
    QPushButton* sign_out = box.addButton(Tr("Sign out"), QMessageBox::DestructiveRole);
    QPushButton* cancel = box.addButton(Tr("Cancel"), QMessageBox::RejectRole);
    box.setDefaultButton(cancel);
    box.setEscapeButton(cancel);
    box.exec();
    return box.clickedButton() == sign_out;
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
    dialog.setMinimumWidth(420);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(12);
    auto* title = BoldLabel(Tr("Connect to OpenPak?"), 2);
    layout->addWidget(title);
    auto* text = new QLabel(body);
    text->setWordWrap(true);
    layout->addWidget(text);
    auto* create = new QLabel(WebsiteLink(QStringLiteral("/register"), Tr("Create an account")));
    create->setOpenExternalLinks(true);
    layout->addWidget(create);

    auto* buttons = new QDialogButtonBox;
    QPushButton* sign_in = buttons->addButton(Tr("Sign in"), QDialogButtonBox::AcceptRole);
    buttons->addButton(Tr("Not now"), QDialogButtonBox::RejectRole);
    sign_in->setDefault(true);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    return dialog.exec() == QDialog::Accepted;
}

SaveChoice ChooseSave(QWidget* parent, const ConflictSides& sides) {
    QDialog dialog(parent);
    dialog.setWindowTitle(Tr("Choose a save"));
    dialog.setMinimumWidth(480);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(12);
    auto* body = new QLabel(Tr("%1 has a save on this machine and a different one in the cloud. "
                               "Choose which one to keep. The other is kept as a backup.")
                                .arg(sides.game));
    body->setWordWrap(true);
    layout->addWidget(body);

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
    dialog.exec();
    return choice;
}

} // namespace openpak::qt
