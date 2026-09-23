// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

// The account window's list pages: Invitations, Cloud saves, Mods, News and Status, as the
// reference draws them. Their network work goes through the window's one queue; what comes back
// is drawn on the UI thread, where the host is asked for names, icons and save folders.

#include "openpak/qt/extra_pages.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTcpSocket>
#include <QTimer>
#include <QVBoxLayout>

#include <fmt/format.h>

#ifdef OPENPAK_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

#include "openpak/account.h"
#include "openpak/api.h"
#include "openpak/compatibility.h"
#include "openpak/compatible_titles.h"
#include "openpak/friends_cache.h"
#include "openpak/platform.h"
#include "openpak/qt/account_page_p.h"
#include "openpak/qt/host.h"
#include "openpak/qt/pages_p.h"
#include "openpak/qt/save_sync.h"
#include "openpak/session.h"

namespace openpak::qt {

namespace Api = WebService::OpenPakApi;

using Platform::ContainedPath;

namespace {

std::string Hex(u64 title_id) {
    return fmt::format("{:016x}", title_id);
}

u64 IdOf(const std::string& title_id_hex) {
    try {
        return std::stoull(title_id_hex, nullptr, 16);
    } catch (const std::exception&) {
        return 0;
    }
}

// A name for a title even the catalogue does not know, rather than its id.
QString NameOrUnknown(Host* host, const std::string& title_id_hex) {
    const QString name = GameName(host, title_id_hex);
    return name.isEmpty() ? Tr("Unknown game") : name;
}

QVBoxLayout* Frame(Page* page, QHBoxLayout*& header) {
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(10);
    header = new QHBoxLayout;
    layout->addLayout(header);
    return layout;
}

// The title picker Cloud saves, Mods and News share: the library, else the titles OpenPak is
// known to serve that the catalogue can name. Filled once; after that it only follows the shared
// choice.
void FillTitles(QComboBox* picker, const PageContext& context) {
    if (picker->count() == 0) {
        std::vector<Host::Title> titles = context.host->InstalledTitles();
        if (titles.empty()) {
            for (const auto& [title_id, version] : OpenPak::CompatibleTitles::Table()) {
                const QString name = GameName(context.host, Hex(title_id));
                if (!name.isEmpty()) {
                    titles.push_back({title_id, name});
                }
            }
        }
        std::sort(titles.begin(), titles.end(), [](const Host::Title& left, const Host::Title& right) {
            return left.name.localeAwareCompare(right.name) < 0;
        });
        const QSignalBlocker blocker{picker};
        for (const Host::Title& title : titles) {
            picker->addItem(title.name, QString::fromStdString(Hex(title.id)));
        }
        if (context.title->empty()) {
            const int running = picker->findData(
                QString::fromStdString(context.host->GetLocalAppId()).toLower());
            picker->setCurrentIndex(std::max(running, 0));
            *context.title = picker->currentData().toString().toStdString();
        }
    }
    const int chosen = picker->findData(QString::fromStdString(*context.title));
    if (chosen >= 0 && chosen != picker->currentIndex()) {
        const QSignalBlocker blocker{picker};
        picker->setCurrentIndex(chosen);
    }
}

// A dot in a state's colour: up, down, bad.
QLabel* Dot(int state, int size = 10) {
    auto* dot = new QLabel(QStringLiteral("\xE2\x97\x8F"));
    QFont font = dot->font();
    font.setPixelSize(size + 4);
    dot->setFont(font);
    dot->setStyleSheet(QStringLiteral("color: %1;").arg(state > 0   ? QStringLiteral("#3FB950")
                                                        : state == 0 ? QStringLiteral("#7A7A7A")
                                                                     : QStringLiteral("#B54141")));
    return dot;
}

// A row with a dot, a name and a line under it, for the Status page's two columns.
QFrame* StateRow(bool up, const QString& name, const QString& detail, const QString& trailing = {}) {
    auto* row = new QFrame;
    row->setObjectName(QStringLiteral("row"));
    row->setStyleSheet(style::Card());
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(12, 6, 12, 6);
    layout->addWidget(Dot(up ? 1 : 0), 0, Qt::AlignTop);
    auto* text = new QVBoxLayout;
    text->setSpacing(0);
    text->addWidget(BoldLabel(name));
    if (!detail.isEmpty()) {
        text->addWidget(DimLabel(detail));
    }
    layout->addLayout(text, 1);
    if (!trailing.isEmpty()) {
        layout->addWidget(DimLabel(trailing), 0, Qt::AlignTop);
    }
    return row;
}

// The invitation's words in the language the UI speaks: the exact tag, then the language alone,
// then English, then whatever the game wrote.
QString MessageFor(const std::map<std::string, std::string>& messages) {
    if (messages.empty()) {
        return {};
    }
    const std::string tag = QLocale().bcp47Name().toStdString();
    const std::string language = tag.substr(0, tag.find('-'));
    const auto starts = [](const std::string& text, const std::string& prefix) {
        return text.size() >= prefix.size() &&
               std::equal(prefix.begin(), prefix.end(), text.begin(), [](char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a)) ==
                          std::tolower(static_cast<unsigned char>(b));
               });
    };
    for (const std::string& wanted : {tag, language, std::string{"en-US"}, std::string{"en"}}) {
        for (const auto& [key, text] : messages) {
            if (starts(key, wanted) && (key.size() == wanted.size() || key[wanted.size()] == '-')) {
                return QString::fromStdString(text);
            }
        }
    }
    return QString::fromStdString(messages.begin()->second);
}

