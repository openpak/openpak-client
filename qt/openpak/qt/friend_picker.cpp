// SPDX-License-Identifier: GPL-2.0-or-later
#include "openpak/qt/friend_picker.h"

#include <algorithm>

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QMetaObject>
#include <QPointer>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

#include <fmt/format.h>

#include "openpak/api.h"
#include "openpak/baas.h"
#include "openpak/my_page.h"
#include "openpak/qt/controller_navigation.h"
#include "openpak/qt/host.h"
#include "openpak/qt/pages_p.h"

namespace openpak::qt {
namespace {

constexpr int kPicture = 34;

// The avatar with its presence dot, green when online (UX spec §6).
QPixmap WithDot(const QPixmap& picture, bool online) {
    QPixmap out = picture;
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(24, 24, 28), 2));
    painter.setBrush(online ? QColor(0x3F, 0xB9, 0x50) : QColor(0x7A, 0x7A, 0x7A));
    painter.drawEllipse(QRectF(kPicture - 12, kPicture - 12, 11, 11));
    return out;
}

} // namespace

std::vector<std::uint64_t> PickFriends(Host* host, QWidget* parent, int max) {
    if (!host) {
        host = Host::Current();
    }
    max = (std::max)(max, 1);

    QDialog dialog(parent);
    dialog.setWindowTitle(Tr("Invite friends"));
    dialog.setMinimumWidth(380);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(10);
    auto* hint_text = new QLabel(max == 1 ? Tr("Choose a friend to invite.")
                                          : Tr("Choose up to %1 to invite.").arg(max));
    hint_text->setWordWrap(true);
    layout->addWidget(hint_text);

    auto* list = new QListWidget;
    list->setIconSize(QSize(kPicture, kPicture));
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(list, 1);

    // Online friends first: they are the ones an invitation reaches now.
    auto friends = *openpak::baas::Friends();
    std::stable_sort(friends.begin(), friends.end(), [](const auto& a, const auto& b) {
        if ((a.state != 0) != (b.state != 0)) {
            return a.state != 0;
        }
        return QString::fromStdString(a.nickname).localeAwareCompare(QString::fromStdString(b.nickname)) < 0;
    });
    for (const auto& f : friends) {
        const QString name = QString::fromStdString(f.nickname);
        QString status = Tr("Offline");
        if (f.state == 2) {
            const QString game = f.application_id != 0
                                     ? GameName(host, fmt::format("{:016x}", f.application_id))
                                     : QString{};
            status = game.isEmpty() ? Tr("Playing") : Tr("Playing %1").arg(game);
        } else if (f.state == 1) {
            status = Tr("Online");
        }
        auto* item = new QListWidgetItem(QIcon(WithDot(Initials(name, kPicture), f.state != 0)),
                                         QStringLiteral("%1\n%2").arg(name, status));
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(f.id));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        list->addItem(item);
        if (!f.thumbnail_url.empty()) {
            Picture(list, "thumb:" + f.thumbnail_url,
                    [url = f.thumbnail_url] { return WebService::OpenPakApi::FetchSitePath(url); },
                    [item, online = f.state != 0](const QPixmap& picture) {
                        item->setIcon(QIcon(WithDot(Round(picture, kPicture), online)));
                    });
        }
    }
    if (friends.empty()) {
        hint_text->setText(Tr("You have no friends to invite yet."));
    }

    auto* buttons = new QDialogButtonBox;
    QPushButton* invite = buttons->addButton(Tr("Send invite"), QDialogButtonBox::AcceptRole);
    QPushButton* cancel = buttons->addButton(Tr("Cancel"), QDialogButtonBox::RejectRole);
    invite->setEnabled(false);
    invite->setDefault(true);
    layout->addWidget(buttons);

    auto* hint = new QLabel;
    hint->setEnabled(false); // the muted palette: a hint, not content
    layout->addWidget(hint);

    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    // Checkboxes: with a maximum of one they behave like radio buttons; once the game's maximum
    // is reached, the rows not picked are disabled until one is let go (UX spec §3.7).
    bool updating = false;
    const auto picked_count = [list] {
        int count = 0;
        for (int row = 0; row < list->count(); ++row) {
            count += list->item(row)->checkState() == Qt::Checked ? 1 : 0;
        }
        return count;
    };
    QObject::connect(list, &QListWidget::itemChanged, &dialog, [&, max](QListWidgetItem* changed) {
        if (updating) {
            return;
        }
        updating = true;
        if (max == 1 && changed->checkState() == Qt::Checked) {
            for (int row = 0; row < list->count(); ++row) {
                if (list->item(row) != changed) {
                    list->item(row)->setCheckState(Qt::Unchecked);
                }
            }
        }
        const int count = picked_count();
        const bool full = max > 1 && count >= max;
        for (int row = 0; row < list->count(); ++row) {
            QListWidgetItem* item = list->item(row);
            const bool enable = !full || item->checkState() == Qt::Checked;
            item->setFlags(enable ? item->flags() | Qt::ItemIsEnabled : item->flags() & ~Qt::ItemIsEnabled);
        }
        invite->setEnabled(count > 0);
        updating = false;
    });
    const auto toggle = [list](QListWidgetItem* item) {
        if (item && (item->flags() & Qt::ItemIsEnabled)) {
            item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
        }
    };
    QObject::connect(list, &QListWidget::itemActivated, &dialog, toggle);

    Navigation* navigation = host ? host->CreateNavigation(&dialog) : nullptr;
    if (navigation) {
        hint->setText(Tr("A: choose   B: cancel   Down past the list: Send invite"));

        const auto on_buttons = [&dialog, buttons] {
            QWidget* focused = QApplication::focusWidget();
            return focused && dialog.isAncestorOf(focused) && buttons->isAncestorOf(focused);
        };
        const auto focus_buttons = [invite, cancel] {
            (invite->isEnabled() ? static_cast<QWidget*>(invite) : cancel)->setFocus();
        };

        QObject::connect(navigation, &Navigation::navigated, &dialog,
                         [&, on_buttons, focus_buttons](int dx, int dy) {
                             if (on_buttons()) {
                                 if (dy < 0 && list->count() > 0) {
                                     list->setFocus();
                                 } else if (dx != 0) {
                                     QWidget* other = QApplication::focusWidget() == invite
                                                          ? static_cast<QWidget*>(cancel)
                                                          : invite;
                                     if (other->isEnabled()) {
                                         other->setFocus();
                                     }
                                 }
                                 return;
                             }
                             const int row = list->currentRow() + dy;
                             if (dx > 0 || list->count() == 0 || row >= list->count()) {
                                 focus_buttons();
                                 return;
                             }
                             list->setFocus();
                             list->setCurrentRow((std::max)(row, 0), QItemSelectionModel::NoUpdate);
                             list->scrollToItem(list->currentItem());
                         });
        QObject::connect(
            navigation, &Navigation::activated, &dialog,
            [&, on_buttons, toggle] {
                if (on_buttons()) {
                    if (auto* button =
                            qobject_cast<QAbstractButton*>(QApplication::focusWidget())) {
                        button->click();
                    }
                    return;
                }
                toggle(list->currentItem());
            },
            Qt::QueuedConnection);
        QObject::connect(navigation, &Navigation::backPressed, &dialog, &QDialog::reject);
    }

    if (list->count() > 0) {
        list->setFocus();
        list->setCurrentRow(0, QItemSelectionModel::NoUpdate);
    } else {
        cancel->setFocus();
    }

    // The controller that drives the picker must not also drive the game behind it.
    if (host) {
        host->SetGuestInputSuspended(true);
    }
    const bool accepted = dialog.exec() == QDialog::Accepted;
    if (host) {
        host->SetGuestInputSuspended(false);
    }
    if (!accepted) {
        return {};
    }
    std::vector<std::uint64_t> picked;
    for (int row = 0; row < list->count(); ++row) {
        if (list->item(row)->checkState() == Qt::Checked) {
            picked.push_back(list->item(row)->data(Qt::UserRole).toULongLong());
        }
    }
    return picked;
}

void InstallFriendPicker(Host* host, QWidget* parent) {
    openpak::my_page::SetFriendPicker(
        [host = QPointer<Host>(host), window = QPointer<QWidget>(parent)](int max) {
            std::vector<std::uint64_t> picked;
            if (window) {
                QMetaObject::invokeMethod(
                    window, [&] { picked = PickFriends(host, window, max); },
                    Qt::BlockingQueuedConnection);
            }
            return picked;
        });
}

} // namespace openpak::qt
