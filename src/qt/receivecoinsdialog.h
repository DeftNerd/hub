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

#ifndef FLOWEE_QT_RECEIVECOINSDIALOG_H
#define FLOWEE_QT_RECEIVECOINSDIALOG_H

#include "guiutil.h"

#include <QDialog>
#include <QHeaderView>
#include <QItemSelection>
#include <QKeyEvent>
#include <QMenu>
#include <QPoint>
#include <QVariant>

class OptionsModel;
class PlatformStyle;
class WalletModel;

namespace Ui {
    class ReceiveCoinsDialog;
}

QT_BEGIN_NAMESPACE
class QModelIndex;
QT_END_NAMESPACE

/** Dialog for requesting payment of bitcoins */
class ReceiveCoinsDialog : public QDialog
{
    Q_OBJECT

public:
    enum ColumnWidths {
        DATE_COLUMN_WIDTH = 130,
        LABEL_COLUMN_WIDTH = 120,
        AMOUNT_MINIMUM_COLUMN_WIDTH = 160,
        MINIMUM_COLUMN_WIDTH = 130
    };

    explicit ReceiveCoinsDialog(const PlatformStyle *platformStyle, QWidget *parent = 0);
    ~ReceiveCoinsDialog();

    void setModel(WalletModel *model);

public Q_SLOTS:
    void clear();
    void reject();
    void accept();

protected:
    virtual void keyPressEvent(QKeyEvent *event);

private:
    Ui::ReceiveCoinsDialog *ui;
    GUIUtil::TableViewLastColumnResizingFixer *columnResizingFixer;
    WalletModel *model;
    QMenu *contextMenu;
    const PlatformStyle *platformStyle;

    void copyColumnToClipboard(int column);
    virtual void resizeEvent(QResizeEvent *event);

private Q_SLOTS:
    void on_receiveButton_clicked();
    void on_showRequestButton_clicked();
    void on_removeRequestButton_clicked();
    void on_recentRequestsView_doubleClicked(const QModelIndex &index);
    void recentRequestsView_selectionChanged(const QItemSelection &selected, const QItemSelection &deselected);
    void updateDisplayUnit();
    void showMenu(const QPoint &point);
    void copyLabel();
    void copyMessage();
    void copyAmount();
};

#endif