#ifdef OPENPAK_HAVE_LIBARCHIVE
// Unpack a verified package into dir, replacing what was there. False -- and nothing left
// behind -- when an entry names its way out of dir: the package came off the network.
bool UnpackMod(const std::vector<u8>& package, const std::filesystem::path& dir) {
    std::error_code ec;
    // An upgrade replaces the mod rather than merging into it: files the new version dropped
    // would otherwise stay behind and keep being applied.
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    struct archive* zip = archive_read_new();
    archive_read_support_format_zip(zip);
    bool ok = archive_read_open_memory(zip, package.data(), package.size()) == ARCHIVE_OK;

    struct archive_entry* entry = nullptr;
    while (ok && archive_read_next_header(zip, &entry) == ARCHIVE_OK) {
        const char* name = archive_entry_pathname(entry);
        const std::filesystem::path target = ContainedPath(dir, name ? name : "");
        if (target.empty()) {
            ok = false;
            break;
        }
        if (archive_entry_filetype(entry) == AE_IFDIR) {
            std::filesystem::create_directories(target, ec);
            continue;
        }
        if (archive_entry_filetype(entry) != AE_IFREG) {
            continue; // a mod has no business carrying links or devices
        }
        std::filesystem::create_directories(target.parent_path(), ec);
        std::ofstream out{target, std::ios::binary | std::ios::trunc};
        const void* block = nullptr;
        size_t size = 0;
        la_int64_t offset = 0;
        while (archive_read_data_block(zip, &block, &size, &offset) == ARCHIVE_OK) {
            out.write(static_cast<const char*>(block), static_cast<std::streamsize>(size));
        }
        ok = out.good();
    }
    archive_read_free(zip);

    if (!ok) {
        std::filesystem::remove_all(dir, ec);
    }
    return ok;
}
#endif

// A slug is already tame, but it is still going on somebody's filesystem.
std::string ModDirName(const Api::Mod& mod) {
    std::string name = mod.slug.empty() ? mod.name : mod.slug;
    for (char& c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != ' ') {
            c = '-';
        }
    }
    return name.find_first_not_of(" -.") == std::string::npos ? "openpak-mod" : name;
}

} // namespace

