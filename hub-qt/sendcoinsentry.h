/*
 * This file is part of the Flowee project
 * Copyright (C) 2011-2015 The Bitcoin Core developers
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef FLOWEE_QT_SENDCOINSENTRY_H
#define FLOWEE_QT_SENDCOINSENTRY_H

#include "walletmodel.h"

#include <QStackedWidget>

class WalletModel;
class PlatformStyle;

namespace Ui {
    class SendCoinsEntry;
}

/**
 * A single entry in the dialog for sending bitcoins.
 * Stacked widget, with different UIs for payment requests
 * with a strong payee identity.
 */
class SendCoinsEntry : public QStackedWidget
{
    Q_OBJECT

public:
    explicit SendCoinsEntry(const PlatformStyle *platformStyle, QWidget *parent = 0);
    ~SendCoinsEntry();

    void setModel(WalletModel *model);
    bool validate();
    SendCoinsRecipient getValue();

    /** Return whether the entry is still empty and unedited */
    bool isClear();

    void setValue(const SendCoinsRecipient &value);
    void setAddress(const QString &address);

    /** Set up the tab chain manually, as Qt messes up the tab chain by default in some cases
     *  (issue https://bugreports.qt-project.org/browse/QTBUG-10907).
     */
    QWidget *setupTabChain(QWidget *prev);

    void setFocus();

public Q_SLOTS:
    void clear();

Q_SIGNALS:
    void removeEntry(SendCoinsEntry *entry);
    void payAmountChanged();
    void subtractFeeFromAmountChanged();

private Q_SLOTS:
    void deleteClicked();
    void on_payTo_textChanged(const QString &address);
    void on_addressBookButton_clicked();
    void on_pasteButton_clicked();
    void updateDisplayUnit();

private:
    SendCoinsRecipient recipient;
    Ui::SendCoinsEntry *ui;
    WalletModel *model;
    const PlatformStyle *platformStyle;

    bool updateLabel(const QString &address);
};

#endif
