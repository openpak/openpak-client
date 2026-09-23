// SPDX-License-Identifier: GPL-2.0-or-later
// The launch after a crash: offer what openpak/crash_reports.h saved, and send it only on a yes,
// this time or always. The same question Ryujinx asks (OpenPakCrashReportOffer), for every Qt
// host.
#pragma once
class QWidget;
namespace openpak::qt {
// Call once the main window is up. Returns at once when nothing is waiting or the answer is
// already "never"; otherwise asks (unless "always"), then sends on a worker thread.
void OfferCrashReports(QWidget* parent);
} // namespace openpak::qt
