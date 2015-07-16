/*
 *  Copyright (c) 2014, Airbitz
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms are permitted provided that
 *  the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice, this
 *  list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *  this list of conditions and the following disclaimer in the documentation
 *  and/or other materials provided with the distribution.
 *  3. Redistribution or use of modified source code requires the express written
 *  permission of Airbitz Inc.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 *  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 *  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 *  ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 *  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  The views and conclusions contained in the software and documentation are those
 *  of the authors and should not be interpreted as representing official policies,
 *  either expressed or implied, of the Airbitz Project.
 */

#include "WatcherBridge.hpp"
#include "TxUpdater.hpp"
#include "Testnet.hpp"
#include "Watcher.hpp"
#include "../General.hpp"
#include "../Tx.hpp"
#include "../spend/Broadcast.hpp"
#include "../spend/Inputs.hpp"
#include "../spend/Outputs.hpp"
#include "../spend/Spend.hpp"
#include "../util/FileIO.hpp"
#include "../util/Util.hpp"
#include <algorithm>
#include <list>
#include <memory>
#include <unordered_map>

namespace abcd {

#define FALLBACK_OBELISK "tcp://obelisk.airbitz.co:9091"
#define TESTNET_OBELISK "tcp://obelisk-testnet.airbitz.co:9091"

struct PendingSweep
{
    bc::payment_address address;
    abcd::wif_key key;
    bool done;

    tABC_Sweep_Done_Callback fCallback;
    void *pData;
};

struct WatcherInfo
{
    ~WatcherInfo()
    {
        ABC_WalletIDFree(wallet);
    }

    WatcherInfo()
    {
        wallet.account = nullptr;
        wallet.szUUID = nullptr;
    }

    Watcher watcher;
    std::set<std::string> addresses;
    std::list<PendingSweep> sweeping;
    tABC_WalletID wallet;

