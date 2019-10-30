
// Copyright (c) 2016-2019 Duality Blockchain Solutions Developers
// Copyright (c) 2014-2017 The Dash Core Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CREDIT_SERVICENODECONFIG_H
#define CREDIT_SERVICENODECONFIG_H

#include "chainparams.h"
#include "netbase.h"
#include "util.h"

class CServiceNodeConfig;
extern CServiceNodeConfig servicenodeConfig;

class CServiceNodeConfig
{
public:
    class CServiceNodeEntry
    {
    private:
        std::string alias;
        std::string ip;
        std::string privKey;
        std::string txHash;
        std::string outputIndex;

    public:
        CServiceNodeEntry(const std::string& alias, const std::string& ip, const std::string& privKey, const std::string& txHash, const std::string& outputIndex)
        {
            this->alias = alias;
            this->ip = ip;
            this->privKey = privKey;
            this->txHash = txHash;
            this->outputIndex = outputIndex;
        }

        const std::string& getAlias() const
        {
            return alias;
        }

        void setAlias(const std::string& alias)
        {
            this->alias = alias;
        }

        const std::string& getOutputIndex() const
        {
            return outputIndex;
        }

        void setOutputIndex(const std::string& outputIndex)
        {
            this->outputIndex = outputIndex;
        }

        const std::string& getPrivKey() const
        {
            return privKey;
        }

        void setPrivKey(const std::string& privKey)
        {
            this->privKey = privKey;
        }

        const std::string& getTxHash() const
        {
            return txHash;
        }

        void setTxHash(const std::string& txHash)
        {
            this->txHash = txHash;
        }

        const std::string& getIp() const
        {
            return ip;
        }

        void setIp(const std::string& ip)
        {
            this->ip = ip;
        }
    };

    CServiceNodeConfig()
    {
        entries = std::vector<CServiceNodeEntry>();
    }

    void clear();
    bool read(std::string& strErr);
    void add(const std::string& alias, const std::string& ip, const std::string& privKey, const std::string& txHash, const std::string& outputIndex);

    std::vector<CServiceNodeEntry>& getEntries()
    {
        return entries;
    }

    int getCount()
    {
        return (int)entries.size();
    }

private:
    std::vector<CServiceNodeEntry> entries;
};


#endif // CREDIT_SERVICENODECONFIG_H
