
// Copyright (c) 2016-2019 Duality Blockchain Solutions Developers
// Copyright (c) 2014-2019 The Dash Core Developers
// Copyright (c) 2009-2019 The Bitcoin Developers
// Copyright (c) 2009-2019 Satoshi Nakamoto
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "servicenodelist.h"
#include "ui_servicenodelist.h"

#include "clientmodel.h"
#include "guiutil.h"
#include "walletmodel.h"

#include "activeservicenode.h"
#include "servicenode-sync.h"
#include "servicenodeconfig.h"
#include "servicenodeman.h"
#include "init.h"
#include "sync.h"
#include "wallet/wallet.h"

#include <QMessageBox>
#include <QTimer>

int GetOffsetFromUtc()
{
#if QT_VERSION < 0x050200
    const QDateTime dateTime1 = QDateTime::currentDateTime();
    const QDateTime dateTime2 = QDateTime(dateTime1.date(), dateTime1.time(), Qt::UTC);
    return dateTime1.secsTo(dateTime2);
#else
    return QDateTime::currentDateTime().offsetFromUtc();
#endif
}

ServiceNodeList::ServiceNodeList(const PlatformStyle* platformStyle, QWidget* parent) : QWidget(parent),
                                                                              ui(new Ui::ServiceNodeList),
                                                                              clientModel(0),
                                                                              walletModel(0)
{
    ui->setupUi(this);

    ui->startButton->setEnabled(true);

    int columnAliasWidth = 100;
    int columnAddressWidth = 200;
    int columnProtocolWidth = 60;
    int columnStatusWidth = 80;
    int columnActiveWidth = 130;
    int columnLastSeenWidth = 130;

    ui->tableWidgetMyServiceNodes->setColumnWidth(0, columnAliasWidth);
    ui->tableWidgetMyServiceNodes->setColumnWidth(1, columnAddressWidth);
    ui->tableWidgetMyServiceNodes->setColumnWidth(2, columnProtocolWidth);
    ui->tableWidgetMyServiceNodes->setColumnWidth(3, columnStatusWidth);
    ui->tableWidgetMyServiceNodes->setColumnWidth(4, columnActiveWidth);
    ui->tableWidgetMyServiceNodes->setColumnWidth(5, columnLastSeenWidth);

    ui->tableWidgetServiceNodes->setColumnWidth(0, columnAddressWidth);
    ui->tableWidgetServiceNodes->setColumnWidth(1, columnProtocolWidth);
    ui->tableWidgetServiceNodes->setColumnWidth(2, columnStatusWidth);
    ui->tableWidgetServiceNodes->setColumnWidth(3, columnActiveWidth);
    ui->tableWidgetServiceNodes->setColumnWidth(4, columnLastSeenWidth);

    ui->tableWidgetMyServiceNodes->setContextMenuPolicy(Qt::CustomContextMenu);

    QAction* startAliasAction = new QAction(tr("Start alias"), this);
    contextMenu = new QMenu();
    contextMenu->addAction(startAliasAction);
    connect(ui->tableWidgetMyServiceNodes, SIGNAL(customContextMenuRequested(const QPoint&)), this, SLOT(showContextMenu(const QPoint&)));
    connect(startAliasAction, SIGNAL(triggered()), this, SLOT(on_startButton_clicked()));

    timer = new QTimer(this);
    connect(timer, SIGNAL(timeout()), this, SLOT(updateNodeList()));
    connect(timer, SIGNAL(timeout()), this, SLOT(updateMyNodeList()));
    timer->start(1000);

    fFilterUpdated = false;
    nTimeFilterUpdated = GetTime();
    updateNodeList();
}

ServiceNodeList::~ServiceNodeList()
{
    delete ui;
}

void ServiceNodeList::setClientModel(ClientModel* model)
{
    this->clientModel = model;
    if (model) {
        // try to update list when ServiceNode count changes
        connect(clientModel, SIGNAL(strServiceNodesChanged(QString)), this, SLOT(updateNodeList()));
    }
}

