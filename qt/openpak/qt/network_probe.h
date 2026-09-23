// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QObject>
#include <QString>


// One-shot NAT-type test (the account window's "NAT" indicator) and backend latency check (its
// "Ping" indicator). Both are fire-and-forget; results come back via signals.
//
// The NAT test is openpak::nat (client/openpak/nat_check.h): the console's own Test Connection
// exchange, the nncs protocol, run from this machine against the nncs1/nncs2 pair the DNS
// redirect sends a game to -- rather than a UPnP IGD query, which only ever reports something for
// players whose router has UPnP enabled and says nothing about whether P2P gets through.
class ExternalReferenceProbe : public QObject {
    Q_OBJECT

public:
    /// Open is type A or B (one mapping for every destination), Strict is C or D, Unknown is F.
    enum class NatStatus { Checking, Open, Strict, Unknown };

    explicit ExternalReferenceProbe(QObject* parent = nullptr);
    ~ExternalReferenceProbe() override;

    void ProbeNat();
    void PingBackend();

    /// The console's letter from the last finished test (A-F), or '?' before one.
    QChar LastType() const { return last_type; }

    /// Mapping, filtering and the address the servers saw, for a tooltip.
    QString LastDetail() const { return last_detail; }

signals:
    void NatStatusChanged(ExternalReferenceProbe::NatStatus status);
    void PingResult(int ms); // -1 on failure

private:
    QChar last_type{u'?'};
    QString last_detail;
    bool nat_running = false;
};
