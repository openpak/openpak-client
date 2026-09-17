// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#include "openpak/qt/extra_pages.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <fmt/format.h>

#ifdef OPENPAK_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

#include "openpak/account.h"
#include "openpak/api.h"
#include "openpak/compatible_titles.h"
#include "openpak/friends_cache.h"
#include "openpak/platform.h"
#include "openpak/qt/account_page_p.h"
#include "openpak/qt/host.h"
#include "openpak/session.h"

namespace openpak::qt {

namespace Api = WebService::OpenPakApi;

using Platform::ContainedPath;

namespace {

QString Tr(const char* text) {
    return QCoreApplication::translate("OpenPakAccountDialog", text);
}

// Work off the UI thread, its result back on it. The hop goes through the application object,
// which outlives every page, so nothing but the UI thread ever looks at the guard.
template <typename Work, typename Done>
void Async(QWidget* owner, Work work, Done done) {
    std::thread{[guard = QPointer<QWidget>(owner), work = std::move(work),
                 done = std::move(done)]() mutable {
        auto result = std::make_shared<decltype(work())>(work());
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [guard, done = std::move(done), result]() mutable {
                if (guard) {
                    done(std::move(*result));
                }
            },
            Qt::QueuedConnection);
    }}.detach();
}

class Page : public QWidget {
public:
    std::function<void()> on_show;
    std::function<void()> reload;

protected:
    void showEvent(QShowEvent* event) override {
        QWidget::showEvent(event);
        if (on_show) {
            on_show();
        }
    }
};

QLabel* Dim(const QString& text) {
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, style::Dim());
    label->setPalette(palette);
    return label;
}

QPushButton* Button(const QString& text) {
    auto* button = new QPushButton(text);
    button->setCursor(Qt::PointingHandCursor);
    button->setMinimumHeight(30);
    return button;
}

// A scrolling column of card rows: a bold title, dim lines under it, buttons on the right.
class RowList : public QScrollArea {
public:
    RowList() {
        setWidgetResizable(true);
        setFrameShape(QFrame::NoFrame);
        setStyleSheet(QStringLiteral("QScrollArea { background: transparent; }"));
        auto* inner = new QWidget;
        inner->setStyleSheet(QStringLiteral("QWidget#rows { background: transparent; }"));
        inner->setObjectName(QStringLiteral("rows"));
        rows = new QVBoxLayout(inner);
        rows->setContentsMargins(0, 0, 0, 0);
        rows->setSpacing(6);
        rows->addStretch(1);
        setWidget(inner);
    }

    void Clear() {
        while (rows->count() > 1) {
            QLayoutItem* item = rows->takeAt(0);
            delete item->widget();
            delete item;
        }
    }

    void AddEmpty(const QString& text) {
        QLabel* label = Dim(text);
        label->setAlignment(Qt::AlignCenter);
        label->setContentsMargins(0, 24, 0, 24);
        rows->insertWidget(rows->count() - 1, label);
    }

    void Add(const QString& title, const QStringList& lines, const std::vector<QPushButton*>& buttons,
             const QString& trailing = {}) {
        auto* row = new QFrame;
        row->setStyleSheet(style::Card());
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(14, 8, 14, 8);

        auto* text = new QVBoxLayout;
        text->setSpacing(2);
        auto* name = new QLabel(title);
        QFont font = name->font();
        font.setBold(true);
        name->setFont(font);
        text->addWidget(name);
        for (const QString& line : lines) {
            if (!line.isEmpty()) {
                text->addWidget(Dim(line));
            }
        }
        layout->addLayout(text, 1);

        if (!trailing.isEmpty()) {
            layout->addWidget(Dim(trailing));
        }
        for (QPushButton* button : buttons) {
            layout->addWidget(button);
        }
        rows->insertWidget(rows->count() - 1, row);
    }

private:
    QVBoxLayout* rows;
};

// The frame every one of these pages has: a header row the caller fills, then the content.
QVBoxLayout* Frame(Page* page, QHBoxLayout*& header) {
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(10);
    header = new QHBoxLayout;
    layout->addLayout(header);
    return layout;
}

QString TitleHex(u64 title_id) {
    return QString::fromStdString(fmt::format("{:016x}", title_id));
}

QString TitleName(Host* host, const std::string& title_id_hex) {
    if (title_id_hex.empty()) {
        return {};
    }
    // The id is the hint: a title that is not installed is named by it rather than "a game".
    return host->ResolveGameName(title_id_hex, QString::fromStdString(title_id_hex).toUpper().toStdString());
}