Page* MakeInvitationsPage(const PageContext& context) {
    struct Waiting {
        std::vector<Api::CoreInvitation> core;
        std::vector<client::session::Invitation> native;
        std::vector<Common::OpenPakFriends::PendingInvitation> queued;
    };

    auto* page = new Page;
    QHBoxLayout* header = nullptr;
    QVBoxLayout* layout = Frame(page, header);
    QPushButton* refresh = TextButton(Tr("Refresh"));
    header->addStretch(1);
    header->addWidget(refresh);
    auto* list = new RowList;
    layout->addWidget(list, 1);

    // One row per invitation: who sent it, which game, what they wrote and when; what differs
    // between the website's and the console's is how it is turned down.
    const auto add = [=](const std::string& from, const std::string& title_id, const QString& message,
                         const QString& when, std::function<std::string()> decline) {
        const u64 id = IdOf(title_id);
        const QString game = GameName(context.host, title_id);
        const QString sender = from.empty() ? Tr("Someone") : QString::fromStdString(from);

        auto* row = new QFrame;
        row->setObjectName(QStringLiteral("row"));
        row->setStyleSheet(style::Card());
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(14, 8, 14, 8);
        row_layout->setSpacing(10);

        // The sender's picture when they are a friend the site has one for, else their initial.
        auto* avatar = new QLabel;
        avatar->setFixedSize(34, 34);
        avatar->setPixmap(Initials(sender, 34));
        for (const auto& known : context.friends->friends) {
            if (!known.account_id.empty() && known.display_name == from) {
                Picture(avatar, "avatar:" + known.account_id,
                        [account = known.account_id] { return Api::GetAccountAvatar(account); },
                        [avatar](const QPixmap& pixmap) { avatar->setPixmap(Round(pixmap, 34)); });
                break;
            }
        }
        row_layout->addWidget(avatar, 0, Qt::AlignTop);

        // The game's icon, else a placeholder with the game's initial.
        auto* icon = new QLabel;
        icon->setFixedSize(42, 42);
        icon->setPixmap(RoundedRect(Initials(game.isEmpty() ? QStringLiteral("?") : game, 42), 42, 6));
        TitleIcon(icon, context.host, title_id,
                  [icon](const QPixmap& pixmap) { icon->setPixmap(RoundedRect(pixmap, 42, 6)); });
        row_layout->addWidget(icon, 0, Qt::AlignTop);

        auto* text = new QVBoxLayout;
        text->setSpacing(2);
        QLabel* title = BoldLabel(game.isEmpty() ? Tr("A game that is not in your library") : game);
        title->setWordWrap(true);
        text->addWidget(title);
        text->addWidget(DimLabel(Tr("From %1").arg(sender)));
        if (!message.isEmpty()) {
            QLabel* words = new QLabel(QStringLiteral("“%1”").arg(message));
            words->setWordWrap(true);
            text->addWidget(words);
        }
        if (!when.isEmpty()) {
            text->addWidget(DimLabel(when));
        }
        row_layout->addLayout(text, 1);

        // Join launches the title and closes the window first, as the reference does; the host's
        // own poll is what hands the join payload to the guest once it runs.
        QPushButton* join = TextButton(Tr("Join"));
        QObject::connect(join, &QPushButton::clicked, page, [=] {
            if (id == 0 || !Installed(context.host, id)) {
                context.status(Tr("%1 is not in your library.")
                                   .arg(game.isEmpty() ? Tr("That game") : game));
                return;
            }
            Host* host = context.host;
            context.close();
            host->QuickStart(id);
        });
        QPushButton* ignore = TextButton(Tr("Ignore"));
        QObject::connect(ignore, &QPushButton::clicked, page, [=] {
            ignore->setEnabled(false);
            context.queue->Run(page, decline, [=](std::string failure) {
                context.status(QString::fromStdString(failure));
                page->reload(true);
            });
        });
        row_layout->addWidget(join, 0, Qt::AlignVCenter);
        row_layout->addWidget(ignore, 0, Qt::AlignVCenter);
        list->Add(row);
    };

    page->reload = [=](bool) {
        if (!SignedIn()) {
            list->Clear();
            list->AddEmpty(Tr("Sign in to OpenPak to use this."));
            return;
        }
        context.queue->Run(
            page,
            [] {
                compatibility::LoadNames();
                client::session::RefreshInvitations();
                return Waiting{Api::GetInvitations(), client::session::Invitations(),
                               Common::OpenPakFriends::PeekPendingInvitations()};
            },
            [=](Waiting waiting) {
                list->Clear();
                for (const auto& one : waiting.core) {
                    const s64 expires = QDateTime::fromString(QString::fromStdString(one.expires_at),
                                                              Qt::ISODate)
                                            .toSecsSinceEpoch();
                    add(one.from, one.title_id, {},
                        expires > 0 ? Tr("Expires %1").arg(Until(expires)) : QString{},
                        [id = one.id] { return Api::DeclineInvitation(id); });
                }
                for (const auto& one : waiting.native) {
                    add(one.sender_name, one.title_id, MessageFor(one.messages),
                        one.created_at > 0 ? Tr("Sent %1").arg(Ago(one.created_at)) : QString{},
                        [id = one.id] {
                            client::session::DismissInvitation(id);
                            return std::string{};
                        });
                }
                for (const auto& one : waiting.queued) {
                    list->AddRow(Tr("From %1").arg(QString::fromStdString(one.from_name)),
                                 {Tr("Ready: start or resume the game and it will pick this up.")}, {});
                }
                if (waiting.core.empty() && waiting.native.empty() && waiting.queued.empty()) {
                    list->AddEmpty(Tr("Nothing waiting."));
                }
            });
    };
    QObject::connect(refresh, &QPushButton::clicked, page, [page] { page->reload(true); });
    return page;
}

