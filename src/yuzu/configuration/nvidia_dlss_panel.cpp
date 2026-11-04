// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef EGLANTINE_STREAMLINE

#include "common/httplib.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPromise>
#include <QPushButton>
#include <QStringList>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <fmt/format.h>
#include <zlib.h>

#include "common/common_types.h"
#include "common/fs/fs_util.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "video_core/vulkan_common/streamline.h"
#include "yuzu/configuration/nvidia_dlss_panel.h"

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

namespace {

namespace Streamline = Vulkan::Streamline;

constexpr const char* RELEASE_PAGE = "https://github.com/NVIDIA-RTX/Streamline/releases";
constexpr const char* RELEASE_VERSION = "2.14.16";
constexpr const char* ZIP_HOST = "https://github.com";
constexpr const char* ZIP_PATH =
    "/NVIDIA-RTX/Streamline/releases/download/v2.14.16/streamline-sdk-v2.14.16.zip";
constexpr const char* INTERPOSER_ENTRY = "bin/x64/sl.interposer.dll";
constexpr const char* MODEL_ENTRY = "bin/x64/nvngx_dlss.dll";
constexpr const char* STREAMLINE_LICENSE_ENTRY = "license.txt";
constexpr const char* DLSS_LICENSE_ENTRY = "bin/x64/nvngx_dlss.license.txt";

QString Tr(const char* text) {
    return QCoreApplication::translate("NvidiaDlssPanel", text);
}

u16 U16(const std::string& data, size_t at) {
    return static_cast<u16>(static_cast<u8>(data[at]) | static_cast<u8>(data[at + 1]) << 8);
}

u32 U32(const std::string& data, size_t at) {
    return static_cast<u32>(U16(data, at)) | static_cast<u32>(U16(data, at + 2)) << 16;
}

u64 U64(const std::string& data, size_t at) {
    return static_cast<u64>(U32(data, at)) | static_cast<u64>(U32(data, at + 4)) << 32;
}

struct ZipEntry {
    u16 method{};
    u32 crc{};
    u64 compressed{};
    u64 size{};
    u64 offset{};
};

/// Lets the UI thread stop a download running on a worker thread, including the request in
/// flight: httplib's Client::stop() may be called from another thread.
class Cancellation {
public:
    void Cancel() {
        std::scoped_lock lock{mutex};
        canceled = true;
        if (active) {
            active->stop();
        }
    }

    [[nodiscard]] bool IsCanceled() const {
        return canceled;
    }

    /// Sends request() on client unless the download was canceled, letting Cancel() stop it.
    template <typename Request>
    httplib::Result Send(httplib::Client& client, Request&& request) {
        {
            std::scoped_lock lock{mutex};
            if (canceled) {
                return httplib::Result{nullptr, httplib::Error::Canceled};
            }
            active = &client;
        }
        SCOPE_EXIT {
            std::scoped_lock lock{mutex};
            active = nullptr;
        };
        return request();
    }

private:
    std::mutex mutex;
    httplib::Client* active{};
    std::atomic_bool canceled{};
};

/// Reads single files out of a zip on a web server with range requests, so only the files
/// needed are downloaded (about 32 MB of the 276 MB release).
class RemoteZip {
public:
    explicit RemoteZip(Cancellation& cancellation_) : cancellation{cancellation_} {}

    /// Follows GitHub's redirect to the file's (short-lived) download address.
    bool Resolve() {
        host = ZIP_HOST;
        path = ZIP_PATH;
        for (int redirect = 0; redirect < 5; ++redirect) {
            const auto client = MakeClient();
            const httplib::Result result = cancellation.Send(*client, [&] {
                return client->Get(path, httplib::Headers{{"Range", "bytes=0-0"}});
            });
            if (!result) {
                error = cancellation.IsCanceled() ? "canceled" : httplib::to_string(result.error());
                return false;
            }
            if (result->status == 206) {
                // "bytes 0-0/<size>"; the server doesn't take "last N bytes" ranges.
                const std::string range = result->get_header_value("Content-Range");
                const size_t slash = range.rfind('/');
                zip_size = slash == std::string::npos
                               ? 0
                               : std::strtoull(range.c_str() + slash + 1, nullptr, 10);
                if (zip_size == 0) {
                    error = "no file size";
                    return false;
                }
                return true;
            }
            const std::string location = result->get_header_value("Location");
            const size_t scheme = location.find("://");
            const size_t slash =
                scheme == std::string::npos ? std::string::npos : location.find('/', scheme + 3);
            if (result->status < 300 || result->status >= 400 || slash == std::string::npos) {
                error = fmt::format("HTTP {}", result->status);
                return false;
            }
            host = location.substr(0, slash);
            path = location.substr(slash);
        }
        error = "too many redirects";
        return false;
    }

