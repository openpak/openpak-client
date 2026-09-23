// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#include "openpak/qt/sign_in_dialog.h"

#include <memory>
#include <thread>

#include <QAction>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QIcon>
#include <QVBoxLayout>

#include "openpak/api.h"

OpenPakSignInDialog::OpenPakSignInDialog(QWidget* parent, const QString& intro_text,
                                         const QString& error, const QString& email)
    : QDialog(parent) {
    setWindowTitle(tr("Sign in to OpenPak"));
    setMinimumWidth(420);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(12);

    if (!intro_text.isEmpty()) {
        auto* intro = new QLabel(intro_text);
        intro->setWordWrap(true);
        layout->addWidget(intro);
    }
    error_label = new QLabel;
    error_label->setWordWrap(true);
    error_label->setStyleSheet(QStringLiteral("QLabel { color: #e0393e; }"));
    layout->addWidget(error_label);
    ShowError(error);

    email_edit = new QLineEdit(email);
    password_edit = new QLineEdit;
    password_edit->setEchoMode(QLineEdit::Password);
    // The reveal toggle, where the icon theme has an eye for it: a trailing action on the field.
    const QIcon eye = QIcon::fromTheme(QStringLiteral("view-visible"),
                                       QIcon::fromTheme(QStringLiteral("password-show-on")));
    if (!eye.isNull()) {
        QAction* reveal = password_edit->addAction(eye, QLineEdit::TrailingPosition);
        reveal->setCheckable(true);
        reveal->setToolTip(tr("Show password"));
        connect(reveal, &QAction::toggled, this, [this](bool on) {
            password_edit->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
        });
    }
    device_edit = new QLineEdit;
    device_label = new QLabel(tr("Device name"));
    device_hint = new QLabel(tr("How this machine appears in your account's device list."));
    device_hint->setWordWrap(true);
    device_hint->setEnabled(false);
    auto* form = new QFormLayout;
    form->addRow(tr("Email"), email_edit);
    form->addRow(tr("Password"), password_edit);
    form->addRow(device_label, device_edit);
    form->addRow(QString{}, device_hint);
    layout->addLayout(form);
    device_label->hide();
    device_edit->hide();
    device_hint->hide();

    auto* create = new QLabel(QStringLiteral("<a href=\"%1/register\">%2</a>")
                                  .arg(QString::fromStdString(WebService::OpenPakApi::BaseUrl()),
                                       tr("Create an account")));
    create->setOpenExternalLinks(true);
    layout->addWidget(create);

    busy = new QProgressBar;
    busy->setRange(0, 0);
    busy->setFixedSize(90, 4);
    busy->setTextVisible(false);
    busy->hide();
    auto* buttons = new QDialogButtonBox;
    submit = buttons->addButton(tr("Sign in"), QDialogButtonBox::AcceptRole);
    submit->setDefault(true);
    buttons->addButton(tr("Cancel"), QDialogButtonBox::RejectRole);
    auto* footer = new QHBoxLayout;
    footer->addWidget(busy, 0, Qt::AlignVCenter);
    footer->addWidget(buttons, 1);
    layout->addLayout(footer);

    connect(email_edit, &QLineEdit::textChanged, this, &OpenPakSignInDialog::UpdateReady);
    connect(password_edit, &QLineEdit::textChanged, this, &OpenPakSignInDialog::UpdateReady);
    connect(buttons, &QDialogButtonBox::accepted, this, &OpenPakSignInDialog::Submit);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    UpdateReady();

    (email.isEmpty() ? email_edit : password_edit)->setFocus();
}

void OpenPakSignInDialog::SetDeviceName(const QString& device_default) {
    device_edit->setText(device_default);
    device_label->show();
    device_edit->show();
    device_hint->show();
}

void OpenPakSignInDialog::SetSubmitter(Submitter submitter_) {
    submitter = std::move(submitter_);
}

void OpenPakSignInDialog::UpdateReady() {
    submit->setEnabled(!running && !email_edit->text().trimmed().isEmpty() &&
                       !password_edit->text().isEmpty());
}

void OpenPakSignInDialog::ShowError(const QString& text) {
    error_label->setText(text);
    error_label->setVisible(!text.isEmpty());
}

void OpenPakSignInDialog::Submit() {
    if (!submitter) {
        accept();
        return;
    }
    if (running) {
        return;
    }
    running = true;
    ShowError({});
    UpdateReady();
    busy->show();
    QPointer<OpenPakSignInDialog> self{this};
    std::thread([self, work = submitter, email = Email(), password = Password(),
                 device = DeviceName()] {
        const QString failure = work(email, password, device);
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, failure] {
                if (!self) {
                    return;
                }
                self->running = false;
                self->busy->hide();
                if (failure.isEmpty()) {
                    self->password_edit->clear();
                    self->accept();
                    return;
                }
                self->ShowError(failure);
                self->UpdateReady();
                self->password_edit->setFocus();
                self->password_edit->selectAll();
            },
            Qt::QueuedConnection);
    }).detach();
}

QString OpenPakSignInDialog::Email() const {
    return email_edit->text().trimmed();
}

QString OpenPakSignInDialog::Password() const {
    return password_edit->text();
}

QString OpenPakSignInDialog::DeviceName() const {
    return device_edit->text().trimmed();
}
