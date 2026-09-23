// SPDX-License-Identifier: GPL-2.0-or-later
#include "openpak/qt/crash_report_prompt.h"

#include <thread>

#include <QCheckBox>
#include <QCoreApplication>
#include <QMessageBox>
#include <QPushButton>

#include "openpak/crash_reports.h"
#include "openpak/log.h"

namespace openpak::qt {
namespace {
QString Tr(const char* text) {
    return QCoreApplication::translate("OpenPakCrashReports", text);
}
} // namespace

void OfferCrashReports(QWidget* parent) {
    auto reports = CrashReports::Pending();
    if (reports.empty()) {
        return;
    }
    const std::string policy = CrashReports::Policy();
    if (policy == CrashReports::kNever) {
        CrashReports::DiscardAll();
        return;
    }
    if (policy != CrashReports::kAlways) {
        QString last = QStringLiteral("-");
        for (auto it = reports.rbegin(); it != reports.rend(); ++it) {
            if (!it->message.empty()) {
                last = QString::fromStdString(it->message);
                break;
            }
        }
        QMessageBox box(parent);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(Tr("Send a crash report?"));
        box.setText(Tr("The emulator crashed last time (%n report(s) saved). Send to OpenPak to "
                       "help fix it?")
                        .replace(QStringLiteral("%n"), QString::number(reports.size())));
        box.setInformativeText(
            Tr("A report holds the emulator version, your OS, the game that was running and the "
               "last lines of the log. If you are signed in, it is sent with your account.\n\n"
               "Last error: %1")
                .arg(last));
        auto* send = box.addButton(Tr("Send"), QMessageBox::AcceptRole);
        auto* dont = box.addButton(Tr("Don't send"), QMessageBox::RejectRole);
        box.setDefaultButton(dont);
        auto* remember = new QCheckBox(Tr("Remember my choice"));
        box.setCheckBox(remember);
        box.exec();

        const bool yes = box.clickedButton() == send;
        if (remember->isChecked()) {
            CrashReports::SetPolicy(yes ? CrashReports::kAlways : CrashReports::kNever);
        }
        if (!yes) {
            CrashReports::DiscardAll();
            return;
        }
    }
    // The network is not waited on from the UI thread. A report that fails stays on disk and is
    // offered again next time.
    std::thread{[reports = std::move(reports)] {
        std::size_t sent = 0;
        for (const auto& report : reports) {
            if (CrashReports::Send(report)) {
                CrashReports::Discard(report);
                ++sent;
            }
        }
        OPENPAK_LOG_INFO("Crash reports sent: {} of {}", sent, reports.size());
    }}.detach();
}
} // namespace openpak::qt