// Filled the first time its page is shown: the library, else the running title and the titles
// OpenPak is known to serve.
void FillTitles(QComboBox* picker, Host* host) {
    if (picker->count() > 0) {
        return;
    }
    std::vector<Host::Title> titles = host->InstalledTitles();
    if (titles.empty()) {
        for (const auto& [title_id, version] : OpenPak::CompatibleTitles::Table()) {
            titles.push_back({title_id, TitleName(host, TitleHex(title_id).toStdString())});
        }
    }
    std::sort(titles.begin(), titles.end(), [](const Host::Title& left, const Host::Title& right) {
        return left.name.localeAwareCompare(right.name) < 0;
    });
    const QSignalBlocker blocker{picker};
    for (const Host::Title& title : titles) {
        picker->addItem(title.name, TitleHex(title.id));
    }
    const int running = picker->findData(QString::fromStdString(host->GetLocalAppId()).toLower());
    picker->setCurrentIndex(std::max(running, 0));
}

QString LocalTime(const QDateTime& when) {
    return when.isValid() ? QLocale().toString(when.toLocalTime(), QLocale::ShortFormat) : QString{};
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

QWidget* MakeInvitationsPage(Host* host, StatusSink status) {
    struct Waiting {
        std::vector<Api::CoreInvitation> core;
        std::vector<client::session::Invitation> native;
        std::vector<Common::OpenPakFriends::PendingInvitation> queued;
    };

    auto* page = new Page;
    QHBoxLayout* header = nullptr;
    QVBoxLayout* layout = Frame(page, header);
    QPushButton* refresh = Button(Tr("Refresh"));
    header->addStretch(1);
    header->addWidget(refresh);
    auto* list = new RowList;
    layout->addWidget(list, 1);

    page->reload = [=] {
        if (!Common::OpenPakAccount::IsLinked()) {
            list->Clear();
            list->AddEmpty(Tr("Sign in to OpenPak to use this."));
            return;
        }
        refresh->setEnabled(false);
        Async(
            page,
            [] {
                client::session::RefreshInvitations();
                return Waiting{Api::GetInvitations(), client::session::Invitations(),
                               Common::OpenPakFriends::PeekPendingInvitations()};
            },
            [=](Waiting waiting) {
                refresh->setEnabled(true);
                list->Clear();

                // One row per invitation; what differs is how it is turned down. Play launches
                // the title -- the host's own poll is what hands the join payload to the guest.
                const auto add = [&](const std::string& from, const std::string& title_id,
                                     const QDateTime& expires, std::function<std::string()> decline) {
                    u64 id = 0;
                    try {
                        id = std::stoull(title_id, nullptr, 16);
                    } catch (const std::exception&) {
                    }
                    QPushButton* play = Button(Tr("Play"));
                    play->setEnabled(id != 0 && !host->ResolveGameIcon(title_id).isEmpty());
                    play->setToolTip(play->isEnabled() ? QString{}
                                                       : Tr("%1 is not in your library.")
                                                             .arg(TitleName(host, title_id)));
                    QObject::connect(play, &QPushButton::clicked, page, [=] { host->QuickStart(id); });
                    QPushButton* no = Button(Tr("Decline"));
                    QObject::connect(no, &QPushButton::clicked, page, [=] {
                        no->setEnabled(false);
                        Async(page, decline, [=](std::string failure) {
                            if (!failure.empty()) {
                                status(QString::fromStdString(failure));
                            }
                            page->reload();
                        });
                    });
                    list->Add(TitleName(host, title_id),
                              {Tr("From %1").arg(QString::fromStdString(from)),
                               expires.isValid() ? Tr("Expires %1").arg(LocalTime(expires)) : QString{}},
                              {play, no});
                };

                for (const auto& one : waiting.core) {
                    add(one.from, one.title_id,
                        QDateTime::fromString(QString::fromStdString(one.expires_at), Qt::ISODate),
                        [id = one.id] { return Api::DeclineInvitation(id); });
                }
                for (const auto& one : waiting.native) {
                    add(one.sender_name, one.title_id,
                        one.expires_at > 0 ? QDateTime::fromSecsSinceEpoch(one.expires_at) : QDateTime{},
                        [id = one.id] {
                            client::session::DismissInvitation(id);
                            return std::string{};
                        });
                }
                for (const auto& one : waiting.queued) {
                    list->Add(Tr("From %1").arg(QString::fromStdString(one.from_name)),
                              {Tr("Ready: start or resume the game and it will pick this up.")}, {});
                }
                if (waiting.core.empty() && waiting.native.empty() && waiting.queued.empty()) {
                    list->AddEmpty(Tr("Nothing waiting."));
                }
            });
    };
    QObject::connect(refresh, &QPushButton::clicked, page, [=] { page->reload(); });
    page->on_show = [=] { page->reload(); };
    return page;
}

QWidget* MakeModsPage(Host* host, StatusSink status) {
    struct Catalogue {
        std::vector<Api::Mod> mods;
        std::vector<std::string> favourites;
    };

    auto* page = new Page;
    QHBoxLayout* header = nullptr;
    QVBoxLayout* layout = Frame(page, header);
    auto* picker = new QComboBox;
    QPushButton* refresh = Button(Tr("Refresh"));
    header->addWidget(new QLabel(Tr("Title")));
    header->addWidget(picker, 1);
    header->addWidget(refresh);
    auto* list = new RowList;
    layout->addWidget(list, 1);

    page->reload = [=] {
        const std::string title = picker->currentData().toString().toStdString();
        if (title.empty()) {
            return;
        }
        refresh->setEnabled(false);
        Async(
            page, [title] { return Catalogue{Api::GetMods(title), Api::GetFavouriteModIds()}; },
            [=](Catalogue catalogue) {
                refresh->setEnabled(true);
                if (picker->currentData().toString().toStdString() != title) {
                    return; // the person moved on while this was in flight
                }
                list->Clear();
                if (catalogue.mods.empty()) {
                    list->AddEmpty(Tr("No mods published for this title."));
                }
                const std::filesystem::path mods_dir = host->ModDirectory(std::stoull(title, nullptr, 16));

                for (const Api::Mod& mod : catalogue.mods) {
                    const QString name = QString::fromStdString(mod.name);
                    const std::filesystem::path dir = mods_dir.empty() ? mods_dir : mods_dir / ModDirName(mod);
                    std::error_code ec;
                    const bool installed = !dir.empty() && std::filesystem::exists(dir, ec) &&
                                           !std::filesystem::is_empty(dir, ec);

                    // Nothing is registered anywhere: the host's own mod manager lists what is
                    // on disk, so a mod installed here is disabled or deleted there.
                    QPushButton* install = Button(installed ? Tr("Reinstall") : Tr("Install"));
                    QObject::connect(install, &QPushButton::clicked, page, [=] {
#ifdef OPENPAK_HAVE_LIBARCHIVE
                        if (dir.empty()) {
                            status(Tr("This build has nowhere to install mods."));
                            return;
                        }
                        install->setEnabled(false);
                        status(Tr("Working..."));
                        Async(
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
                                status(failure == 0   ? Tr("Installed %1.").arg(name)
                                       : failure == 1 ? Tr("Could not install %1: the download failed or "
                                                           "did not match its published hash.")
                                                            .arg(name)
                                                      : Tr("Could not install %1.").arg(name));
                            });
#else
                        // ponytail: no unzip without libarchive (the mingw cross build); wire
                        // the PowerShell fallback save_sync uses if that build wants mods.
                        status(Tr("This build cannot unpack mods."));
#endif
                    });

                    const bool favourite = std::find(catalogue.favourites.begin(), catalogue.favourites.end(),
                                                     mod.id) != catalogue.favourites.end();
                    QPushButton* star = Button(favourite ? Tr("\xE2\x98\x85 Favourite") : Tr("\xE2\x98\x86 Favourite"));
                    star->setProperty("favourite", favourite);
                    QObject::connect(star, &QPushButton::clicked, page, [=] {
                        if (!Common::OpenPakAccount::IsLinked()) {
                            status(Tr("Sign in to OpenPak to use this."));
                            return;
                        }
                        const bool want = !star->property("favourite").toBool();
                        star->setEnabled(false);
                        Async(page, [id = mod.id, want] { return Api::SetModFavourite(id, want); },
                              [=](bool ok) {
                                  star->setEnabled(true);
                                  if (ok) {
                                      star->setProperty("favourite", want);
                                      star->setText(want ? Tr("\xE2\x98\x85 Favourite")
                                                         : Tr("\xE2\x98\x86 Favourite"));
                                  }
                              });
                    });

                    list->Add(QStringLiteral("%1  %2").arg(name, QString::fromStdString(mod.version)),
                              {Tr("by %1, %2").arg(QString::fromStdString(mod.author),
                                                   QString::fromStdString(mod.licence)),
                               QString::fromStdString(mod.summary)},
                              {star, install});
                }
            });
    };
    QObject::connect(refresh, &QPushButton::clicked, page, [=] { page->reload(); });
    QObject::connect(picker, &QComboBox::currentIndexChanged, page, [=] { page->reload(); });
    page->on_show = [=] {
        const bool first = picker->count() == 0;
        FillTitles(picker, host);
        if (first) {
            page->reload();
        }
    };
    return page;
}

