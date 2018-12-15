// Copyright (c) 2011-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/transactionrecord.h>

#include <consensus/consensus.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <policy/policy.h>
#include <timedata.h>
#include <validation.h>

#include <stdint.h>


/* Return positive answer if transaction should be shown in list.
 */
bool TransactionRecord::showTransaction()
{
    // There are currently no cases where we hide transactions, but
    // we may want to use this in the future for things like RBF.
    return true;
}

/*
 * Decompose CWallet transaction to model transaction records.
 */
QList<TransactionRecord> TransactionRecord::decomposeTransaction(const interfaces::WalletTx& wtx)
{
    QList<TransactionRecord> parts;
    int64_t nTime = wtx.time;
    CAmount nCredit = valueFor(wtx.credit, ::policyAsset);
    CAmount nDebit = valueFor(wtx.debit, ::policyAsset);
    CAmount nNet = nCredit - nDebit;
    uint256 hash = wtx.tx->GetHash();
    std::map<std::string, std::string> mapValue = wtx.value_map;

    if (nNet > 0 || wtx.is_coinbase)
    {
        //
        // Credit
        //
        for(unsigned int i = 0; i < wtx.tx->vout.size(); i++)
        {
            isminetype mine = wtx.txout_is_mine[i];
            if(mine)
            {
                TransactionRecord sub(hash, nTime);
                CTxDestination address;
                sub.idx = i; // vout index
                sub.amount = wtx.txout_amounts[i];
                sub.involvesWatchAddress = mine & ISMINE_WATCH_ONLY;
                if (wtx.txout_address_is_mine[i])
                {
                    // Received by Bitcoin Address
                    sub.type = TransactionRecord::RecvWithAddress;
                    sub.address = EncodeDestination(wtx.txout_address[i]);
                    sub.asset = wtx.txout_assets[i];
                }
                else
                {
                    // Received by IP connection (deprecated features), or a multisignature or other non-simple transaction
                    sub.type = TransactionRecord::RecvFromOther;
                    sub.address = mapValue["from"];
                    sub.asset = wtx.txout_assets[i];
                }
                if (wtx.is_coinbase)
                {
                    // Generated
                    sub.type = TransactionRecord::Generated;
                    sub.asset = wtx.txout_assets[i];
                }

                parts.append(sub);
            }
        }
    }
    else
    {
        bool involvesWatchAddress = false;
        isminetype fAllFromMe = ISMINE_SPENDABLE;
        std::set<CAsset> assets_issued_to_me_only;
        CAmountMap assets_received_by_me_only;
        for (unsigned int i = 0; i < wtx.tx->vout.size(); i++)
        {
            const CAsset& asset = wtx.txout_assets[i];
            if (assets_received_by_me_only.count(asset) && assets_received_by_me_only.at(asset) < 0) {
                // Already known to be received by not-me
                continue;
            }
            isminetype mine = wtx.txout_address_is_mine[i];
            if (!mine) {
                assets_received_by_me_only[asset] = -1;
            } else {
                assets_received_by_me_only[asset] += wtx.txout_amounts[i];
            }
        }

        for (size_t i = 0; i < wtx.tx->vin.size(); ++i)
        {
            isminetype mine = wtx.txin_is_mine[i];
            if(mine & ISMINE_WATCH_ONLY) involvesWatchAddress = true;
            if(fAllFromMe > mine) fAllFromMe = mine;
            const CAsset& asset = wtx.txin_issuance_asset[i];
            const CAmount& asset_amount = wtx.txin_issuance_asset_amount[i];
            const CAsset& token = wtx.txin_issuance_token[i];
            const CAmount& token_amount = wtx.txin_issuance_token_amount[i];
            if (!asset.IsNull()) {
                if (assets_received_by_me_only.count(asset) == 0) {
                    continue;
                }
                if (asset_amount == assets_received_by_me_only.at(asset)) {
                    // Special case: collapse the chain of issue, send, receive to just an issue
                    assets_issued_to_me_only.insert(asset);
                    continue;
                }

                TransactionRecord sub(hash, nTime);
                sub.involvesWatchAddress = involvesWatchAddress;
                sub.asset = asset;
                sub.amount = asset_amount;
                sub.type = TransactionRecord::IssuedAsset;
                parts.append(sub);
            }
            if (!token.IsNull()) {
                if (assets_received_by_me_only.count(token) == 0) {
                    continue;
                }
                if (token_amount == assets_received_by_me_only.at(asset)) {
                    // Special case: collapse the chain of issue, send, receive to just an issue
                    assets_issued_to_me_only.insert(asset);
                    continue;
                }

                TransactionRecord sub(hash, nTime);
                sub.involvesWatchAddress = involvesWatchAddress;
                sub.asset = token;
                sub.amount = token_amount;
                sub.type = TransactionRecord::IssuedAsset;
                parts.append(sub);
            }

            if (!wtx.txin_issuance_token[i].IsNull()) {
                TransactionRecord sub(hash, nTime);
                sub.involvesWatchAddress = involvesWatchAddress;
                sub.asset = wtx.txin_issuance_token[i];
                sub.amount = wtx.txin_issuance_token_amount[i];
                sub.type = TransactionRecord::IssuedAsset;
                parts.append(sub);
            }
        }

        isminetype fAllToMe = ISMINE_SPENDABLE;
        for (unsigned int i = 0; i < wtx.txout_is_mine.size(); ++i) {
            const isminetype mine = wtx.txout_is_mine[i];
            const CTxOut txout = wtx.tx->vout[i];
            if (txout.IsFee()) {
                // explicit fee; ignore
                continue;
            }
            if(mine & ISMINE_WATCH_ONLY) involvesWatchAddress = true;
            if(fAllToMe > mine) fAllToMe = mine;
        }

        if (fAllFromMe && fAllToMe)
        {
            // Payment to self
            CAmount nChange = valueFor(wtx.change, ::policyAsset);

            parts.append(TransactionRecord(hash, nTime, TransactionRecord::SendToSelf, "",
                            -(nDebit - nChange) + (nCredit - nChange), ::policyAsset));
            parts.last().involvesWatchAddress = involvesWatchAddress;   // maybe pass to TransactionRecord as constructor argument
        }
        else if (fAllFromMe)
        {

            //
            // Debit
            //

            for (unsigned int nOut = 0; nOut < wtx.tx->vout.size(); nOut++)
            {

                const CTxOut& txout = wtx.tx->vout[nOut];
                const CAsset& asset = wtx.txout_assets[nOut];

                if(wtx.txout_is_mine[nOut] || txout.IsFee())
                {
                    // Ignore parts sent to self, as this is usually the change
                    // from a transaction sent back to our own address.
                    continue;
                }

                // Short-circuit when it's an issuance to self
                if (assets_issued_to_me_only.count(asset) == 0) {
                    continue;
                }

                TransactionRecord sub(hash, nTime);
                sub.idx = nOut;
                sub.involvesWatchAddress = involvesWatchAddress;
                sub.amount = -wtx.txout_amounts[nOut];
                sub.asset = asset;

                if (!boost::get<CNoDestination>(&wtx.txout_address[nOut]))
                {
                    // Sent to Bitcoin Address
                    sub.type = TransactionRecord::SendToAddress;
                    sub.address = EncodeDestination(wtx.txout_address[nOut]);
                }
                else
                {
                    // Sent to IP, or other non-address transaction like OP_EVAL
                    sub.type = TransactionRecord::SendToOther;
                    sub.address = mapValue["to"];
                }
                if (assets_issued_to_me_only.count(asset)) {
                    sub.type = TransactionRecord::IssuedAsset;
                }
                parts.append(sub);
            }

            for (const auto& tx_fee : GetFeeMap(*wtx.tx)) {
                if (!tx_fee.second) continue;
                TransactionRecord sub(hash, nTime);
                sub.type = TransactionRecord::Fee;
                sub.asset = tx_fee.first;
                sub.amount = -tx_fee.second;
                parts.append(sub);
            }
        }
        else
        {
            //
            // Mixed debit transaction, can't break down payees
            //
            parts.append(TransactionRecord(hash, nTime, TransactionRecord::Other, "", nNet, CAsset()));
            parts.last().involvesWatchAddress = involvesWatchAddress;
        }
    }

    return parts;
}

