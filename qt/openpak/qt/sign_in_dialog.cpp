// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#include "openpak/qt/sign_in_dialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "openpak/api.h"

OpenPakSignInDialog::OpenPakSignInDialog(QWidget* parent, bool first_run, const QString& error,
                                         const QString& email)
    : QDialog(parent) {
    setWindowTitle(tr("Sign in to OpenPak"));
    setMinimumWidth(380);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(12);

    if (first_run) {
        auto* intro = new QLabel(
            tr("Sign in once and this emulator does the rest: certificates, the console link and "
               "the network redirect are set up for you. You can do this later from the OpenPak "
               "menu."));
        intro->setWordWrap(true);
        layout->addWidget(intro);
    }
    if (!error.isEmpty()) {
        auto* failed = new QLabel(error);
        failed->setWordWrap(true);
        failed->setStyleSheet(QStringLiteral("QLabel { color: #e0393e; }"));
        layout->addWidget(failed);
    }

    email_edit = new QLineEdit(email);
    password_edit = new QLineEdit;
    password_edit->setEchoMode(QLineEdit::Password);
    auto* form = new QFormLayout;
    form->addRow(tr("Email"), email_edit);
    form->addRow(tr("Password"), password_edit);
    layout->addLayout(form);

    auto* create = new QLabel(QStringLiteral("<a href=\"%1/register\">%2</a>")
                                  .arg(QString::fromStdString(WebService::OpenPakApi::BaseUrl()),
                                       tr("Create an account")));
    create->setOpenExternalLinks(true);
    layout->addWidget(create);

    auto* buttons = new QDialogButtonBox;
    QPushButton* submit = buttons->addButton(tr("Sign in"), QDialogButtonBox::AcceptRole);
    buttons->addButton(first_run ? tr("Not now") : tr("Cancel"), QDialogButtonBox::RejectRole);
    layout->addWidget(buttons);

    const auto ready = [this, submit] {
        submit->setEnabled(!email_edit->text().trimmed().isEmpty() && !password_edit->text().isEmpty());
    };
    connect(email_edit, &QLineEdit::textChanged, this, ready);
    connect(password_edit, &QLineEdit::textChanged, this, ready);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    ready();

    (email.isEmpty() ? email_edit : password_edit)->setFocus();
}

QString OpenPakSignInDialog::Email() const {
    return email_edit->text().trimmed();
}

QString OpenPakSignInDialog::Password() const {
    return password_edit->text();
}