Page* MakeSavesPage(const PageContext& context) {
    struct Row {
        Api::CloudSave save;
        bool installed = false;
        std::filesystem::path dir;
        OpenPak::SaveSync::LocalCopy local;
    };
    struct Loaded {
        Api::CloudSaves list;
        std::vector<Row> rows;
    };

    auto* page = new Page;
    QHBoxLayout* header = nullptr;
    QVBoxLayout* layout = Frame(page, header);
    auto* picker = new QComboBox;
    QPushButton* upload = TextButton(Tr("Upload"));
    upload->setMinimumWidth(90);
    upload->setToolTip(Tr("Upload this title's save; a title's first save goes up this way."));
    QPushButton* refresh = TextButton(Tr("Refresh"));
    header->addWidget(picker, 1);
    header->addWidget(upload);
    header->addWidget(refresh);
    QLabel* usage = DimLabel({});
    layout->addWidget(usage);
    auto* list = new RowList;
    layout->addWidget(list, 1);

    // Governs only the automatic pull before launch and push on exit; the buttons here are an
    // explicit choice each time.
    auto* automatic = new QCheckBox(Tr("Sync cloud saves automatically when a game starts and stops"));
    automatic->setChecked(context.host->CloudSyncEnabled());
    QObject::connect(automatic, &QCheckBox::toggled, page,
                     [host = context.host](bool on) { host->SetCloudSyncEnabled(on); });
    layout->addWidget(automatic);

    // The newest cloud version of each title as last drawn: an upload is based on it.
    auto newest = std::make_shared<std::map<std::string, int>>();

    const auto after = [=](QString done) {
        return [=](std::string failure) {
            context.status(failure.empty() ? done : QString::fromStdString(failure));
            page->reload(true);
        };
    };
    const auto upload_title = [=](u64 id, const QString& name) {
        const auto dir = context.host->SaveDirectory(id);
        const auto it = newest->find(Hex(id));
        const int base = it == newest->end() ? 0 : it->second;
        context.queue->Run(page, [dir, id, base] { return OpenPak::SaveSync::Upload(dir, id, base); },
                           after(Tr("Uploaded the save for %1.").arg(name)));
    };

    const auto add = [=](const Row& row) {
        const Api::CloudSave& save = row.save;
        const u64 id = IdOf(save.title_id);
        QString name = GameName(context.host, save.title_id);
        if (name.isEmpty() && !save.name.empty() && IdOf(save.name) != id) {
            name = QString::fromStdString(save.name); // the site's, when it is not the id
        }
        if (name.isEmpty()) {
            name = Tr("Unknown game");
        }

        u64 total = 0;
        bool conflict = false;
        for (const auto& version : save.versions) {
            total += version.size;
            conflict = conflict || version.conflict;
        }
        QStringList lines{QCoreApplication::translate("OpenPakAccountDialog",
                                                      "%1 in the cloud across %n version(s)",
                                                      nullptr, static_cast<int>(save.versions.size()))
                              .arg(Bytes(total))};
        if (!save.versions.empty()) {
            const auto& top = save.versions.front();
            const s64 when = QDateTime::fromString(QString::fromStdString(top.saved_at), Qt::ISODate)
                                 .toSecsSinceEpoch();
            QString detail = Tr("Version %1").arg(top.number);
            if (!top.device.empty()) {
                detail += QLatin1Char(' ') + Tr("from %1").arg(QString::fromStdString(top.device));
            }
            if (when > 0) {
                detail += QStringLiteral(" — ") + LocalTime(when);
            }
            lines << detail;
        }

        using OpenPak::SaveSync::LocalState;
        const auto& local = row.local;
        const QString written = LocalTime(local.last_written);
        if (!row.installed) {
            lines << Tr("Not installed on this machine.");
        } else {
            switch (local.state) {
            case LocalState::NoLocal:
                lines << Tr("Nothing on this machine yet.");
                break;
            case LocalState::NoHistory:
                lines << Tr("On this machine: last written %1, never synced with the cloud. Take "
                            "the cloud copy or keep this one.")
                             .arg(written);
                break;
            case LocalState::InStep:
                lines << Tr("On this machine: last written %1, in step with version %2.")
                             .arg(written, QString::fromStdString(local.version));
                break;
            case LocalState::ChangedHere:
                lines << (local.version.empty()
                              ? Tr("On this machine: last written %1, never synced with the cloud.")
                                    .arg(written)
                              : Tr("On this machine: last written %1, played since version %2.")
                                    .arg(written, QString::fromStdString(local.version)));
                break;
            case LocalState::CloudNewer:
                lines << Tr("On this machine: last written %1, in step with version %2; the cloud "
                            "has a newer one.")
                             .arg(written, QString::fromStdString(local.version));
                break;
            }
        }

        // A save with no shared history, or one the server marked, is a choice between the two
        // copies; the buttons say which way each one goes.
        const bool choose = conflict || local.state == LocalState::NoHistory;
        const std::string running = openpak::Platform::ToLower(context.host->GetLocalAppId());
        std::vector<QWidget*> buttons;
        if (row.installed) {
            QPushButton* download = TextButton(choose ? Tr("Take cloud") : Tr("Download"));
            // Writing a save folder while a game runs is what breaks the running game's mounted
            // filesystem, so the download waits for it to stop, whichever title it is.
            download->setEnabled(running.empty());
            download->setToolTip(running.empty() ? QString{} : Tr("Stop the running game first."));
            QObject::connect(download, &QPushButton::clicked, page, [=] {
                if (!context.confirm(Tr("Cloud saves"),
                                     Tr("Replace the local save for %1 with the cloud copy? The "
                                        "local one is backed up beside it.")
                                         .arg(name),
                                     Tr("Replace"))) {
                    return;
                }
                context.queue->Run(page, [dir = row.dir, id] { return OpenPak::SaveSync::Download(dir, id); },
                                   after(Tr("Downloaded the cloud save for %1.").arg(name)));
            });
            QPushButton* up = TextButton(choose ? Tr("Keep local") : Tr("Upload"));
            up->setEnabled(local.state != LocalState::NoLocal && running != save.title_id);
            QObject::connect(up, &QPushButton::clicked, page, [=] { upload_title(id, name); });
            buttons = {download, up};
        }
        QPushButton* remove = TextButton(Tr("Delete from cloud"));
        QObject::connect(remove, &QPushButton::clicked, page, [=] {
            if (!context.confirm(Tr("Cloud saves"),
                                 QCoreApplication::translate(
                                     "OpenPakAccountDialog",
                                     "Delete the cloud save for %1? All %n stored version(s) go, and "
                                     "OpenPak keeps no copy. The save on this machine is not touched.",
                                     nullptr, static_cast<int>(save.versions.size()))
                                     .arg(name),
                                 Tr("Delete"))) {
                return;
            }
            context.queue->Run(
                page,
                [versions = save.versions] {
                    for (const auto& version : versions) {
                        if (std::string failure = Api::DeleteSaveVersion(version.id); !failure.empty()) {
                            return failure;
                        }
                    }
                    return std::string{};
                },
                [=](std::string failure) {
                    context.status(failure.empty()
                                       ? Tr("Deleted the cloud save for %1.").arg(name)
                                       : Tr("Could not delete the cloud save for %1: %2")
                                             .arg(name, QString::fromStdString(failure)));
                    page->reload(true);
                });
        });
        buttons.push_back(remove);

        QLabel* icon = nullptr;
        QFrame* frame = list->AddRow(name, lines, buttons, {}, &icon);
        if (conflict) {
            auto* badge = new QLabel(Tr("Conflict"));
            badge->setStyleSheet(QStringLiteral("background: #66B54141; border-radius: 3px; "
                                                "padding: 1px 6px; font-size: 11px;"));
            static_cast<QHBoxLayout*>(frame->layout())->insertWidget(2, badge, 0, Qt::AlignTop);
        }
        icon->setPixmap(RoundedRect(Initials(name, 42), 42, 6));
        const auto draw = [icon](const QPixmap& pixmap) { icon->setPixmap(RoundedRect(pixmap, 42, 6)); };
        const QString library = context.host->ResolveGameIcon(fmt::format("{:016X}", id));
        if (!library.isEmpty() || save.icon_path.empty()) {
            TitleIcon(icon, context.host, save.title_id, draw);
        } else {
            Picture(icon, "save-icon:" + save.title_id,
                    [path = save.icon_path] { return Api::FetchSitePath(path); }, draw);
        }
    };

    page->reload = [=](bool) {
        FillTitles(picker, context);
        upload->setEnabled(SignedIn() && picker->count() > 0);
        if (!SignedIn()) {
            list->Clear();
            usage->clear();
            list->AddEmpty(Tr("Sign in to OpenPak to use this."));
            return;
        }
        // Where each installed title keeps its save is the host's to say, on its own thread.
        std::map<std::string, std::filesystem::path> dirs;
        for (const Host::Title& title : context.host->InstalledTitles()) {
            dirs[Hex(title.id)] = context.host->SaveDirectory(title.id);
        }
        context.queue->Run(
            page,
            [dirs] {
                compatibility::LoadNames();
                Loaded loaded{Api::GetCloudSaves(), {}};
                std::sort(loaded.list.saves.begin(), loaded.list.saves.end(),
                          [](const Api::CloudSave& a, const Api::CloudSave& b) {
                              return openpak::Platform::ToLower(a.title_id) <
                                     openpak::Platform::ToLower(b.title_id);
                          });
                // Reading every save folder is disk work, so it happens here and not in the
                // drawing.
                for (const auto& save : loaded.list.saves) {
                    Row row;
                    row.save = save;
                    if (const auto it = dirs.find(openpak::Platform::ToLower(save.title_id));
                        it != dirs.end()) {
                        row.installed = true;
                        row.dir = it->second;
                        row.local = OpenPak::SaveSync::Compare(
                            row.dir, save.versions.empty() ? 0 : save.versions.front().number);
                    }
                    loaded.rows.push_back(std::move(row));
                }
                return loaded;
            },
            [=](Loaded loaded) {
                list->Clear();
                if (!loaded.list.ok) {
                    usage->clear();
                    list->AddEmpty(QString::fromStdString(loaded.list.error));
                    return;
                }
                usage->setText(loaded.list.allowance > 0
                                   ? Tr("%1 of %2 used").arg(Bytes(loaded.list.allowance_used),
                                                             Bytes(loaded.list.allowance))
                                   : QString{});
                newest->clear();
                for (Row& row : loaded.rows) {
                    // A host with no library list still knows its installed titles by icon.
                    const u64 id = IdOf(row.save.title_id);
                    if (!row.installed && Installed(context.host, id)) {
                        row.installed = true;
                        row.dir = context.host->SaveDirectory(id);
                        row.local = OpenPak::SaveSync::Compare(
                            row.dir, row.save.versions.empty() ? 0 : row.save.versions.front().number);
                    }
                    (*newest)[Hex(id)] = row.save.versions.empty() ? 0 : row.save.versions.front().number;
                    add(row);
                }
                if (loaded.rows.empty()) {
                    list->AddEmpty(Tr("Nothing in the cloud yet."));
                }
            });
    };

    QObject::connect(upload, &QPushButton::clicked, page, [=] {
        const u64 id = IdOf(picker->currentData().toString().toStdString());
        if (id == 0) {
            return;
        }
        if (openpak::Platform::ToLower(context.host->GetLocalAppId()) == Hex(id)) {
            context.status(Tr("Stop the game first; its save uploads when it exits."));
            return;
        }
        upload_title(id, picker->currentText());
    });
    QObject::connect(picker, &QComboBox::currentIndexChanged, page,
                     [=] { *context.title = picker->currentData().toString().toStdString(); });
    QObject::connect(refresh, &QPushButton::clicked, page, [page] { page->reload(true); });
    // A pull before launch or a push on exit changes what this page would say.
    QObject::connect(context.host, &Host::StatusChanged, page, [page] {
        if (page->isVisible()) {
            page->reload(false);
        }
    });
    return page;
}