void TransactionRecord::updateStatus(const interfaces::WalletTxStatus& wtx, int numBlocks, int64_t adjustedTime)
{
    // Determine transaction status

    // Sort order, unrecorded transactions sort to the top
    int typesort;
    switch (type) {
    case Fee:
        typesort = 0;
        break;
    case IssuedAsset:
        typesort = 1;
        break;
    case SendToAddress:
    case SendToOther:
    case SendToSelf:
        typesort = 2;
        break;
    case RecvWithAddress:
    case RecvFromOther:
        typesort = 3;
        break;
    default:
        typesort = 10;
    }
    status.sortKey = strprintf("%010d-%01d-%010u-%03d-%d",
        wtx.block_height,
        wtx.is_coinbase ? 1 : 0,
        wtx.time_received,
        idx,
        typesort);
    status.countsForBalance = wtx.is_trusted && !(wtx.blocks_to_maturity > 0);
    status.depth = wtx.depth_in_main_chain;
    status.cur_num_blocks = numBlocks;

    if (!wtx.is_final)
    {
        if (wtx.lock_time < LOCKTIME_THRESHOLD)
        {
            status.status = TransactionStatus::OpenUntilBlock;
            status.open_for = wtx.lock_time - numBlocks;
        }
        else
        {
            status.status = TransactionStatus::OpenUntilDate;
            status.open_for = wtx.lock_time;
        }
    }
    // For generated transactions, determine maturity
    else if(type == TransactionRecord::Generated)
    {
        if (wtx.blocks_to_maturity > 0)
        {
            status.status = TransactionStatus::Immature;

            if (wtx.is_in_main_chain)
            {
                status.matures_in = wtx.blocks_to_maturity;
            }
            else
            {
                status.status = TransactionStatus::NotAccepted;
            }
        }
        else
        {
            status.status = TransactionStatus::Confirmed;
        }
    }
    else
    {
        if (status.depth < 0)
        {
            status.status = TransactionStatus::Conflicted;
        }
        else if (status.depth == 0)
        {
            status.status = TransactionStatus::Unconfirmed;
            if (wtx.is_abandoned)
                status.status = TransactionStatus::Abandoned;
        }
        else if (status.depth < RecommendedNumConfirmations)
        {
            status.status = TransactionStatus::Confirming;
        }
        else
        {
            status.status = TransactionStatus::Confirmed;
        }
    }
    status.needsUpdate = false;
}

bool TransactionRecord::statusUpdateNeeded(int numBlocks) const
{
    return status.cur_num_blocks != numBlocks || status.needsUpdate;
}

QString TransactionRecord::getTxHash() const
{
    return QString::fromStdString(hash.ToString());
}

int TransactionRecord::getOutputIndex() const
{
    return idx;
}