void ServiceNodeList::setWalletModel(WalletModel* model)
{
    this->walletModel = model;
}

void ServiceNodeList::showContextMenu(const QPoint& point)
{
    QTableWidgetItem* item = ui->tableWidgetMyServiceNodes->itemAt(point);
    if (item)
        contextMenu->exec(QCursor::pos());
}

void ServiceNodeList::StartAlias(std::string strAlias)
{
    std::string strStatusHtml;
    strStatusHtml += "<center>Alias: " + strAlias;

    for (const auto& dne : servicenodeConfig.getEntries()) {
        if (dne.getAlias() == strAlias) {
            std::string strError;
            CServiceNodeBroadcast dnb;

            bool fSuccess = CServiceNodeBroadcast::Create(dne.getIp(), dne.getPrivKey(), dne.getTxHash(), dne.getOutputIndex(), strError, dnb);

            int nDoS;
            if (fSuccess && !snodeman.CheckDnbAndUpdateServiceNodeList(NULL, dnb, nDoS, *g_connman)) {
                strError = "Failed to verify DNB";
                fSuccess = false;
            }

            if (fSuccess) {
                strStatusHtml += "<br>Successfully started ServiceNode.";
                snodeman.NotifyServiceNodeUpdates(*g_connman);
            } else {
                strStatusHtml += "<br>Failed to start ServiceNode.<br>Error: " + strError;
            }
            break;
        }
    }
    strStatusHtml += "</center>";

    QMessageBox msg;
    msg.setText(QString::fromStdString(strStatusHtml));
    msg.exec();

    updateMyNodeList(true);
}

void ServiceNodeList::StartAll(std::string strCommand)
{
    int nCountSuccessful = 0;
    int nCountFailed = 0;
    std::string strFailedHtml;

    for (const auto& dne : servicenodeConfig.getEntries()) {
        std::string strError;
        CServiceNodeBroadcast dnb;

        int32_t nOutputIndex = 0;
        if (!ParseInt32(dne.getOutputIndex(), &nOutputIndex)) {
            continue;
        }

        COutPoint outpoint = COutPoint(uint256S(dne.getTxHash()), nOutputIndex);

        if (strCommand == "start-missing" && snodeman.Has(outpoint))
            continue;

        bool fSuccess = CServiceNodeBroadcast::Create(dne.getIp(), dne.getPrivKey(), dne.getTxHash(), dne.getOutputIndex(), strError, dnb);

        int nDoS;
        if (fSuccess && !snodeman.CheckDnbAndUpdateServiceNodeList(NULL, dnb, nDoS, *g_connman)) {
            strError = "Failed to verify DNB";
            fSuccess = false;
        }

        if (fSuccess) {
            nCountSuccessful++;
            snodeman.NotifyServiceNodeUpdates(*g_connman);
        } else {
            nCountFailed++;
            strFailedHtml += "\nFailed to start " + dne.getAlias() + ". Error: " + strError;
        }
    }

    std::string returnObj;
    returnObj = strprintf("Successfully started %d ServiceNodes, failed to start %d, total %d", nCountSuccessful, nCountFailed, nCountFailed + nCountSuccessful);
    if (nCountFailed > 0) {
        returnObj += strFailedHtml;
    }

    QMessageBox msg;
    msg.setText(QString::fromStdString(returnObj));
    msg.exec();

    updateMyNodeList(true);
}