    bool Open() {
        if (!Resolve()) {
            return false;
        }
        const u64 tail_start = zip_size > 65558 ? zip_size - 65558 : 0;
        const std::optional<std::string> tail =
            Fetch(fmt::format("bytes={}-{}", tail_start, zip_size - 1));
        if (!tail) {
            return false;
        }
        const size_t end = tail->rfind(std::string{"PK\x05\x06", 4});
        if (end == std::string::npos || end + 22 > tail->size()) {
            error = "no zip directory";
            return false;
        }
        u64 directory_size = U32(*tail, end + 12);
        u64 directory_offset = U32(*tail, end + 16);
        if (directory_offset == 0xFFFFFFFF && end >= 20 &&
            tail->compare(end - 20, 4, std::string{"PK\x06\x07", 4}) == 0) {
            const u64 record = U64(*tail, end - 20 + 8);
            const std::optional<std::string> zip64 =
                Fetch(fmt::format("bytes={}-{}", record, record + 55));
            if (!zip64 || zip64->size() < 56) {
                return false;
            }
            directory_size = U64(*zip64, 40);
            directory_offset = U64(*zip64, 48);
        }
        const std::optional<std::string> directory = Fetch(
            fmt::format("bytes={}-{}", directory_offset, directory_offset + directory_size - 1));
        if (!directory) {
            return false;
        }
        for (size_t at = 0; at + 46 <= directory->size() && U32(*directory, at) == 0x02014b50;) {
            const u16 name_length = U16(*directory, at + 28);
            const u16 extra_length = U16(*directory, at + 30);
            const u16 comment_length = U16(*directory, at + 32);
            ZipEntry entry{
                .method = U16(*directory, at + 10),
                .crc = U32(*directory, at + 16),
                .compressed = U32(*directory, at + 20),
                .size = U32(*directory, at + 24),
                .offset = U32(*directory, at + 42),
            };
            const size_t extra_end = at + 46 + name_length + extra_length;
            if (extra_end + comment_length > directory->size()) {
                error = "bad zip directory";
                return false;
            }
            const std::string name = directory->substr(at + 46, name_length);
            // Zip64 sizes and offset, in this order, for the fields that overflowed.
            for (size_t extra = at + 46 + name_length; extra + 4 <= extra_end;) {
                const u16 id = U16(*directory, extra);
                const u16 length = U16(*directory, extra + 2);
                const size_t record_end = std::min<size_t>(extra + 4 + length, extra_end);
                if (id == 0x0001) {
                    size_t field = extra + 4;
                    for (u64* value : {&entry.size, &entry.compressed, &entry.offset}) {
                        if (*value == 0xFFFFFFFF && field + 8 <= record_end) {
                            *value = U64(*directory, field);
                            field += 8;
                        }
                    }
                }
                extra += 4 + length;
            }
            entries.emplace(name, entry);
            at += 46 + name_length + extra_length + comment_length;
        }
        return true;
    }

    const ZipEntry* Find(const std::string& name) const {
        const auto it = entries.find(name);
        return it == entries.end() ? nullptr : &it->second;
    }