QWidget* MakeNewsPage(Host* host, StatusSink status) {
    auto* page = new Page;
    QHBoxLayout* header = nullptr;
    QVBoxLayout* layout = Frame(page, header);
    auto* picker = new QComboBox;
    QPushButton* refresh = Button(Tr("Refresh"));
    QPushButton* save = Button(Tr("Save to disk..."));
    save->setEnabled(false);
    header->addWidget(new QLabel(Tr("Title")));
    header->addWidget(picker, 1);
    header->addWidget(refresh);
    header->addWidget(save);
    QLabel* valid_from = Dim({});
    layout->addWidget(valid_from);
    auto* list = new RowList;
    layout->addWidget(list, 1);

    auto files = std::make_shared<std::vector<Api::NewsFile>>();

    const auto reload = [=] {
        const std::string title = picker->currentData().toString().toStdString();
        if (title.empty()) {
            return;
        }
        refresh->setEnabled(false);
        Async(page, [title] { return Api::GetNewsManifest(title); }, [=](Api::NewsManifest manifest) {
            refresh->setEnabled(true);
            if (picker->currentData().toString().toStdString() != title) {
                return;
            }
            list->Clear();
            *files = manifest.files;
            save->setEnabled(!files->empty());
            valid_from->setText(manifest.ok && !manifest.valid_from.empty()
                                    ? Tr("From %1").arg(LocalTime(QDateTime::fromString(
                                          QString::fromStdString(manifest.valid_from), Qt::ISODate)))
                                    : QString{});
            if (files->empty()) {
                list->AddEmpty(Tr("No news for this title."));
            }
            for (const Api::NewsFile& file : *files) {
                list->Add(QString::fromStdString(file.path), {}, {},
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
        status(Tr("Working..."));
        Async(
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
                status(Tr("Saved %1 files to %2.").arg(written).arg(chosen));
            });
    });
    QObject::connect(refresh, &QPushButton::clicked, page, reload);
    QObject::connect(picker, &QComboBox::currentIndexChanged, page, reload);
    page->on_show = [=] {
        const bool first = picker->count() == 0;
        FillTitles(picker, host);
        if (first) {
            reload();
        }
    };
    return page;
}

QWidget* MakeStatusPage(Host* host, StatusSink status) {
    auto* page = new Page;
    QHBoxLayout* header = nullptr;
    QVBoxLayout* layout = Frame(page, header);
    QLabel* players = Dim({});
    QPushButton* refresh = Button(Tr("Refresh"));
    header->addWidget(players, 1);
    header->addWidget(refresh);

    auto* columns = new QHBoxLayout;
    columns->setSpacing(16);
    const auto column = [&](const QString& title) {
        auto* card = new QFrame;
        card->setStyleSheet(style::Card());
        auto* card_layout = new QVBoxLayout(card);
        card_layout->setContentsMargins(18, 16, 18, 16);
        auto* label = new QLabel(title);
        QFont font = label->font();
        font.setBold(true);
        font.setPointSize(font.pointSize() + 1);
        label->setFont(font);
        card_layout->addWidget(label);
        auto* list = new RowList;
        card_layout->addWidget(list, 1);
        columns->addWidget(card, 1);
        return list;
    };
    RowList* titles = column(Tr("Titles"));
    RowList* networks = column(Tr("Networks"));
    layout->addLayout(columns, 1);

    // Public: this answers even when the sign-in is the broken part.
    const auto reload = [=] {
        refresh->setEnabled(false);
        Async(page, [] { return Api::GetStatus(); }, [=](Api::NetworkStatus now) {
            refresh->setEnabled(true);
            titles->Clear();
            networks->Clear();
            if (!now.ok) {
                players->setText(Tr("Could not reach %1.").arg(QString::fromStdString(Api::BaseUrl())));
                status(players->text());
                return;
            }
            players->setText(Tr("%1 players online").arg(now.players_online));
            for (const auto& [title_id, count] : now.titles) {
                titles->Add(TitleName(host, title_id), {}, {}, Tr("%1 players").arg(count));
            }
            for (const auto& [name, count] : now.networks) {
                networks->Add(QString::fromStdString(name), {}, {}, Tr("%1 players").arg(count));
            }
        });
    };
    QObject::connect(refresh, &QPushButton::clicked, page, reload);
    page->on_show = reload;
    return page;
}

} // namespace openpak::qt
