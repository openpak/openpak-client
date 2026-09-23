// SPDX-License-Identifier: GPL-2.0-or-later
#include "openpak/qt/friend_picker.h"

#include <algorithm>

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include "openpak/baas.h"
#include "openpak/my_page.h"
#include "openpak/qt/controller_navigation.h"
#include "openpak/qt/host.h"

namespace openpak::qt {
namespace {

QString Tr(const char* text) {
    return QCoreApplication::translate("OpenPakFriendPicker", text);
}

} // namespace

std::vector<std::uint64_t> PickFriends(Host* host, QWidget* parent, int max) {
    if (!host) {
        host = Host::Current();
    }
    max = std::max(max, 1);

    QDialog dialog(parent);
    dialog.setWindowTitle(Tr("Invite friends"));
    dialog.setMinimumWidth(360);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(max == 1 ? Tr("Invite a friend to play:")
                                          : Tr("Invite up to %1 friends to play:").arg(max)));

    auto* list = new QListWidget;
    list->setSelectionMode(max == 1 ? QAbstractItemView::SingleSelection
                                    : QAbstractItemView::MultiSelection);
    layout->addWidget(list);

    // Online friends first: they are the ones an invitation reaches now.
    auto friends = *openpak::baas::Friends();
    std::stable_sort(friends.begin(), friends.end(),
                     [](const auto& a, const auto& b) { return a.state > b.state; });
    for (const auto& f : friends) {
        const QString status = f.state == 2   ? Tr("playing")
                               : f.state == 1 ? Tr("online")
                                              : Tr("offline");
        auto* item = new QListWidgetItem(
            QStringLiteral("%1 (%2)").arg(QString::fromStdString(f.nickname), status));
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(f.id));
        list->addItem(item);
    }
    if (friends.empty()) {
        layout->insertWidget(1, new QLabel(Tr("You have no friends on OpenPak yet.")));
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    QPushButton* invite = buttons->addButton(Tr("Invite"), QDialogButtonBox::AcceptRole);
    QPushButton* cancel = buttons->button(QDialogButtonBox::Cancel);
    invite->setEnabled(false);
    layout->addWidget(buttons);

    auto* hint = new QLabel;
    hint->setEnabled(false); // the muted palette: a hint, not content
    layout->addWidget(hint);

    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(list, &QListWidget::itemSelectionChanged, &dialog, [list, invite, max] {
        // More than the game allows is refused here, not silently trimmed later: the pick that
        // went over (the current item) is the one undone.
        if (list->selectedItems().size() > max && list->currentItem()) {
            list->currentItem()->setSelected(false);
        }
        invite->setEnabled(!list->selectedItems().isEmpty());
    });

    Navigation* navigation = host ? host->CreateNavigation(&dialog) : nullptr;
    if (navigation) {
        hint->setText(Tr("A: pick a friend    B: cancel    Down past the list: Invite"));

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
                             list->setCurrentRow(std::max(row, 0), QItemSelectionModel::NoUpdate);
                             list->scrollToItem(list->currentItem());
                         });
        QObject::connect(
            navigation, &Navigation::activated, &dialog,
            [&, on_buttons] {
                if (on_buttons()) {
                    if (auto* button =
                            qobject_cast<QAbstractButton*>(QApplication::focusWidget())) {
                        button->click();
                    }
                    return;
                }
                QListWidgetItem* item = list->currentItem();
                if (!item) {
                    return;
                }
                if (max == 1) {
                    list->clearSelection();
                    item->setSelected(true);
                } else {
                    item->setSelected(!item->isSelected());
                }
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
    for (const auto* item : list->selectedItems()) {
        picked.push_back(item->data(Qt::UserRole).toULongLong());
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