    std::optional<std::string> Read(const ZipEntry& entry,
                                    const std::function<bool(u64)>& progress = {}) {
        const std::optional<std::string> header =
            Fetch(fmt::format("bytes={}-{}", entry.offset, entry.offset + 29));
        if (!header) {
            return std::nullopt;
        }
        if (header->size() < 30 || U32(*header, 0) != 0x04034b50) {
            error = "bad zip entry";
            return std::nullopt;
        }
        const u64 start = entry.offset + 30 + U16(*header, 26) + U16(*header, 28);
        std::string data;
        if (entry.compressed != 0) {
            std::optional<std::string> fetched =
                Fetch(fmt::format("bytes={}-{}", start, start + entry.compressed - 1), progress);
            if (!fetched) {
                return std::nullopt;
            }
            data = std::move(*fetched);
        }
        std::string out;
        if (entry.method == 0) {
            out = std::move(data);
        } else if (entry.method == 8) {
            out.resize(entry.size);
            z_stream stream{};
            if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
                error = "zlib";
                return std::nullopt;
            }
            stream.next_in = reinterpret_cast<Bytef*>(data.data());
            stream.avail_in = static_cast<uInt>(data.size());
            stream.next_out = reinterpret_cast<Bytef*>(out.data());
            stream.avail_out = static_cast<uInt>(out.size());
            const int result = inflate(&stream, Z_FINISH);
            inflateEnd(&stream);
            if (result != Z_STREAM_END || stream.total_out != entry.size) {
                error = "could not unpack";
                return std::nullopt;
            }
        } else {
            error = "unknown zip compression";
            return std::nullopt;
        }
        const uLong crc = crc32(0L, reinterpret_cast<const Bytef*>(out.data()),
                                static_cast<uInt>(out.size()));
        if (crc != entry.crc) {
            error = "damaged download (checksum)";
            return std::nullopt;
        }
        return out;
    }

    std::string error;

private:
    std::unique_ptr<httplib::Client> MakeClient() const {
        auto client = std::make_unique<httplib::Client>(host);
        client->set_connection_timeout(30);
        client->set_read_timeout(30);
        client->set_write_timeout(30);
#ifdef YUZU_BUNDLED_OPENSSL
        client->load_ca_cert_store(kCert, sizeof(kCert));
#endif
        return client;
    }

    std::optional<std::string> Fetch(const std::string& range,
                                     const std::function<bool(u64)>& progress = {}) {
        std::string body;
        int status = 0;
        const auto client = MakeClient();
        const httplib::Result result = cancellation.Send(*client, [&] {
            return client->Get(
                path, httplib::Headers{{"Range", range}},
                [&status](const httplib::Response& response) {
                    status = response.status;
                    return response.status == 206;
                },
                [&](const char* data, size_t length) {
                    body.append(data, length);
                    return !cancellation.IsCanceled() && (!progress || progress(body.size()));
                });
        });
        // Refusing any other answer in the response handler makes httplib report "Canceled":
        // say what the server answered instead.
        if (status != 0 && status != 206) {
            error = fmt::format("HTTP {}", status);
            return std::nullopt;
        }
        if (!result) {
            error = cancellation.IsCanceled() ? "canceled" : httplib::to_string(result.error());
            return std::nullopt;
        }
        return body;
    }

    Cancellation& cancellation;
    std::string host;
    std::string path;
    u64 zip_size{};
    std::map<std::string, ZipEntry> entries;
};

/// One download, shared by the panel on the UI thread and the worker thread doing the network
/// and file work. The panel reads the results only after the worker's future has finished;
/// cancellation is the only part both use at once.
struct DownloadJob {
    DownloadJob(std::vector<std::string> wanted_, std::filesystem::path folder_,
                std::vector<std::pair<std::string, const char*>> licence_titles_)
        : wanted{std::move(wanted_)}, folder{std::move(folder_)},
          licence_titles{std::move(licence_titles_)} {}

    /// Worker thread: reads the release's zip directory and the licences of the files wanted.
    void Prepare() {
        if (!zip.Open()) {
            error = zip.error;
            return;
        }
        for (const auto& [title, entry_name] : licence_titles) {
            const ZipEntry* const entry = zip.Find(entry_name);
            const std::optional<std::string> text = entry ? zip.Read(*entry) : std::nullopt;
            if (cancellation.IsCanceled()) {
                return;
            }
            licences += fmt::format("==== {} ====\n\n{}\n\n", title,
                                    text ? *text : std::string{"(see the release on GitHub)"});
        }
    }