Page* MakeModsPage(const PageContext& context) {
    struct Catalogue {
        std::vector<Api::Mod> mods;
        std::vector<std::string> favourites;
    };

    auto* page = new Page;
    QHBoxLayout* header = nullptr;
    QVBoxLayout* layout = Frame(page, header);
    auto* picker = new QComboBox;
    QPushButton* refresh = TextButton(Tr("Refresh"));
    header->addWidget(picker, 1);
    header->addWidget(refresh);
    auto* list = new RowList;
    layout->addWidget(list, 1);
    auto shown = std::make_shared<std::string>(); // the title the list was last fetched for

    page->reload = [=](bool force) {
        FillTitles(picker, context);
        const std::string title = picker->currentData().toString().toStdString();
        if (title.empty() || (!force && title == *shown)) {
            return;
        }
        *shown = title;
        context.queue->Run(
            page, [title] { return Catalogue{Api::GetMods(title), Api::GetFavouriteModIds()}; },
            [=](Catalogue catalogue) {
                if (picker->currentData().toString().toStdString() != title) {
                    return; // the person moved on while this was in flight
                }
                list->Clear();
                if (catalogue.mods.empty()) {
                    list->AddEmpty(Tr("No mods published for this title."));
                }
                const std::filesystem::path mods_dir = context.host->ModDirectory(IdOf(title));

                for (const Api::Mod& mod : catalogue.mods) {
                    const QString name = QString::fromStdString(mod.name);
                    const std::filesystem::path dir = mods_dir.empty() ? mods_dir : mods_dir / ModDirName(mod);
                    std::error_code ec;
                    const bool installed = !dir.empty() && std::filesystem::exists(dir, ec) &&
                                           !std::filesystem::is_empty(dir, ec);

                    // Nothing is registered anywhere: the host's own mod manager lists what is
                    // on disk, so a mod installed here is disabled or deleted there.
                    QPushButton* install = TextButton(installed ? Tr("Reinstall") : Tr("Install"));
                    QObject::connect(install, &QPushButton::clicked, page, [=] {
#ifdef OPENPAK_HAVE_LIBARCHIVE
                        if (dir.empty()) {
                            context.status(Tr("This build has nowhere to install mods."));
                            return;
                        }
                        install->setEnabled(false);
                        context.queue->Run(
                            page,
                            [mod, dir]() -> int {
                                const auto package = Api::DownloadModPackage(mod);
                                return !package ? 1 : UnpackMod(*package, dir) ? 0 : 2;
                            },
                            [=](int failure) {
                                install->setEnabled(true);
                                if (failure == 0) {
                                    install->setText(Tr("Reinstall"));
                                }
                                context.status(
                                    failure == 0   ? Tr("Installed %1.").arg(name)
                                    : failure == 1 ? Tr("Could not install %1: the download failed or "
                                                        "did not match its published hash.")
                                                         .arg(name)
                                                   : Tr("Could not install %1.").arg(name));
                            });
#else
                        // ponytail: no unzip without libarchive (the mingw cross build); wire
                        // the PowerShell fallback save_sync uses if that build wants mods.
                        context.status(Tr("This build cannot unpack mods."));
#endif
                    });

                    const bool favourite = std::find(catalogue.favourites.begin(), catalogue.favourites.end(),
                                                     mod.id) != catalogue.favourites.end();
                    QPushButton* star = TextButton(favourite ? Tr("\xE2\x98\x85 Favourite") : Tr("\xE2\x98\x86 Favourite"));
                    star->setProperty("favourite", favourite);
                    QObject::connect(star, &QPushButton::clicked, page, [=] {
                        if (!SignedIn()) {
                            context.status(Tr("Sign in to OpenPak to use this."));
                            return;
                        }
                        const bool want = !star->property("favourite").toBool();
                        star->setEnabled(false);
                        context.queue->Run(page, [id = mod.id, want] { return Api::SetModFavourite(id, want); },
                                           [=](bool ok) {
                                               star->setEnabled(true);
                                               if (ok) {
                                                   star->setProperty("favourite", want);
                                                   star->setText(want ? Tr("\xE2\x98\x85 Favourite")
                                                                      : Tr("\xE2\x98\x86 Favourite"));
                                               }
                                           });
                    });

                    list->AddRow(QStringLiteral("%1  %2").arg(name, QString::fromStdString(mod.version)),
                                 {QString::fromStdString(mod.summary),
                                  Tr("by %1, %2").arg(QString::fromStdString(mod.author),
                                                      QString::fromStdString(mod.licence))},
                                 {star, install});
                }
            });
    };
    QObject::connect(refresh, &QPushButton::clicked, page, [page] { page->reload(true); });
    QObject::connect(picker, &QComboBox::currentIndexChanged, page, [=] {
        *context.title = picker->currentData().toString().toStdString();
        page->reload(false);
    });
    return page;
}

