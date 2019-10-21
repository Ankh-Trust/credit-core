// Copyright (c) 2019-2019 The Ankh Core Developers
// Copyright (c) 2016-2019 Duality Blockchain Solutions Developers
// Copyright (c) 2014-2019 The Dash Core Developers
// Copyright (c) 2009-2019 The Bitcoin Developers
// Copyright (c) 2009-2019 Satoshi Nakamoto
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CREDIT_QT_CREDITADDRESSVALIDATOR_H
#define CREDIT_QT_CREDITADDRESSVALIDATOR_H

#include <QValidator>

/** Base58 entry widget validator, checks for valid characters and
 * removes some whitespace.
 */
class CreditAddressEntryValidator : public QValidator
{
    Q_OBJECT

public:
    explicit CreditAddressEntryValidator(QObject* parent);

    State validate(QString& input, int& pos) const;
};

/** Credit address widget validator, checks for a valid Credit address.
 */
class CreditAddressCheckValidator : public QValidator
{
    Q_OBJECT

public:
    explicit CreditAddressCheckValidator(QObject* parent);

    State validate(QString& input, int& pos) const;
};

#endif // CREDIT_QT_CREDITADDRESSVALIDATOR_H
