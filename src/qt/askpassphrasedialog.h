// Copyright (c) 2016-2019 Duality Blockchain Solutions Developers
// Copyright (c) 2014-2019 The Dash Core Developers
// Copyright (c) 2009-2019 The Bitcoin Developers
// Copyright (c) 2009-2019 Satoshi Nakamoto
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CREDIT_QT_ASKPASSPHRASEDIALOG_H
#define CREDIT_QT_ASKPASSPHRASEDIALOG_H

#include <QDialog>

#include <support/allocators/secure.h>

class WalletModel;

namespace Ui
{
class AskPassphraseDialog;
}

/** Multifunctional dialog to ask for passphrases. Used for encryption, unlocking, and changing the passphrase.
 */
class AskPassphraseDialog : public QDialog
{
    Q_OBJECT

public:
    enum Mode {
        Encrypt,      /**< Ask passphrase twice and encrypt */
        UnlockMixing, /**< Ask passphrase and unlock only for mixing */
        Unlock,       /**< Ask passphrase and unlock */
        ChangePass,   /**< Ask old passphrase + new passphrase twice */
        Decrypt       /**< Ask passphrase and decrypt wallet */
    };

    explicit AskPassphraseDialog(Mode mode, QWidget* parent);
    ~AskPassphraseDialog();

    void accept();

    void setModel(WalletModel* model);

private:
    Ui::AskPassphraseDialog* ui;
    Mode mode;
    WalletModel* model;
    bool fCapsLock;

private Q_SLOTS:
    void textChanged();
    void secureClearPassFields();
    void toggleShowPassword(bool);

protected:
    bool event(QEvent* event);
    bool eventFilter(QObject* object, QEvent* event);
};

#endif // CREDIT_QT_ASKPASSPHRASEDIALOG_H
