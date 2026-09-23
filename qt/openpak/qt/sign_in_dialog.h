// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>

#include <QDialog>
#include <QString>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;

// Sign in to OpenPak (UX spec §3.3): email, password, the device name and a Create an account
// link. Two ways to use it. Without a submitter it only asks: the host reads the fields after
// exec() and shows it again with the reason when signing in fails. With one (SetSubmitter), Sign
// in runs it off the UI thread with the button disabled and a busy bar beside it; an error keeps
// the dialog open, shown inline with the email kept, and success closes it.
class OpenPakSignInDialog : public QDialog {
public:
    // intro is a line saying what to do before signing in (the host's setup wizard uses it after
    // sending somebody to create an account); error is why the previous attempt failed, email
    // what was typed then.
    explicit OpenPakSignInDialog(QWidget* parent, const QString& intro = {}, const QString& error = {},
                                 const QString& email = {});

    // Shows the Device name field, filled with device_default ("{emulator} on {machine}").
    void SetDeviceName(const QString& device_default);

    // Runs on a worker thread with (email, password, device name); returns empty on success, else
    // the text to show.
    using Submitter = std::function<QString(QString, QString, QString)>;
    void SetSubmitter(Submitter submitter);

    QString Email() const;
    QString Password() const;
    QString DeviceName() const;

private:
    void Submit();
    void ShowError(const QString& text);
    void UpdateReady();

    QLineEdit* email_edit;
    QLineEdit* password_edit;
    QLineEdit* device_edit;
    QLabel* device_label;
    QLabel* device_hint;
    QLabel* error_label;
    QPushButton* submit;
    QProgressBar* busy;
    Submitter submitter;
    bool running = false;
};