    // Callback:
    tABC_BitCoin_Event_Callback fAsyncCallback;
    void *pData;
};

static std::map<std::string, std::unique_ptr<WatcherInfo>> watchers_;

// The last obelisk server we connected to:
static unsigned gLastObelisk = 0;

static tABC_CC     ABC_BridgeTxDetailsSplit(tABC_WalletID self, const char *szTxID, tABC_TxOutput ***iarr, unsigned int *pInCount, tABC_TxOutput ***oarr, unsigned int *pOutCount, int64_t *pAmount, int64_t *pFees, tABC_Error *pError);
static tABC_CC     ABC_BridgeDoSweep(WatcherInfo *watcherInfo, PendingSweep& sweep, tABC_Error *pError);
static void        ABC_BridgeQuietCallback(WatcherInfo *watcherInfo);
static void        ABC_BridgeTxCallback(WatcherInfo *watcherInfo, const libbitcoin::transaction_type& tx, tABC_BitCoin_Event_Callback fAsyncBitCoinEventCallback, void *pData);

static Status
watcherFind(WatcherInfo *&result, tABC_WalletID self)
{
    std::string id = self.szUUID;
    auto row = watchers_.find(id);
    if (row == watchers_.end())
        return ABC_ERROR(ABC_CC_Synchronizing, "Cannot find watcher for " + id);

    result = row->second.get();
    return Status();
}

Status
watcherFind(Watcher *&result, tABC_WalletID self)
{
    WatcherInfo *watcherInfo = nullptr;
    ABC_CHECK(watcherFind(watcherInfo, self));

    result = &watcherInfo->watcher;
    return Status();
}

static Status
watcherLoad(tABC_WalletID self)
{
    Watcher *watcher = nullptr;
    ABC_CHECK(watcherFind(watcher, self));

    DataChunk data;
    ABC_CHECK(fileLoad(data, watcherPath(self.szUUID)));
    if (!watcher->load(data))
        return ABC_ERROR(ABC_CC_Error, "Unable to load serialized watcher");

    return Status();
}

Status
watcherSave(tABC_WalletID self)
{
    Watcher *watcher = nullptr;
    ABC_CHECK(watcherFind(watcher, self));

    auto data = watcher->serialize();;
    ABC_CHECK(fileSave(data, watcherPath(self.szUUID)));

    return Status();
}

std::string
watcherPath(const std::string &walletId)
{
    return walletDir(walletId) + "watcher.ser";
}

tABC_CC ABC_BridgeSweepKey(tABC_WalletID self,
                           tABC_U08Buf key,
                           bool compressed,
                           tABC_Sweep_Done_Callback fCallback,
                           void *pData,
                           tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    bc::ec_secret ec_key;
    bc::ec_point ec_addr;
    bc::payment_address address;
    PendingSweep sweep;

    WatcherInfo *watcherInfo = nullptr;
    ABC_CHECK_NEW(watcherFind(watcherInfo, self));

    // Decode key and address:
    ABC_CHECK_ASSERT(key.size() == ec_key.size(),
        ABC_CC_Error, "Bad key size");
    std::copy(key.begin(), key.end(), ec_key.data());
    ec_addr = bc::secret_to_public_key(ec_key, compressed);
    address.set(pubkeyVersion(), bc::bitcoin_short_hash(ec_addr));

    // Start the sweep:
    sweep.address = address;
    sweep.key = abcd::wif_key{ec_key, compressed};
    sweep.done = false;
    sweep.fCallback = fCallback;
    sweep.pData = pData;
    watcherInfo->sweeping.push_back(sweep);
    watcherInfo->watcher.watch_address(address);

exit:
    return cc;
}

tABC_CC ABC_BridgeWatcherStart(tABC_WalletID self,
                               tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;

    std::string id = self.szUUID;

    if (watchers_.end() != watchers_.find(id))
        ABC_RET_ERROR(ABC_CC_Error, ("Watcher already exists for " + id).c_str());

    watchers_[id].reset(new WatcherInfo());
    ABC_CHECK_RET(ABC_WalletIDCopy(&watchers_[id]->wallet, self, pError));

    watcherLoad(self); // Failure is not fatal

exit:
    return cc;
}

tABC_CC ABC_BridgeWatcherLoop(tABC_WalletID self,
                              tABC_BitCoin_Event_Callback fAsyncCallback,
                              void *pData,
                              tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    Watcher::block_height_callback heightCallback;
    Watcher::tx_callback txCallback;
    Watcher::tx_sent_callback sendCallback;
    Watcher::quiet_callback on_quiet;
    Watcher::fail_callback failCallback;

    WatcherInfo *watcherInfo = nullptr;
    ABC_CHECK_NEW(watcherFind(watcherInfo, self));
    watcherInfo->fAsyncCallback = fAsyncCallback;
    watcherInfo->pData = pData;

    txCallback = [watcherInfo, fAsyncCallback, pData] (const libbitcoin::transaction_type& tx)
    {
        ABC_BridgeTxCallback(watcherInfo, tx, fAsyncCallback, pData);
    };
    watcherInfo->watcher.set_tx_callback(txCallback);

    heightCallback = [watcherInfo, fAsyncCallback, pData](const size_t height)
    {
        tABC_Error error;
        ABC_TxBlockHeightUpdate(height, fAsyncCallback, pData, &error);
        watcherSave(watcherInfo->wallet); // Failure is not fatal
    };
    watcherInfo->watcher.set_height_callback(heightCallback);

    on_quiet = [watcherInfo]()
    {
        ABC_BridgeQuietCallback(watcherInfo);
    };
    watcherInfo->watcher.set_quiet_callback(on_quiet);

    failCallback = [watcherInfo]()
    {
        tABC_Error error;
        ABC_BridgeWatcherConnect(watcherInfo->wallet, &error);
    };
    watcherInfo->watcher.set_fail_callback(failCallback);

    watcherInfo->watcher.loop();

exit:
    return cc;
}

tABC_CC ABC_BridgeWatcherConnect(tABC_WalletID self, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    tABC_GeneralInfo *ppInfo = NULL;
    const char *szServer = FALLBACK_OBELISK;

    Watcher *watcher = nullptr;
    ABC_CHECK_NEW(watcherFind(watcher, self));

    // Pick a server:
    if (isTestnet())
    {
        szServer = TESTNET_OBELISK;
    }
    else if (ABC_CC_Ok == ABC_GeneralGetInfo(&ppInfo, pError) &&
        0 < ppInfo->countObeliskServers)
    {
        ++gLastObelisk;
        if (ppInfo->countObeliskServers <= gLastObelisk)
            gLastObelisk = 0;
        szServer = ppInfo->aszObeliskServers[gLastObelisk];
    }

    // Connect:
    ABC_DebugLog("WalletID:%s Connecting to %s\n", self.szUUID, szServer);
    watcher->connect(szServer);

exit:
    ABC_GeneralFreeInfo(ppInfo);
    return cc;
}

tABC_CC ABC_BridgeWatchAddr(tABC_WalletID self,
                            const char *pubAddress,
                            tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;

    ABC_DebugLog("Watching %s for %s\n", pubAddress, self.szUUID);
    bc::payment_address addr;

    WatcherInfo *watcherInfo = nullptr;
    ABC_CHECK_NEW(watcherFind(watcherInfo, self));

    if (!addr.set_encoded(pubAddress))
    {
        cc = ABC_CC_Error;
        ABC_DebugLog("Invalid pubAddress %s\n", pubAddress);
        goto exit;
    }
    watcherInfo->addresses.insert(pubAddress);
    watcherInfo->watcher.watch_address(addr);

exit:
    return cc;
}

tABC_CC ABC_BridgePrioritizeAddress(tABC_WalletID self,
                                    const char *szAddress,
                                    tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    bc::payment_address addr;

    Watcher *watcher = nullptr;
    ABC_CHECK_NEW(watcherFind(watcher, self));

    if (szAddress)
    {
        if (!addr.set_encoded(szAddress))
        {
            cc = ABC_CC_Error;
            ABC_DebugLog("Invalid szAddress %s\n", szAddress);
            goto exit;
        }
    }

    watcher->prioritize_address(addr);

exit:
    return cc;
}

tABC_CC ABC_BridgeWatcherDisconnect(tABC_WalletID self, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;

    Watcher *watcher = nullptr;
    ABC_CHECK_NEW(watcherFind(watcher, self));

    watcher->disconnect();

exit:
    return cc;
}

tABC_CC ABC_BridgeWatcherStop(tABC_WalletID self, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;

    Watcher *watcher = nullptr;
    ABC_CHECK_NEW(watcherFind(watcher, self));

    watcher->disconnect();
    watcher->stop();

exit:
    return cc;
}

tABC_CC ABC_BridgeWatcherDelete(tABC_WalletID self, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;

    watcherSave(self); // Failure is not fatal
    watchers_.erase(self.szUUID);

    return cc;
}

tABC_CC
ABC_BridgeTxHeight(tABC_WalletID self, const char *szTxId, unsigned int *height, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    int height_;
    bc::hash_digest txId;

    Watcher *watcher = nullptr;
    ABC_CHECK_NEW(watcherFind(watcher, self));

    txId = bc::decode_hash(szTxId);
    if (!watcher->get_tx_height(txId, height_))
    {
        cc = ABC_CC_Synchronizing;
    }
    *height = height_;
exit:
    return cc;
}

tABC_CC
ABC_BridgeTxBlockHeight(tABC_WalletID self, unsigned int *height, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;

    Watcher *watcher = nullptr;
    ABC_CHECK_NEW(watcherFind(watcher, self));

    *height = watcher->get_last_block_height();
    if (*height == 0)
    {
        cc = ABC_CC_Synchronizing;
    }
exit:
    return cc;
}

tABC_CC ABC_BridgeTxDetails(tABC_WalletID self, const char *szTxID,
                            tABC_TxOutput ***paOutputs, unsigned int *pCount,
                            int64_t *pAmount, int64_t *pFees, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    tABC_TxOutput **paInArr = NULL;
    tABC_TxOutput **paOutArr = NULL;
    tABC_TxOutput **farr = NULL;
    unsigned int outCount = 0;
    unsigned int inCount = 0;
    unsigned int totalCount = 0;

    ABC_CHECK_RET(ABC_BridgeTxDetailsSplit(self, szTxID,
                                           &paInArr, &inCount,
                                           &paOutArr, &outCount,
                                           pAmount, pFees, pError));
    farr = (tABC_TxOutput **) malloc(sizeof(tABC_TxOutput *) * (inCount + outCount));
    totalCount = outCount + inCount;
    for (unsigned i = 0; i < totalCount; ++i) {
        if (i < inCount) {
            farr[i] = paInArr[i];
            paInArr[i] = NULL;
        } else {
            farr[i] = paOutArr[i - inCount];
            paOutArr[i - inCount] = NULL;
        }
    }
    *paOutputs = farr;
    *pCount = totalCount;
    farr = NULL;
exit:
    ABC_TxFreeOutputs(farr, inCount + outCount);
    ABC_TxFreeOutputs(paInArr, inCount);
    ABC_TxFreeOutputs(paOutArr, outCount);
    return cc;
}

tABC_CC ABC_BridgeTxDetailsSplit(tABC_WalletID self, const char *szTxID,
                                 tABC_TxOutput ***paInputs, unsigned int *pInCount,
                                 tABC_TxOutput ***paOutputs, unsigned int *pOutCount,
                                 int64_t *pAmount, int64_t *pFees,
                                 tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    tABC_TxOutput **paInArr = NULL;
    tABC_TxOutput **paOutArr = NULL;
    bc::transaction_type tx;
    unsigned int idx = 0, iCount = 0, oCount = 0;
    int64_t fees = 0;
    int64_t totalInSatoshi = 0, totalOutSatoshi = 0, totalMeSatoshi = 0, totalMeInSatoshi = 0;

    WatcherInfo *watcherInfo = nullptr;
    ABC_CHECK_NEW(watcherFind(watcherInfo, self));

    bc::hash_digest txid;
    txid = bc::decode_hash(szTxID);

    tx = watcherInfo->watcher.find_tx(txid);

    idx = 0;
    iCount = tx.inputs.size();
    paInArr = (tABC_TxOutput **) malloc(sizeof(tABC_TxOutput *) * iCount);
    for (auto i : tx.inputs)
    {
        bc::payment_address addr;
        bc::extract(addr, i.script);
        auto prev = i.previous_output;

        // Create output
        tABC_TxOutput *out = (tABC_TxOutput *) malloc(sizeof(tABC_TxOutput));
        out->input = true;
        ABC_STRDUP(out->szTxId, bc::encode_hex(prev.hash).c_str());
        ABC_STRDUP(out->szAddress, addr.encoded().c_str());

        auto tx = watcherInfo->watcher.find_tx(prev.hash);
        if (prev.index < tx.outputs.size())
        {
            out->value = tx.outputs[prev.index].value;
            totalInSatoshi += tx.outputs[prev.index].value;
            auto row = watcherInfo->addresses.find(addr.encoded());
            if  (row != watcherInfo->addresses.end())
                totalMeInSatoshi += tx.outputs[prev.index].value;
        } else {
            out->value = 0;
        }
        paInArr[idx] = out;
        idx++;
    }

    idx = 0;
    oCount = tx.outputs.size();
    paOutArr = (tABC_TxOutput **) malloc(sizeof(tABC_TxOutput *) * oCount);
    for (auto o : tx.outputs)
    {
        bc::payment_address addr;
        bc::extract(addr, o.script);
        // Create output
        tABC_TxOutput *out = (tABC_TxOutput *) malloc(sizeof(tABC_TxOutput));
        out->input = false;
        out->value = o.value;
        ABC_STRDUP(out->szAddress, addr.encoded().c_str());
        ABC_STRDUP(out->szTxId, szTxID);

        // Do we own this address?
        auto row = watcherInfo->addresses.find(addr.encoded());
        if  (row != watcherInfo->addresses.end())
        {
            totalMeSatoshi += o.value;
        }
        totalOutSatoshi += o.value;
        paOutArr[idx] = out;
        idx++;
    }
    fees = totalInSatoshi - totalOutSatoshi;
    totalMeSatoshi -= totalMeInSatoshi;

    *paInputs = paInArr;
    *pInCount = iCount;
    *paOutputs = paOutArr;
    *pOutCount = oCount;
    *pAmount = totalMeSatoshi;
    *pFees = fees;
    paInArr = NULL;
    paOutArr = NULL;
exit:
    ABC_TxFreeOutputs(paInArr, iCount);
    ABC_TxFreeOutputs(paOutArr, oCount);
    return cc;
}

/**
 * Filters a transaction list, removing any that aren't found in the
 * watcher database.
 * @param aTransactions The array to filter. This will be modified in-place.
 * @param pCount        The array length. This will be updated upon return.
 */
tABC_CC ABC_BridgeFilterTransactions(tABC_WalletID self,
                                     tABC_TxInfo **aTransactions,
                                     unsigned int *pCount,
                                     tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    tABC_TxInfo *const *end = aTransactions + *pCount;
    tABC_TxInfo *const *si = aTransactions;
    tABC_TxInfo **di = aTransactions;

    Watcher *watcher = nullptr;
    ABC_CHECK_NEW(watcherFind(watcher, self));

    while (si < end)
    {
        tABC_TxInfo *pTx = *si++;

        int height;
        bc::hash_digest txid;
        txid = bc::decode_hash(pTx->szMalleableTxId);
        if (watcher->get_tx_height(txid, height))
        {
            *di++ = pTx;
        }
        else
        {
            ABC_TxFreeTransaction(pTx);
        }
    }
    *pCount = di - aTransactions;

exit:
    return cc;
}

static
tABC_CC ABC_BridgeDoSweep(WatcherInfo *watcherInfo,
                          PendingSweep& sweep,
                          tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    char *szID = NULL;
    char *szAddress = NULL;
    uint64_t funds = 0;
    abcd::unsigned_transaction utx;
    bc::transaction_output_type output;
    abcd::key_table keys;
    std::string malTxId, txId;

    // Find utxos for this address:
    auto utxos = watcherInfo->watcher.get_utxos(sweep.address);

    // Bail out if there are no funds to sweep:
    if (!utxos.size())
    {
        // Tell the GUI if there were funds in the past:
        if (watcherInfo->watcher.db().has_history(sweep.address))
        {
            if (sweep.fCallback)
            {
                sweep.fCallback(ABC_CC_Ok, NULL, 0);
            }
            else
            {
                if (watcherInfo->fAsyncCallback)
                {
                    tABC_AsyncBitCoinInfo info;
                    info.eventType = ABC_AsyncEventType_IncomingSweep;
                    info.sweepSatoshi = 0;
                    info.szTxID = NULL;
                    watcherInfo->fAsyncCallback(&info);
                }
            }
            sweep.done = true;
        }
        return ABC_CC_Ok;
    }

    // There are some utxos, so send them to ourselves:
    tABC_TxDetails details;
    memset(&details, 0, sizeof(tABC_TxDetails));
    details.amountSatoshi = 0;
    details.amountCurrency = 0;
    details.amountFeesAirbitzSatoshi = 0;
    details.amountFeesMinersSatoshi = 0;
    details.szName = const_cast<char*>("");
    details.szCategory = const_cast<char*>("");
    details.szNotes = const_cast<char*>("");
    details.attributes = 0x2;

    // Create a new receive request:
    ABC_CHECK_RET(ABC_TxCreateReceiveRequest(watcherInfo->wallet,
        &details, &szID, false, pError));
    ABC_CHECK_RET(ABC_TxGetRequestAddress(watcherInfo->wallet, szID,
        &szAddress, pError));

    // Build a transaction:
    utx.tx.version = 1;
    utx.tx.locktime = 0;
    for (auto &utxo : utxos)
    {
        bc::transaction_input_type input;
        input.sequence = 0xffffffff;
        input.previous_output = utxo.point;
        funds += utxo.value;
        utx.tx.inputs.push_back(input);
    }
    ABC_CHECK_ASSERT(outputDust + 10000 <= funds, ABC_CC_InsufficientFunds, "Not enough funds");
    funds -= 10000;
    output.value = funds;
    ABC_CHECK_NEW(outputScriptForAddress(output.script, szAddress));
    utx.tx.outputs.push_back(output);

    // Now sign that:
    keys[sweep.address] = sweep.key;
    ABC_CHECK_SYS(abcd::gather_challenges(utx, watcherInfo->watcher), "gather_challenges");
    ABC_CHECK_SYS(abcd::sign_tx(utx, keys), "sign_tx");

    // Send:
    {
        bc::data_chunk raw_tx(satoshi_raw_size(utx.tx));
        bc::satoshi_save(utx.tx, raw_tx.begin());
        ABC_CHECK_NEW(broadcastTx(raw_tx));
    }

    // Save the transaction in the database:
    malTxId = bc::encode_hex(bc::hash_transaction(utx.tx));
    txId = ABC_BridgeNonMalleableTxId(utx.tx);
    ABC_CHECK_RET(ABC_TxSweepSaveTransaction(watcherInfo->wallet,
        txId.c_str(), malTxId.c_str(), funds, &details, pError));

    // Done:
    if (sweep.fCallback)
    {
        sweep.fCallback(ABC_CC_Ok, txId.c_str(), output.value);
    }
    else
    {
        if (watcherInfo->fAsyncCallback)
        {
            tABC_AsyncBitCoinInfo info;
            info.eventType = ABC_AsyncEventType_IncomingSweep;
            info.sweepSatoshi = output.value;
            ABC_STRDUP(info.szTxID, txId.c_str());
            watcherInfo->fAsyncCallback(&info);
            ABC_FREE_STR(info.szTxID);
        }
    }
    sweep.done = true;
    watcherInfo->watcher.send_tx(utx.tx);

exit:
    ABC_FREE_STR(szID);
    ABC_FREE_STR(szAddress);

    return cc;
}

static
void ABC_BridgeQuietCallback(WatcherInfo *watcherInfo)
{
    // If we are sweeping any keys, do that now:
    for (auto& sweep: watcherInfo->sweeping)
    {
        tABC_CC cc;
        tABC_Error error;

        cc = ABC_BridgeDoSweep(watcherInfo, sweep, &error);
        if (cc != ABC_CC_Ok)
        {
            if (sweep.fCallback)
                sweep.fCallback(cc, NULL, 0);
            sweep.done = true;
        }
    }

    // Remove completed ones:
    watcherInfo->sweeping.remove_if([](const PendingSweep& sweep) {
        return sweep.done; });
}

static
void ABC_BridgeTxCallback(WatcherInfo *watcherInfo, const libbitcoin::transaction_type& tx,
                          tABC_BitCoin_Event_Callback fAsyncBitCoinEventCallback,
                          void *pData)
{
    tABC_CC cc = ABC_CC_Ok;
    tABC_Error error;
    tABC_Error *pError = &error;
    int64_t fees = 0;
    int64_t totalInSatoshi = 0, totalOutSatoshi = 0, totalMeSatoshi = 0, totalMeInSatoshi = 0;
    tABC_TxOutput **iarr = NULL, **oarr = NULL;
    unsigned int idx = 0, iCount = 0, oCount = 0;
    std::string txId, malTxId;

    if (watcherInfo == NULL)
    {
        cc = ABC_CC_Error;
        goto exit;
    }

    txId = ABC_BridgeNonMalleableTxId(tx);
    malTxId = bc::encode_hex(bc::hash_transaction(tx));

    idx = 0;
    iCount = tx.inputs.size();
    iarr = (tABC_TxOutput **) malloc(sizeof(tABC_TxOutput *) * iCount);
    for (auto i : tx.inputs)
    {
        bc::payment_address addr;
        bc::extract(addr, i.script);
        auto prev = i.previous_output;

        // Create output
        tABC_TxOutput *out = (tABC_TxOutput *) malloc(sizeof(tABC_TxOutput));
        out->input = true;
        ABC_STRDUP(out->szTxId, bc::encode_hex(prev.hash).c_str());
        ABC_STRDUP(out->szAddress, addr.encoded().c_str());

        // Check prevouts for values
        auto tx = watcherInfo->watcher.find_tx(prev.hash);
        if (prev.index < tx.outputs.size())
        {
            out->value = tx.outputs[prev.index].value;
            totalInSatoshi += tx.outputs[prev.index].value;
            auto row = watcherInfo->addresses.find(addr.encoded());
            if  (row != watcherInfo->addresses.end())
                totalMeInSatoshi += tx.outputs[prev.index].value;
        }
        iarr[idx] = out;
        idx++;
    }

    idx = 0;
    oCount = tx.outputs.size();
    oarr = (tABC_TxOutput **) malloc(sizeof(tABC_TxOutput *) * oCount);
    for (auto o : tx.outputs)
    {
        bc::payment_address addr;
        bc::extract(addr, o.script);
        // Create output
        tABC_TxOutput *out = (tABC_TxOutput *) malloc(sizeof(tABC_TxOutput));
        out->input = false;
        out->value = o.value;
        ABC_STRDUP(out->szAddress, addr.encoded().c_str());
        ABC_STRDUP(out->szTxId, malTxId.c_str());

        // Do we own this address?
        auto row = watcherInfo->addresses.find(addr.encoded());
        if  (row != watcherInfo->addresses.end())
        {
            totalMeSatoshi += o.value;
        }
        totalOutSatoshi += o.value;

        oarr[idx] = out;
        idx++;
    }
    if (totalMeSatoshi == 0 && totalMeInSatoshi == 0)
    {
        ABC_DebugLog("values == 0, this tx does not concern me.\n");
        goto exit;
    }
    fees = totalInSatoshi - totalOutSatoshi;
    totalMeSatoshi -= totalMeInSatoshi;

    ABC_DebugLog("calling ABC_TxReceiveTransaction\n");
    ABC_DebugLog("Total Me: %d, Total In: %d, Total Out: %d, Fees: %d\n",
                    totalMeSatoshi, totalInSatoshi, totalOutSatoshi, fees);
    ABC_CHECK_RET(
        ABC_TxReceiveTransaction(
            watcherInfo->wallet,
            totalMeSatoshi, fees,
            iarr, iCount,
            oarr, oCount,
            txId.c_str(), malTxId.c_str(),
            fAsyncBitCoinEventCallback,
            pData,
            &error));
    watcherSave(watcherInfo->wallet); // Failure is not fatal

exit:
    ABC_FREE(oarr);
    ABC_FREE(iarr);
}

std::string
ABC_BridgeNonMalleableTxId(bc::transaction_type tx)
{
    for (auto& input: tx.inputs)
        input.script = bc::script_type();
    return bc::encode_hex(bc::hash_transaction(tx, bc::sighash::all));
}

Status
watcherBridgeRawTx(tABC_WalletID self, const char *szTxID,
    DataChunk &result)
{
    Watcher *watcher = nullptr;
    ABC_CHECK(watcherFind(watcher, self));

    auto tx = watcher->find_tx(bc::decode_hash(szTxID));
    result.resize(satoshi_raw_size(tx));
    bc::satoshi_save(tx, result.begin());

    return Status();
}

} // namespace abcd
