// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDialog>
#include <QString>

class QLineEdit;

// Email and password, asked together. It only asks: the host does the signing in, and shows
// this again with the reason when that fails.
class OpenPakSignInDialog : public QDialog {
public:
    // intro is a line saying what to do before signing in (the host's setup wizard uses it after
    // sending somebody to create an account); error is why the previous attempt failed, email
    // what was typed then.
    explicit OpenPakSignInDialog(QWidget* parent, const QString& intro = {}, const QString& error = {},
                                 const QString& email = {});

    QString Email() const;
    QString Password() const;

private:
    QLineEdit* email_edit;
    QLineEdit* password_edit;
};