Page* MakeNewsPage(const PageContext& context) {
    auto* page = new Page;
    QHBoxLayout* header = nullptr;
    QVBoxLayout* layout = Frame(page, header);
    auto* picker = new QComboBox;
    QPushButton* refresh = TextButton(Tr("Refresh"));
    QPushButton* save = TextButton(Tr("Save to disk..."));
    save->setEnabled(false);
    header->addWidget(picker, 1);
    header->addWidget(save);
    header->addWidget(refresh);
    QLabel* valid_from = DimLabel({});
    layout->addWidget(valid_from);
    auto* list = new RowList;
    layout->addWidget(list, 1);

    auto files = std::make_shared<std::vector<Api::NewsFile>>();
    auto shown = std::make_shared<std::string>();

    page->reload = [=](bool force) {
        FillTitles(picker, context);
        const std::string title = picker->currentData().toString().toStdString();
        if (title.empty() || (!force && title == *shown)) {
            return;
        }
        *shown = title;
        context.queue->Run(page, [title] { return Api::GetNewsManifest(title); }, [=](Api::NewsManifest manifest) {
            if (picker->currentData().toString().toStdString() != title) {
                return;
            }
            list->Clear();
            *files = manifest.files;
            save->setEnabled(!files->empty());
            const s64 from = QDateTime::fromString(QString::fromStdString(manifest.valid_from), Qt::ISODate)
                                 .toSecsSinceEpoch();
            valid_from->setText(manifest.ok && from > 0 ? Tr("From %1").arg(LocalTime(from)) : QString{});
            if (files->empty()) {
                list->AddEmpty(Tr("No news for this title."));
            }
            for (const Api::NewsFile& file : *files) {
                list->AddRow(QString::fromStdString(file.path), {}, {},
                             QLocale().formattedDataSize(static_cast<qint64>(file.size)));
            }
        });
    };

    // bcat is not implemented for the guest, so the dataset a title would receive is shown and
    // saved rather than delivered.
    QObject::connect(save, &QPushButton::clicked, page, [=] {
        const QString chosen = QFileDialog::getExistingDirectory(page, Tr("Save to disk..."));
        if (chosen.isEmpty()) {
            return;
        }
        save->setEnabled(false);
        context.queue->Run(
            page,
            [wanted = *files, root = std::filesystem::path{chosen.toStdU16String()}] {
                int written = 0;
                for (const Api::NewsFile& file : wanted) {
                    // The path came off the network: one that would leave the chosen directory
                    // is skipped, not followed.
                    const std::filesystem::path target = ContainedPath(root, file.path);
                    const auto data = target.empty() ? std::nullopt : Api::DownloadNewsFile(file);
                    if (!data) {
                        continue;
                    }
                    std::error_code ec;
                    std::filesystem::create_directories(target.parent_path(), ec);
                    std::ofstream out{target, std::ios::binary | std::ios::trunc};
                    out.write(reinterpret_cast<const char*>(data->data()),
                              static_cast<std::streamsize>(data->size()));
                    written += out.good() ? 1 : 0;
                }
                return written;
            },
            [=](int written) {
                save->setEnabled(true);
                context.status(Tr("Saved %1 files to %2.").arg(written).arg(chosen));
            });
    });
    QObject::connect(refresh, &QPushButton::clicked, page, [page] { page->reload(true); });
    QObject::connect(picker, &QComboBox::currentIndexChanged, page, [=] {
        *context.title = picker->currentData().toString().toStdString();
        page->reload(false);
    });
    return page;
}