void ServiceNodeList::updateMyServiceNodeInfo(QString strAlias, QString strAddr, const COutPoint& outpoint)
{
    bool fOldRowFound = false;
    int nNewRow = 0;

    for (int i = 0; i < ui->tableWidgetMyServiceNodes->rowCount(); i++) {
        if (ui->tableWidgetMyServiceNodes->item(i, 0)->text() == strAlias) {
            fOldRowFound = true;
            nNewRow = i;
            break;
        }
    }

    if (nNewRow == 0 && !fOldRowFound) {
        nNewRow = ui->tableWidgetMyServiceNodes->rowCount();
        ui->tableWidgetMyServiceNodes->insertRow(nNewRow);
    }

    servicenode_info_t infoDn;
    bool fFound = snodeman.GetServiceNodeInfo(outpoint, infoDn);

    QTableWidgetItem* aliasItem = new QTableWidgetItem(strAlias);
    QTableWidgetItem* addrItem = new QTableWidgetItem(fFound ? QString::fromStdString(infoDn.addr.ToString()) : strAddr);
    QTableWidgetItem* protocolItem = new QTableWidgetItem(QString::number(fFound ? infoDn.nProtocolVersion : -1));
    QTableWidgetItem* statusItem = new QTableWidgetItem(QString::fromStdString(fFound ? CServiceNode::StateToString(infoDn.nActiveState) : "MISSING"));
    QTableWidgetItem* activeSecondsItem = new QTableWidgetItem(QString::fromStdString(DurationToDHMS(fFound ? (infoDn.nTimeLastPing - infoDn.sigTime) : 0)));
    QTableWidgetItem* lastSeenItem = new QTableWidgetItem(QString::fromStdString(DateTimeStrFormat("%Y-%m-%d %H:%M",
        fFound ? infoDn.nTimeLastPing + GetOffsetFromUtc() : 0)));
    QTableWidgetItem* pubkeyItem = new QTableWidgetItem(QString::fromStdString(fFound ? CCreditAddress(infoDn.pubKeyCollateralAddress.GetID()).ToString() : ""));

    ui->tableWidgetMyServiceNodes->setItem(nNewRow, 0, aliasItem);
    ui->tableWidgetMyServiceNodes->setItem(nNewRow, 1, addrItem);
    ui->tableWidgetMyServiceNodes->setItem(nNewRow, 2, protocolItem);
    ui->tableWidgetMyServiceNodes->setItem(nNewRow, 3, statusItem);
    ui->tableWidgetMyServiceNodes->setItem(nNewRow, 4, activeSecondsItem);
    ui->tableWidgetMyServiceNodes->setItem(nNewRow, 5, lastSeenItem);
    ui->tableWidgetMyServiceNodes->setItem(nNewRow, 6, pubkeyItem);
}

void ServiceNodeList::updateMyNodeList(bool fForce)
{
    TRY_LOCK(cs_mydnlist, fLockAcquired);
    if (!fLockAcquired) {
        return;
    }
    static int64_t nTimeMyListUpdated = 0;

    // automatically update my ServiceNode list only once in MY_SERVICENODELIST_UPDATE_SECONDS seconds,
    // this update still can be triggered manually at any time via button click
    int64_t nSecondsTillUpdate = nTimeMyListUpdated + MY_SERVICENODELIST_UPDATE_SECONDS - GetTime();
    ui->secondsLabel->setText(QString::number(nSecondsTillUpdate));

    if (nSecondsTillUpdate > 0 && !fForce)
        return;
    nTimeMyListUpdated = GetTime();

    // Find selected row
    QItemSelectionModel* selectionModel = ui->tableWidgetMyServiceNodes->selectionModel();
    QModelIndexList selected = selectionModel->selectedRows();
    int nSelectedRow = selected.count() ? selected.at(0).row() : 0;

    ui->tableWidgetServiceNodes->setSortingEnabled(false);
    for (const auto& dne : servicenodeConfig.getEntries()) {
        int32_t nOutputIndex = 0;
        if (!ParseInt32(dne.getOutputIndex(), &nOutputIndex)) {
            continue;
        }

        updateMyServiceNodeInfo(QString::fromStdString(dne.getAlias()), QString::fromStdString(dne.getIp()), COutPoint(uint256S(dne.getTxHash()), nOutputIndex));
    }
    ui->tableWidgetMyServiceNodes->selectRow(nSelectedRow);
    ui->tableWidgetServiceNodes->setSortingEnabled(true);

    // reset "timer"
    ui->secondsLabel->setText("0");
}

