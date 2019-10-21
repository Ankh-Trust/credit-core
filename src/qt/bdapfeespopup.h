// Copyright (c) 2019 Duality Blockchain Solutions Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CREDIT_QT_BDAPFEESPOPUP_H
#define CREDIT_QT_BDAPFEESPOPUP_H

#include "bdap/fees.h"
#include "bdappage.h"
#include "creditunits.h"

#include <QDialog>
#include <QMessageBox>
#include <QObject>
#include <QPushButton>
#include <QTranslator>

bool bdapFeesPopup(QWidget *parentDialog, const opcodetype& opCodeAction, const opcodetype& opCodeObject, BDAP::ObjectType inputAccountType, int unit, int32_t regMonths = DEFAULT_REGISTRATION_MONTHS);

#endif // CREDIT_QT_BDAPFEESPOPUP_H