namespace {

// Milliseconds a TCP connect to the console server took, -1 when it did not answer, -2 when no
// server is configured: the edge a console reaches, not the website.
int ProbeEdge(const std::string& address) {
    if (address.empty()) {
        return -2;
    }
    std::string host = address;
    quint16 port = 443;
    if (const auto colon = address.rfind(':');
        colon != std::string::npos && address.find(':') == colon) {
        host = address.substr(0, colon);
        port = static_cast<quint16>(std::atoi(address.c_str() + colon + 1));
    }
    QTcpSocket socket;
    QElapsedTimer clock;
    clock.start();
    socket.connectToHost(QString::fromStdString(host), port);
    const bool answered = socket.waitForConnected(4000);
    const int elapsed = static_cast<int>(clock.elapsed());
    socket.abort();
    return answered ? elapsed : -1;
}

// The state the last presence publish carried: the JSON-Patch body's /presence/state value.
QString PresenceWord(const std::string& body) {
    const auto at = body.find("/presence/state");
    if (at == std::string::npos) {
        return {};
    }
    const auto value = body.find("\"value\"", at);
    const auto open = value == std::string::npos ? value : body.find('"', body.find(':', value) + 1);
    const auto close = open == std::string::npos ? open : body.find('"', open + 1);
    return close == std::string::npos ? QString{}
                                      : QString::fromStdString(body.substr(open + 1, close - open - 1));
}

} // namespace

