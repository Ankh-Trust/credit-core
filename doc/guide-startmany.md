## Setting up your Wallet

### Create New Wallet Addresses

1. Open the QT Wallet.
2. Click the Receive tab.
3. Fill in the form to request a payment.
    * Label: sn01
    * Amount: 50000 (optional)
    * Click *Request payment*
5. Click the *Copy Address* button

Create a new wallet address for each ServiceNode.

Close your QT Wallet.

### Send 50,000 0AC to New Addresses

Send exactly 50,000 0AC to each new address created above.

### Create New ServiceNode Private Keys

Open your QT Wallet and go to console (from the menu select Tools => Debug Console)

Issue the following:

```servicenode genkey```

*Note: A ServiceNode private key will need to be created for each ServiceNode you run. You should not use the same ServiceNode private key for multiple ServiceNodes.*

Close your QT Wallet.

## <a name="servicenodeconf"></a>Create servicenode.conf file

Remember... this is local. Make sure your QT is not running.

Create the servicenode.conf file in the same directory as your wallet.dat.

Copy the ServiceNode private key and correspondig collateral output transaction that holds the 50K CREDIT.

*Please note, the ServiceNode priviate key is not the same as a wallet private key. Never put your wallet private key in the servicenode.conf file. That is equivalent to putting your 50,000 0AC on the remote server and defeats the purpose of a hot/cold setup.*

### Get the collateral output

Open your QT Wallet and go to console (from the menu select Tools => Debug Console)

Issue the following:

```servicenode outputs```

Make note of the hash (which is your collaterla_output) and index.

### Enter your ServiceNode details into your servicenode.conf file
[From the credit github repo](https://github.com/duality-solutions/credit/blob/master/doc/servicenode_conf.md)

The new servicenode.conf format consists of a space separated text file. Each line consisting of an alias, IP address followed by port, ServiceNode private key, collateral output transaction id and collateral output index.
(!!! Currently not implemented: "donation address and donation percentage (the latter two are optional and should be in format "address:percentage")." !!!)

```
alias ipaddress:port servicenode_private_key collateral_output collateral_output_index (!!! see above "donationin_address:donation_percentage" !!!)
```



Example:

```
sn01 127.0.0.1:33600 93HaYBVUCYjEMeeH1Y4sBGLALQZE1Yc1K64xiqgX37tGBDQL8Xg 2bcd3c84c84f87eaa86e4e56834c92927a07f9e18718810b92e0d0324456a67c 0
sn02 127.0.0.2:33600 93WaAb3htPJEV8E9aQcN23Jt97bPex7YvWfgMDTUdWJvzmrMqey aa9f1034d973377a5e733272c3d0eced1de22555ad45d6b24abadff8087948d4 0 (!!! see above "7gnwGHt17heGpG9Crfeh4KGpYNFugPhJdh:25" !!!)
```

## Update credit.conf on server

If you generated a new ServiceNode private key, you will need to update the remote credit.conf files.

Shut down the daemon and then edit the file.

```sudo nano .credit/credit.conf```

### Edit the servicenodepairingkey
If you generated a new ServiceNode private key, you will need to update the servicenodepairingkey value in your remote credit.conf file.

## Start your ServiceNodes

### Remote

If your remote server is not running, start your remote daemon as you normally would.

I usually confirm that remote is on the correct block by issuing:

```creditd getinfo```

And compare with the official explorer at http://explorer.creditpay.io/chain/Credit

### Local

Finally... time to start from local.

#### Open up your QT Wallet

From the menu select Tools => Debug Console

If you want to review your servicenode.conf setting before starting the ServiceNodes, issue the following in the Debug Console:

```servicenode list-conf```

Give it the eye-ball test. If satisfied, you can start your nodes one of two ways.

1. servicenode start-alias [alias_from_servicenode.conf]. Example ```servicenode start-alias sn01```
2. servicenode start-many