void ServiceNodeList::updateNodeList()
{
    TRY_LOCK(cs_dnlist, fLockAcquired);
    if (!fLockAcquired) {
        return;
    }

    static int64_t nTimeListUpdated = GetTime();

    // to prevent high cpu usage update only once in SERVICENODELIST_UPDATE_SECONDS seconds
    // or SERVICENODELIST_FILTER_COOLDOWN_SECONDS seconds after filter was last changed
    int64_t nSecondsToWait = fFilterUpdated ? nTimeFilterUpdated - GetTime() + SERVICENODELIST_FILTER_COOLDOWN_SECONDS : nTimeListUpdated - GetTime() + SERVICENODELIST_UPDATE_SECONDS;

    if (fFilterUpdated)
        ui->countLabel->setText(QString::fromStdString(strprintf("Please wait... %d", nSecondsToWait)));
    if (nSecondsToWait > 0)
        return;

    nTimeListUpdated = GetTime();
    fFilterUpdated = false;

    QString strToFilter;
    ui->countLabel->setText("Updating...");
    ui->tableWidgetServiceNodes->setSortingEnabled(false);
    ui->tableWidgetServiceNodes->clearContents();
    ui->tableWidgetServiceNodes->setRowCount(0);
    std::map<COutPoint, CServiceNode> mapServiceNodes = snodeman.GetFullServiceNodeMap();
    int offsetFromUtc = GetOffsetFromUtc();

    for (const auto& dnpair : mapServiceNodes) {
        CServiceNode dn = dnpair.second;
        // populate list
        // Address, Protocol, Status, Active Seconds, Last Seen, Pub Key
        QTableWidgetItem* addressItem = new QTableWidgetItem(QString::fromStdString(dn.addr.ToString()));
        QTableWidgetItem* protocolItem = new QTableWidgetItem(QString::number(dn.nProtocolVersion));
        QTableWidgetItem* statusItem = new QTableWidgetItem(QString::fromStdString(dn.GetStatus()));
        QTableWidgetItem* activeSecondsItem = new QTableWidgetItem(QString::fromStdString(DurationToDHMS(dn.lastPing.sigTime - dn.sigTime)));
        QTableWidgetItem* lastSeenItem = new QTableWidgetItem(QString::fromStdString(DateTimeStrFormat("%Y-%m-%d %H:%M", dn.lastPing.sigTime + offsetFromUtc)));
        QTableWidgetItem* pubkeyItem = new QTableWidgetItem(QString::fromStdString(CCreditAddress(dn.pubKeyCollateralAddress.GetID()).ToString()));

        if (strCurrentFilter != "") {
            strToFilter = addressItem->text() + " " +
                          protocolItem->text() + " " +
                          statusItem->text() + " " +
                          activeSecondsItem->text() + " " +
                          lastSeenItem->text() + " " +
                          pubkeyItem->text();
            if (!strToFilter.contains(strCurrentFilter))
                continue;
        }

        ui->tableWidgetServiceNodes->insertRow(0);
        ui->tableWidgetServiceNodes->setItem(0, 0, addressItem);
        ui->tableWidgetServiceNodes->setItem(0, 1, protocolItem);
        ui->tableWidgetServiceNodes->setItem(0, 2, statusItem);
        ui->tableWidgetServiceNodes->setItem(0, 3, activeSecondsItem);
        ui->tableWidgetServiceNodes->setItem(0, 4, lastSeenItem);
        ui->tableWidgetServiceNodes->setItem(0, 5, pubkeyItem);
    }

    ui->countLabel->setText(QString::number(ui->tableWidgetServiceNodes->rowCount()));
    ui->tableWidgetServiceNodes->setSortingEnabled(true);
}

