// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include "common/net/net.h"
#include "frontend_common/update_checker.h"

namespace Ui {
class UpdateDialog;
}

class UpdateDialog : public QDialog {
    Q_OBJECT

public:
    explicit UpdateDialog(const UpdateChecker::Update& update, QWidget* parent = nullptr);
    ~UpdateDialog();

private slots:
    void Download();

private:
    Ui::UpdateDialog* ui;
    Common::Net::Asset m_asset;
};