Page* MakeStatusPage(const PageContext& context) {
    struct Health {
        Api::NetworkStatus players;
        Api::ServiceStatus services;
        int edge = -2;
        std::string address;
    };

    auto* page = new Page;
    QHBoxLayout* header = nullptr;
    QVBoxLayout* layout = Frame(page, header);
    QLabel* players = DimLabel({});
    QPushButton* refresh = TextButton(Tr("Refresh"));
    header->addWidget(players, 1);
    header->addWidget(refresh);

    auto* verdict = new QFrame;
    verdict->setObjectName(QStringLiteral("row"));
    verdict->setStyleSheet(style::Card());
    auto* verdict_layout = new QHBoxLayout(verdict);
    verdict_layout->setContentsMargins(16, 10, 16, 10);
    auto* verdict_dot = Dot(0, 14);
    verdict_layout->addWidget(verdict_dot, 0, Qt::AlignTop);
    auto* verdict_text = new QVBoxLayout;
    QLabel* headline = BoldLabel({}, 2);
    headline->setWordWrap(true);
    QLabel* summary = DimLabel({});
    verdict_text->addWidget(headline);
    verdict_text->addWidget(summary);
    verdict_layout->addLayout(verdict_text, 1);
    layout->addWidget(verdict);

    auto* columns = new QHBoxLayout;
    columns->setSpacing(12);
    const auto column = [&](const QString& title) {
        auto* box = new QVBoxLayout;
        box->addWidget(BoldLabel(title, 1));
        auto* list = new RowList;
        box->addWidget(list, 1);
        columns->addLayout(box, 1);
        return list;
    };
    RowList* services = column(Tr("Services"));
    RowList* session = column(Tr("This session"));
    RowList* population = column(Tr("Players"));
    layout->addLayout(columns, 1);

    // This session's side is read here, on the UI thread, from state the library already holds.
    const auto draw_session = [=](int edge, const std::string& address) {
        session->Clear();
        const bool signed_in = SignedIn();
        const std::string username = Common::OpenPakAccount::GetUsername();
        session->Add(StateRow(signed_in, Tr("OpenPak account"),
                              signed_in ? Tr("Signed in as %1")
                                              .arg(username.empty() ? QStringLiteral("—")
                                                                    : QString::fromStdString(username))
                                        : Tr("Not signed in")));
        const bool linked = client::session::Linked();
        const std::string code = client::session::FriendCode();
        session->Add(StateRow(linked, Tr("Console link"),
                              linked ? Tr("Linked as %1 (%2)")
                                           .arg(QString::fromStdString(client::session::Nickname()),
                                                code.empty() ? QStringLiteral("—")
                                                             : QString::fromStdString(code))
                                     : Tr("No console account is linked.")));
        const QString where = QString::fromStdString(address);
        session->Add(StateRow(edge >= 0, Tr("Console edge"),
                              edge == -2   ? Tr("No console server is configured.")
                              : edge == -1 ? Tr("%1 did not answer").arg(where)
                                           : Tr("%1 answered in %2 ms").arg(where).arg(edge)));
        const bool beating = client::session::Beating();
        const QString word = beating && linked ? PresenceWord(client::session::PresenceBody()) : QString{};
        session->Add(StateRow(!word.isEmpty(), Tr("Presence"),
                              word.isEmpty() ? Tr("Nothing published yet.")
                                             : Tr("Published as %1").arg(word)));
        session->Add(StateRow(
            beating, Tr("Notifications"),
            beating ? Tr("Asked for every 30 seconds; %1 waiting. Nothing here holds a push "
                         "connection.")
                          .arg(static_cast<int>(client::session::Invitations().size()))
                    : Tr("The session is not running, so nothing is asking.")));
    };

    // Public: this answers even when the sign-in is the broken part.
    page->reload = [=](bool) {
        const std::string address = context.host->ServerIp();
        context.queue->Run(
            page,
            [address] {
                compatibility::LoadNames();
                // The three answer independently, so they are asked at once, as the reference asks.
                auto counts = std::async(std::launch::async, [] { return Api::GetStatus(); });
                auto checks = std::async(std::launch::async, [] { return Api::GetServiceStatus(); });
                const int edge = ProbeEdge(address);
                return Health{counts.get(), checks.get(), edge, address};
            },
            [=](Health health) {
                const auto& checks = health.services;
                verdict_dot->setStyleSheet(QStringLiteral("color: %1;").arg(
                    !checks.ok ? QStringLiteral("#7A7A7A")
                    : checks.state == "ok" ? QStringLiteral("#3FB950")
                                           : QStringLiteral("#B54141")));
                headline->setText(checks.ok ? QString::fromStdString(checks.headline)
                                            : Tr("The status box at %1 did not answer.")
                                                  .arg(QString::fromStdString(checks.url)));
                summary->setText(QString::fromStdString(checks.sub));

                services->Clear();
                for (const auto& service : checks.services) {
                    services->Add(StateRow(
                        service.up, QString::fromStdString(service.name),
                        Tr("%1% up, %2")
                            .arg(QLocale().toString(service.uptime, 'f', service.uptime < 100 ? 1 : 0),
                                 QString::fromStdString(service.latency)),
                        service.up ? Tr("Up") : Tr("Down")));
                }
                if (checks.services.empty()) {
                    services->AddEmpty(Tr("No checks to show."));
                }

                draw_session(health.edge, health.address);

                population->Clear();
                if (!health.players.ok) {
                    players->setText(Tr("Could not reach %1.").arg(QString::fromStdString(Api::BaseUrl())));
                    return;
                }
                players->setText(QCoreApplication::translate("OpenPakAccountDialog", "%n player(s) online",
                                                             nullptr, health.players.players_online));
                for (const auto& [title_id, count] : health.players.titles) {
                    population->AddRow(NameOrUnknown(context.host, title_id), {}, {},
                                       QString::number(count));
                }
                for (const auto& [name, count] : health.players.networks) {
                    population->AddRow(ConsoleName(name), {Tr("Network")}, {}, QString::number(count));
                }
                if (health.players.titles.empty() && health.players.networks.empty()) {
                    population->AddEmpty(Tr("Nobody is online right now."));
                }
            });
    };

    // Every 30 seconds while the page is the one showing; a tick during other work waits its
    // turn on the queue like anything else.
    auto* timer = new QTimer(page);
    timer->setInterval(30000);
    QObject::connect(timer, &QTimer::timeout, page, [page] {
        if (page->isVisible()) {
            page->reload(false);
        }
    });
    timer->start();
    QObject::connect(refresh, &QPushButton::clicked, page, [page] { page->reload(true); });
    return page;
}

} // namespace openpak::qt