void ServiceNodeList::on_filterLineEdit_textChanged(const QString& strFilterIn)
{
    strCurrentFilter = strFilterIn;
    nTimeFilterUpdated = GetTime();
    fFilterUpdated = true;
    ui->countLabel->setText(QString::fromStdString(strprintf("Please wait... %d", SERVICENODELIST_FILTER_COOLDOWN_SECONDS)));
}

void ServiceNodeList::on_startButton_clicked()
{
    std::string strAlias;
    {
        LOCK(cs_mydnlist);
        // Find selected node alias
        QItemSelectionModel* selectionModel = ui->tableWidgetMyServiceNodes->selectionModel();
        QModelIndexList selected = selectionModel->selectedRows();

        if (selected.count() == 0)
            return;

        QModelIndex index = selected.at(0);
        int nSelectedRow = index.row();
        strAlias = ui->tableWidgetMyServiceNodes->item(nSelectedRow, 0)->text().toStdString();
    }

    // Display message box
    QMessageBox::StandardButton retval = QMessageBox::question(this, tr("Confirm ServiceNode start"),
        tr("Are you sure you want to start ServiceNode %1?").arg(QString::fromStdString(strAlias)),
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel);

    if (retval != QMessageBox::Yes)
        return;

    WalletModel::EncryptionStatus encStatus = walletModel->getEncryptionStatus();

    if (encStatus == walletModel->Locked || encStatus == walletModel->UnlockedForMixingOnly) {
        WalletModel::UnlockContext ctx(walletModel->requestUnlock());

        if (!ctx.isValid())
            return; // Unlock wallet was cancelled

        StartAlias(strAlias);
        return;
    }

    StartAlias(strAlias);
}

void ServiceNodeList::on_startAllButton_clicked()
{
    // Display message box
    QMessageBox::StandardButton retval = QMessageBox::question(this, tr("Confirm all ServiceNodes start"),
        tr("Are you sure you want to start ALL ServiceNodes?"),
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel);

    if (retval != QMessageBox::Yes)
        return;

    WalletModel::EncryptionStatus encStatus = walletModel->getEncryptionStatus();

    if (encStatus == walletModel->Locked || encStatus == walletModel->UnlockedForMixingOnly) {
        WalletModel::UnlockContext ctx(walletModel->requestUnlock());

        if (!ctx.isValid())
            return; // Unlock wallet was cancelled

        StartAll();
        return;
    }

    StartAll();
}

void ServiceNodeList::on_startMissingButton_clicked()
{
    if (!servicenodeSync.IsServiceNodeListSynced()) {
        QMessageBox::critical(this, tr("Command is not available right now"),
            tr("You can't use this command until ServiceNode list is synced"));
        return;
    }

    // Display message box
    QMessageBox::StandardButton retval = QMessageBox::question(this,
        tr("Confirm missing ServiceNodes start"),
        tr("Are you sure you want to start MISSING ServiceNodes?"),
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel);

    if (retval != QMessageBox::Yes)
        return;

    WalletModel::EncryptionStatus encStatus = walletModel->getEncryptionStatus();

    if (encStatus == walletModel->Locked || encStatus == walletModel->UnlockedForMixingOnly) {
        WalletModel::UnlockContext ctx(walletModel->requestUnlock());

        if (!ctx.isValid())
            return; // Unlock wallet was cancelled

        StartAll("start-missing");
        return;
    }

    StartAll("start-missing");
}

void ServiceNodeList::on_tableWidgetMyServiceNodes_itemSelectionChanged()
{
    if (ui->tableWidgetMyServiceNodes->selectedItems().count() > 0) {
        ui->startButton->setEnabled(true);
    }
}

void ServiceNodeList::on_UpdateButton_clicked()
{
    updateMyNodeList(true);
}
