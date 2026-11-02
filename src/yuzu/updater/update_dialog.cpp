// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include "common/logging.h"
#include "common/scm_rev.h"
#include "qt_common/abstract/frontend.h"
#include "qt_common/abstract/progress.h"
#include "ui_update_dialog.h"
#include "update_dialog.h"

#include "common/httplib.h"

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

#undef GetSaveFileName

UpdateDialog::UpdateDialog(const UpdateChecker::Update& update, QWidget* parent)
    : QDialog(parent), ui(new Ui::UpdateDialog) {
    ui->setupUi(this);

    const auto& release = update.release;
    ui->version->setText(tr("%1 is available. You are running %2.")
                             .arg(QString::fromStdString(release.title),
                                  QString::fromUtf8(Common::g_build_version)));
    ui->url->setText(
        tr("<a href=\"%1\">View on GitHub</a>").arg(QString::fromStdString(release.html_url)));

    const auto count = static_cast<int>(update.changes.size());
    QString changelog = QStringLiteral("### %1\n\n").arg(tr("What's new"));
    if (update.changes_complete) {
        changelog += tr("%n change(s) since your build:", "", count);
    } else if (count > 0) {
        changelog += tr("Your build is more than %n change(s) behind. The newest ones:", "", count);
    } else {
        changelog += tr("The list of changes could not be loaded.");
    }
    changelog += QStringLiteral("\n\n");
    for (const Common::Net::Commit& commit : update.changes) {
        changelog += QStringLiteral("- %1 (%2, [%3](https://github.com/%4/commit/%5))\n")
                         .arg(QString::fromStdString(commit.summary),
                              QString::fromStdString(commit.author),
                              QString::fromStdString(commit.sha.substr(0, 7)),
                              QString::fromUtf8(Common::g_build_auto_update_repo),
                              QString::fromStdString(commit.sha));
    }
    changelog += QStringLiteral("\n[%1](%2)\n\n---\n\n%3")
                     .arg(tr("Compare your build with this release on GitHub"),
                          QString::fromStdString(update.compare_url),
                          QString::fromStdString(release.body));
    ui->body->setMarkdown(changelog);

    if (const auto asset = release.GetPlatformAsset()) {
        m_asset = *asset;
        ui->prompt->setText(tr("Download %1 now?").arg(QString::fromStdString(m_asset.name)));
        connect(this, &QDialog::accepted, this, &UpdateDialog::Download);
    } else {
        ui->prompt->setText(tr("Open the release page to download it?"));
        connect(this, &QDialog::accepted, this, [url = release.html_url] {
            QDesktopServices::openUrl(QUrl{QString::fromStdString(url)});
        });
    }
}

UpdateDialog::~UpdateDialog() {
    delete ui;
}

// TODO: migrate to a net.cpp wrapper
void UpdateDialog::Download() {
    const auto filename = QtCommon::Frontend::GetSaveFileName(
        tr("New Version Location"),
        QDir{QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)}.filePath(
            QString::fromStdString(m_asset.name)),
        tr("All Files (*.*)"));

    if (filename.isEmpty())
        return;

    QSaveFile file(filename);
    if (!file.open(QIODevice::Truncate | QIODevice::WriteOnly)) {
        LOG_WARNING(Frontend, "Could not open file {}", filename.toStdString());
        QtCommon::Frontend::Critical(tr("Failed to save file"),
                                     tr("Could not open file %1 for writing.").arg(filename));
        return;
    }

    // GitHub serves release files through a redirect to its file host.
    const QUrl url{QString::fromStdString(m_asset.url)};
    const auto host = QStringLiteral("%1://%2").arg(url.scheme(), url.host()).toStdString();
    const auto path = url.path(QUrl::FullyEncoded).toStdString();

    // TODO(crueter): Move to net.cpp
    constexpr std::size_t timeout_seconds = 15;

    std::unique_ptr<httplib::Client> client = std::make_unique<httplib::Client>(host);
    client->set_connection_timeout(timeout_seconds);
    client->set_read_timeout(timeout_seconds);
    client->set_write_timeout(timeout_seconds);
    client->set_follow_location(true);
    client->set_default_headers({{"User-Agent", "Eglantine"}});

#ifdef YUZU_BUNDLED_OPENSSL
    client->load_ca_cert_store(kCert, sizeof(kCert));
#endif

    auto progress =
        QtCommon::Frontend::newProgressDialog(tr("Downloading..."), tr("Cancel"), 0, 100);
    progress->show();

    QGuiApplication::processEvents();

    // Progress dialog.
    auto progress_callback = [&](size_t processed_size, size_t total_size) {
        QGuiApplication::processEvents();
        if (total_size != 0)
            progress->setValue(static_cast<int>((processed_size * 100) / total_size));
        return !progress->wasCanceled();
    };

    // Write file in chunks.
    auto content_receiver = [&file, filename](const char* t_data, size_t data_length) -> bool {
        if (file.write(t_data, data_length) == -1) {
            LOG_WARNING(Frontend, "Could not write {} bytes to file {}", data_length,
                        filename.toStdString());
            QtCommon::Frontend::Critical(tr("Failed to save file"),
                                         tr("Could not write to file %1.").arg(filename));
            return false;
        }

        return true;
    };

    // Now send off request
    auto result = client->Get(path, content_receiver, progress_callback);
    const bool canceled = progress->wasCanceled();
    progress->close();

    if (canceled) {
        file.cancelWriting();
        return;
    }

    if (!result) {
        LOG_ERROR(Frontend, "GET to {}{} failed: {}", host, path, httplib::to_string(result.error()));
        file.cancelWriting();
        QtCommon::Frontend::Critical(tr("Failed to download file"),
                                     tr("Could not download %1.").arg(url.toString()));
        return;
    }

    const auto& response = result.value();
    if (response.status >= 400) {
        LOG_ERROR(Frontend, "GET to {}{} returned error status code: {}", host, path,
                  response.status);
        file.cancelWriting();
        QtCommon::Frontend::Critical(
            tr("Failed to download file"),
            tr("Could not download %1\nError code: %2")
                .arg(url.toString(), QString::number(response.status)));
        return;
    }

    if (!file.commit()) {
        LOG_WARNING(Frontend, "Could not commit to file {}", filename.toStdString());
        QtCommon::Frontend::Critical(tr("Failed to save file"),
                                     tr("Could not commit to file %1.").arg(filename));
        return;
    }

    if (filename.endsWith(QStringLiteral(".AppImage"))) {
        QFile::setPermissions(filename, QFile::permissions(filename) | QFileDevice::ExeOwner |
                                            QFileDevice::ExeGroup | QFileDevice::ExeOther);
    }

    // Download is complete. User may choose to open its folder in the file manager.
    auto button = QtCommon::Frontend::Question(
        tr("Download Complete"),
        tr("Successfully downloaded %1. Would you like to open its folder?")
            .arg(QFileInfo{filename}.fileName()),
        QtCommon::Frontend::Yes | QtCommon::Frontend::No);

    if (button == QtCommon::Frontend::Yes) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo{filename}.absolutePath()));
    }
}