    /// Worker thread: downloads the wanted files into folder, keeping them only when NVIDIA
    /// signed them.
    void Download(QPromise<void>& promise) {
        u64 total = 0;
        std::vector<const ZipEntry*> found;
        for (const std::string& name : wanted) {
            const ZipEntry* const entry = zip.Find(name);
            if (!entry) {
                error = name + " is not in the release";
                return;
            }
            found.push_back(entry);
            total += entry->compressed;
        }
        promise.setProgressRange(0, 1000);
        // The download address from the first step only lasts a few minutes.
        if (!zip.Resolve()) {
            error = zip.error;
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        u64 done = 0;
        for (size_t i = 0; i < wanted.size(); ++i) {
            const std::optional<std::string> contents = zip.Read(*found[i], [&](u64 received) {
                promise.setProgressValue(
                    static_cast<int>((done + received) * 1000 / std::max<u64>(total, 1)));
                return true;
            });
            if (!contents) {
                error = zip.error;
                return;
            }
            done += found[i]->compressed;
            const std::filesystem::path target =
                folder / std::filesystem::path{wanted[i]}.filename();
            std::filesystem::path partial = target;
            partial += L".download";
            {
                std::ofstream file(partial, std::ios::binary | std::ios::trunc);
                file.write(contents->data(), static_cast<std::streamsize>(contents->size()));
                if (!file) {
                    error = "could not write " + Common::FS::PathToUTF8String(partial);
                    return;
                }
            }
            const bool signed_ok = wanted[i] == INTERPOSER_ENTRY
                                       ? Streamline::IsStreamlineModule(partial)
                                       : Streamline::IsSignedByNvidia(partial);
            if (!signed_ok || cancellation.IsCanceled()) {
                std::filesystem::remove(partial, ec);
                if (!signed_ok) {
                    error = Common::FS::PathToUTF8String(target.filename()) +
                            " is not signed by NVIDIA";
                }
                return;
            }
            std::filesystem::rename(partial, target, ec);
            if (ec) {
                std::filesystem::remove(partial, ec);
                error = "could not save " + Common::FS::PathToUTF8String(target);
                return;
            }
            LOG_INFO(Frontend, "Downloaded {} from NVIDIA's Streamline {} release",
                     Common::FS::PathToUTF8String(target), RELEASE_VERSION);
        }
        promise.setProgressValue(1000);
    }

    const std::vector<std::string> wanted;
    const std::filesystem::path folder;
    const std::vector<std::pair<std::string, const char*>> licence_titles;
    Cancellation cancellation;
    RemoteZip zip{cancellation};
    std::string licences;
    std::string error; ///< Empty when the step worked or was canceled
};

/// Runs work(job, promise) on a worker thread behind a window-modal progress dialog whose Cancel
/// cancels job, then calls finished() on the UI thread. Nothing waits on the network here. When
/// the panel goes away first, the dialog and the watcher go with it and finished() never runs.
template <typename Work, typename Finished>
void RunInBackground(QWidget* parent, const QString& label, bool shows_progress,
                     const std::shared_ptr<DownloadJob>& job, Work work, Finished finished) {
    auto* const dialog =
        new QProgressDialog(label, Tr("Cancel"), 0, shows_progress ? 1000 : 0, parent);
    dialog->setWindowTitle(Tr("NVIDIA DLSS"));
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setMinimumDuration(0);
    dialog->setAutoReset(false);
    dialog->setAutoClose(false);
    dialog->setValue(0);
    auto* const watcher = new QFutureWatcher<void>(dialog);
    QObject::connect(dialog, &QProgressDialog::canceled, dialog,
                     [job] { job->cancellation.Cancel(); });
    QObject::connect(watcher, &QFutureWatcher<void>::progressValueChanged, dialog,
                     &QProgressDialog::setValue);
    QObject::connect(watcher, &QFutureWatcher<void>::finished, dialog, [dialog, finished] {
        dialog->hide();
        dialog->deleteLater();
        finished();
    });
    watcher->setFuture(
        QtConcurrent::run([job, work](QPromise<void>& promise) { work(*job, promise); }));
    dialog->show();
}

/// Shows NVIDIA's licences for the files job downloads; true when the user accepts them.
bool AcceptLicences(QWidget* parent, const DownloadJob& job) {
    QStringList names;
    for (const std::string& entry : job.wanted) {
        names << QString::fromStdString(
            Common::FS::PathToUTF8String(std::filesystem::path{entry}.filename()));
    }
    QDialog dialog(parent);
    dialog.setWindowTitle(Tr("Download NVIDIA DLSS"));
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    QLabel* intro = new QLabel(
        Tr("Eglantine will download %1 from NVIDIA's Streamline %2 release on GitHub into %3.<br>"
           "These files are NVIDIA's, under NVIDIA's licence terms below.")
            .arg(names.join(QStringLiteral(" and ")), QString::fromLatin1(RELEASE_VERSION),
                 QString::fromStdWString(job.folder.wstring())),
        &dialog);
    intro->setWordWrap(true);
    QPlainTextEdit* licence_text =
        new QPlainTextEdit(QString::fromStdString(job.licences), &dialog);
    licence_text->setReadOnly(true);
    licence_text->setMinimumSize(640, 380);
    QDialogButtonBox* buttons = new QDialogButtonBox(&dialog);
    buttons->addButton(Tr("Accept and Download"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(intro);
    layout->addWidget(licence_text, 1);
    layout->addWidget(buttons);
    return dialog.exec() == QDialog::Accepted;
}

QString Named(const QString& name, const std::string& version) {
    return version.empty() ? name : QStringLiteral("%1 %2").arg(name, QString::fromStdString(version));
}

QString PathText(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}

} // Anonymous namespace

NvidiaDlssPanel::NvidiaDlssPanel(QWidget* parent) : QWidget(parent) {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 0, 0, 8);
    layout->setSpacing(6);

    QHBoxLayout* status_row = new QHBoxLayout;
    status_row->setSpacing(8);
    icon_label = new QLabel(this);
    icon_label->setTextFormat(Qt::RichText);
    icon_label->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    icon_label->setFixedWidth(16);
    status_label = new QLabel(this);
    status_label->setTextFormat(Qt::RichText);
    status_label->setWordWrap(true);
    status_row->addWidget(icon_label);
    status_row->addWidget(status_label, 1);

    QHBoxLayout* button_row = new QHBoxLayout;
    button_row->setContentsMargins(24, 0, 0, 0);
    download_button = new QPushButton(this);
    folder_button = new QPushButton(Tr("Open Folder"), this);
    QLabel* about = new QLabel(
        QStringLiteral("<a href=\"%1\">%2</a>").arg(QString::fromLatin1(RELEASE_PAGE),
                                                   Tr("Where do these files come from?")),
        this);
    about->setOpenExternalLinks(true);
    about->setToolTip(Tr("NVIDIA's Streamline releases on GitHub. The files are in bin\\x64 of "
                         "the release zip."));
    button_row->addWidget(download_button);
    button_row->addWidget(folder_button);
    button_row->addStretch(1);
    button_row->addWidget(about);

    layout->addLayout(status_row);
    layout->addLayout(button_row);

    connect(download_button, &QPushButton::clicked, this, [this] { Download(); });
    connect(folder_button, &QPushButton::clicked, this, [] {
        const std::filesystem::path folder = Streamline::FindDlssFiles().folder;
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        QDesktopServices::openUrl(QUrl::fromLocalFile(PathText(folder)));
    });
    Refresh();
}

NvidiaDlssPanel::~NvidiaDlssPanel() {
    // The settings closed mid-download: the progress dialog is gone with this panel, so stop
    // the worker too instead of leaving it to finish a download nobody waits for.
    if (cancel_download) {
        cancel_download();
    }
}

void NvidiaDlssPanel::showEvent(QShowEvent* event) {
    Refresh();
    QWidget::showEvent(event);
}

void NvidiaDlssPanel::Refresh() {
    const Streamline::DlssFiles files = Streamline::FindDlssFiles();
    const bool has_loader = !files.interposer.empty();
    const bool has_model = !files.model.empty() || !files.app_model.empty();
    const QString dlss = !files.model.empty()
                             ? Named(QStringLiteral("DLSS"), files.model_version)
                             : Tr("%1 from the NVIDIA app")
                                   .arg(Named(QStringLiteral("DLSS"), files.app_model_version));

    QString status;
    QString button;
    if (has_loader && has_model) {
        status = Tr("<b>Ready.</b> Found %1 and %2.")
                     .arg(Named(QStringLiteral("NVIDIA Streamline"), files.interposer_version), dlss);
    } else if (has_model) {
        status = Tr("<b>One file needed:</b> NVIDIA's Streamline loader (sl.interposer.dll). "
                    "%1 will be used. Until then, games use AMD FSR 3.1.")
                     .arg(dlss);
        button = Tr("Download from NVIDIA (1 MB)");
    } else if (has_loader) {
        status = Tr("<b>One file needed:</b> NVIDIA DLSS (nvngx_dlss.dll). Until then, games use "
                    "AMD FSR 3.1.");
        button = Tr("Download from NVIDIA (31 MB)");
    } else {
        status = Tr("<b>NVIDIA's files needed:</b> the Streamline loader and DLSS "
                    "(sl.interposer.dll and nvngx_dlss.dll). Until then, games use AMD FSR 3.1.");
        button = Tr("Download from NVIDIA (32 MB)");
    }
    const bool ready = has_loader && has_model;
    icon_label->setText(ready ? QStringLiteral("<span style=\"color:#2ea043\">&#10004;</span>")
                              : QStringLiteral("<span style=\"color:#d29922\">&#9888;</span>"));
    status_label->setText(status);
    status_label->setToolTip(Tr("Folder: %1").arg(PathText(files.folder)));
    download_button->setText(button);
    download_button->setVisible(!ready);
}

void NvidiaDlssPanel::Download() {
    if (cancel_download) {
        return;
    }
    const Streamline::DlssFiles files = Streamline::FindDlssFiles();
    std::vector<std::string> wanted;
    if (files.interposer.empty()) {
        wanted.emplace_back(INTERPOSER_ENTRY);
    }
    const bool wants_model = files.model.empty() && files.app_model.empty();
    if (wants_model) {
        wanted.emplace_back(MODEL_ENTRY);
    }
    if (wanted.empty()) {
        return;
    }
    // Translated here: the worker thread only formats them.
    std::vector<std::pair<std::string, const char*>> licence_titles{
        {Tr("NVIDIA Streamline").toStdString(), STREAMLINE_LICENSE_ENTRY}};
    if (wants_model) {
        licence_titles.emplace_back(Tr("NVIDIA DLSS").toStdString(), DLSS_LICENSE_ENTRY);
    }
    // Into the user folder: FindDlssFiles looks there for a model wherever the loader is.
    const auto job = std::make_shared<DownloadJob>(std::move(wanted), Streamline::UserFolder(),
                                                   std::move(licence_titles));
    cancel_download = [job] { job->cancellation.Cancel(); };
    download_button->setEnabled(false);

    const auto finish = [this] {
        cancel_download = nullptr;
        download_button->setEnabled(true);
        Refresh();
    };
    const auto fail = [this, finish](const std::string& error) {
        LOG_ERROR(Frontend, "NVIDIA DLSS download failed: {}", error);
        QMessageBox::warning(this, Tr("NVIDIA DLSS"),
                             Tr("Could not download NVIDIA's files: %1")
                                 .arg(QString::fromStdString(error)));
        finish();
    };
    const auto download = [this, job, finish, fail] {
        RunInBackground(
            this, Tr("Downloading NVIDIA's DLSS files..."), true, job,
            [](DownloadJob& running, QPromise<void>& promise) { running.Download(promise); },
            [this, job, finish, fail] {
                if (job->cancellation.IsCanceled()) {
                    finish();
                } else if (!job->error.empty()) {
                    fail(job->error);
                } else {
                    finish();
                    QMessageBox::information(
                        this, Tr("NVIDIA DLSS"),
                        Tr("NVIDIA DLSS is ready. It is used the next time a game starts."));
                }
            });
    };
    RunInBackground(
        this, Tr("Contacting NVIDIA's download..."), false, job,
        [](DownloadJob& running, QPromise<void>&) { running.Prepare(); },
        [this, job, finish, fail, download] {
            if (job->cancellation.IsCanceled()) {
                finish();
            } else if (!job->error.empty()) {
                fail(job->error);
            } else if (!AcceptLicences(this, *job)) {
                finish();
            } else {
                download();
            }
        });
}

#endif
