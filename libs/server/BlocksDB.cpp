/*
 * This file is part of the Flowee project
 * Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2017-2018 Tom Zander <tomz@freedommail.ch>
 * Copyright (c) 2017 Calin Culianu <calin.culianu@gmail.com>
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

#include "BlocksDB.h"
#include "BlocksDB_p.h"
#include "chainparams.h"
#include "Application.h"
#include "init.h" // for StartShutdown
#include "hash.h"
#include <validation/Engine.h>

#include "chain.h"
#include "main.h"
#include "uint256.h"
#include <SettingsDefaults.h>
#include <primitives/FastBlock.h>
#include <boost/thread.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

static const char DB_BLOCK_FILES = 'f';
static const char DB_TXINDEX = 't';
static const char DB_BLOCK_INDEX = 'b';

static const char DB_FLAG = 'F';
static const char DB_REINDEX_FLAG = 'R';
static const char DB_LAST_BLOCK = 'l';

namespace {
CBlockIndex * InsertBlockIndex(uint256 hash)
{
    if (hash.IsNull())
        return nullptr;

    // Return existing
    auto answer = Blocks::Index::get(hash);
    if (answer)
        return answer;

    // Create new
    CBlockIndex* pindexNew = new CBlockIndex();
    pindexNew->phashBlock = Blocks::Index::insert(hash, pindexNew);
    return pindexNew;
}

bool LoadExternalBlockFile(const CDiskBlockPos &pos)
{
    static_assert(MESSAGE_START_SIZE == 4, "We assume 4");
    int64_t nStart = GetTimeMillis();

    Streaming::ConstBuffer dataFile = Blocks::DB::instance()->loadBlockFile(pos.nFile);
    if (!dataFile.isValid()) {
        logWarning(Log::DB) << "LoadExternalBlockFile: Unable to open file" << pos.nFile;
        return false;
    }

    CBlockFileInfo info;

    auto validation = Application::instance()->validation();
    const int blockHeaderMessage = reinterpret_cast<const int&>(Params().MessageStart());
    const char *buf = dataFile.begin();
    while (buf < dataFile.end()) {
        buf = (const char*) memchr(buf, blockHeaderMessage, (dataFile.end() - buf) / sizeof(int));
        if (buf == nullptr) {
            // no valid block header found; don't complain
            break;
        }
        buf += 4;
        uint32_t blockSize = le32toh(*((uint32_t*)(buf)));
        if (blockSize < 80)
            continue;
        buf += 4;

        validation->waitForSpace();
        validation->addBlock(CDiskBlockPos(pos.nFile, buf - dataFile.begin()));
        ++info.nBlocks;
        buf += blockSize;
        info.nSize = buf - dataFile.begin();
    }
    if (info.nBlocks > 0) {
        logCritical(Log::DB) << "Loaded" << info.nBlocks << "blocks from external file" << pos.nFile << "in" << (GetTimeMillis() - nStart) << "ms";
        Blocks::DB::instance()->priv()->foundBlockFile(pos.nFile, info);
    }

    return true;
}

void reimportBlockFiles()
{
    const CChainParams& chainparams = Params();
    RenameThread("bitcoin-loadblk");
    if (Blocks::DB::instance()->reindexing() == Blocks::ScanningFiles) {
        int nFile = 0;
        while (true) {
            if (!LoadExternalBlockFile(CDiskBlockPos(nFile, 0)))
                break;
            if (Application::closingDown())
                return;
            nFile++;
        }
        Blocks::DB::instance()->setReindexing(Blocks::ParsingBlocks);
    }
    Application::instance()->validation()->waitValidationFinished();
    Blocks::DB::instance()->setReindexing(Blocks::NoReindex);
    FlushStateToDisk();
    logCritical(Log::Bitcoin) << "Reindexing finished";
    // To avoid ending up in a situation without genesis block, re-try initializing (no-op if reindexing worked):
    InitBlockIndex(chainparams);

    if (GetBoolArg("-stopafterblockimport", Settings::DefaultStopAfterBlockImport)) {
        logCritical(Log::Bitcoin) << "Stopping after block import";
        StartShutdown();
    }
}

}

Blocks::DB* Blocks::DB::s_instance = nullptr;

Blocks::DB *Blocks::DB::instance()
{
    return Blocks::DB::s_instance;
}

void Blocks::DB::createInstance(size_t nCacheSize, bool fWipe)
{
    delete Blocks::DB::s_instance;
    Blocks::DB::s_instance = new Blocks::DB(nCacheSize, false, fWipe);
}

void Blocks::DB::createTestInstance(size_t nCacheSize)
{
    delete Blocks::DB::s_instance;
    Blocks::DB::s_instance = new Blocks::DB(nCacheSize, true);
}

void Blocks::DB::shutdown()
{
    delete s_instance;
    s_instance = 0;
}

void Blocks::DB::startBlockImporter()
{
    if (s_instance->reindexing() != NoReindex)
        Application::createThread(std::bind(&reimportBlockFiles));
}

Blocks::DB::DB(size_t nCacheSize, bool fMemory, bool fWipe)
    : CDBWrapper(GetDataDir() / "blocks" / "index", nCacheSize, fMemory, fWipe),
      d(new DBPrivate())
{
    int state;
    bool exists = Read(DB_REINDEX_FLAG, state);
    if (exists) {
        if (state == 1)
            d->reindexing = Blocks::ScanningFiles;
        else
            d->reindexing = Blocks::ParsingBlocks;
    } else {
        d->reindexing = Blocks::NoReindex;
    }
    loadConfig();
}

bool Blocks::DB::ReadBlockFileInfo(int nFile, CBlockFileInfo &info) {
    return Read(std::make_pair(DB_BLOCK_FILES, nFile), info);
}

bool Blocks::DB::ReadLastBlockFile(int &nFile) {
    return Read(DB_LAST_BLOCK, nFile);
}

bool Blocks::DB::WriteBatchSync(const std::vector<std::pair<int, const CBlockFileInfo*> >& fileInfo, int nLastFile, const std::vector<const CBlockIndex*>& blockinfo) {
    CDBBatch batch(&GetObfuscateKey());
    for (std::vector<std::pair<int, const CBlockFileInfo*> >::const_iterator it=fileInfo.begin(); it != fileInfo.end(); it++) {
        batch.Write(std::make_pair(DB_BLOCK_FILES, it->first), *it->second);
    }
    batch.Write(DB_LAST_BLOCK, nLastFile);
    for (std::vector<const CBlockIndex*>::const_iterator it=blockinfo.begin(); it != blockinfo.end(); it++) {
        batch.Write(std::make_pair(DB_BLOCK_INDEX, (*it)->GetBlockHash()), CDiskBlockIndex(*it));
    }
    return WriteBatch(batch, true);
}

bool Blocks::DB::ReadTxIndex(const uint256 &txid, CDiskTxPos &pos) {
    return Read(std::make_pair(DB_TXINDEX, txid), pos);
}

bool Blocks::DB::WriteTxIndex(const std::vector<std::pair<uint256, CDiskTxPos> >&vect) {
    CDBBatch batch(&GetObfuscateKey());
    for (std::vector<std::pair<uint256,CDiskTxPos> >::const_iterator it=vect.begin(); it!=vect.end(); it++)
        batch.Write(std::make_pair(DB_TXINDEX, it->first), it->second);
    return WriteBatch(batch);
}

bool Blocks::DB::WriteFlag(const std::string &name, bool fValue) {
    return Write(std::make_pair(DB_FLAG, name), fValue ? '1' : '0');
}

bool Blocks::DB::ReadFlag(const std::string &name, bool &fValue) {
    char ch;
    if (!Read(std::make_pair(DB_FLAG, name), ch))
        return false;
    fValue = ch == '1';
    return true;
}

bool Blocks::DB::CacheAllBlockInfos()
{
    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    pcursor->Seek(std::make_pair(DB_BLOCK_INDEX, uint256()));
    int maxFile = 0;

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char, uint256> key;
        if (pcursor->GetKey(key) && key.first == DB_BLOCK_INDEX) {
            CDiskBlockIndex diskindex;
            if (pcursor->GetValue(diskindex)) {
                // Construct block index object
                CBlockIndex* pindexNew = InsertBlockIndex(diskindex.GetBlockHash());
                pindexNew->pprev          = InsertBlockIndex(diskindex.hashPrev);
                pindexNew->nHeight        = diskindex.nHeight;
                pindexNew->nFile          = diskindex.nFile;
                maxFile = std::max(pindexNew->nFile, maxFile);
                pindexNew->nDataPos       = diskindex.nDataPos;
                pindexNew->nUndoPos       = diskindex.nUndoPos;
                pindexNew->nVersion       = diskindex.nVersion;
                pindexNew->hashMerkleRoot = diskindex.hashMerkleRoot;
                pindexNew->nTime          = diskindex.nTime;
                pindexNew->nBits          = diskindex.nBits;
                pindexNew->nNonce         = diskindex.nNonce;
                pindexNew->nStatus        = diskindex.nStatus;
                pindexNew->nTx            = diskindex.nTx;

                pcursor->Next();
            } else {
                return error("CacheAllBlockInfos(): failed to read row");
            }
        } else {
            break;
        }
    }
    d->datafiles.resize(maxFile);
    d->revertDatafiles.resize(maxFile);

    std::lock_guard<std::mutex> lock_(d->blockIndexLock);
    for (auto iter = d->indexMap.begin(); iter != d->indexMap.end(); ++iter) {
        iter->second->BuildSkip();
    }
    for (auto iter = d->indexMap.begin(); iter != d->indexMap.end(); ++iter) {
        appendHeader(iter->second);
    }

    return true;
}

Blocks::ReindexingState Blocks::DB::reindexing() const
{
    return d->reindexing;
}

void Blocks::DB::setReindexing(Blocks::ReindexingState state)
{
    if (d->reindexing == state)
        return;
    d->reindexing = state;
    switch (state) {
    case Blocks::NoReindex:
        Erase(DB_REINDEX_FLAG);
        break;
    case Blocks::ScanningFiles:
        Write(DB_REINDEX_FLAG, 1);
        break;
    case Blocks::ParsingBlocks:
        Write(DB_REINDEX_FLAG, 2);
        break;
    }
}

static FILE* OpenDiskFile(const CDiskBlockPos &pos, const char *prefix, bool fReadOnly)
{
    if (pos.IsNull())
        return NULL;
    boost::filesystem::path path = Blocks::getFilepathForIndex(pos.nFile, prefix, true);
    boost::filesystem::create_directories(path.parent_path());
    FILE* file = fopen(path.string().c_str(), "rb+");
    if (!file && !fReadOnly)
        file = fopen(path.string().c_str(), "wb+");
    if (!file) {
        LogPrintf("Unable to open file %s\n", path.string());
        return NULL;
    }
    if (pos.nPos) {
        if (fseek(file, pos.nPos, SEEK_SET)) {
            LogPrintf("Unable to seek to position %u of %s\n", pos.nPos, path.string());
            fclose(file);
            return NULL;
        }
    }
    return file;
}

FILE* Blocks::openFile(const CDiskBlockPos &pos, bool fReadOnly) {
    return OpenDiskFile(pos, "blk", fReadOnly);
}

FILE* Blocks::openUndoFile(const CDiskBlockPos &pos, bool fReadOnly) {
    return OpenDiskFile(pos, "rev", fReadOnly);
}

boost::filesystem::path Blocks::getFilepathForIndex(int fileIndex, const char *prefix, bool fFindHarder)
{
    auto path = GetDataDir() / "blocks" / strprintf("%s%05u.dat", prefix, fileIndex);
    if (fFindHarder && !boost::filesystem::exists(path)) {
        std::shared_ptr<DBPrivate> d = Blocks::DB::instance()->priv();
        for (const std::string &dir : d->blocksDataDirs) {
            boost::filesystem::path alternatePath(dir);
            alternatePath = alternatePath / "blocks" / strprintf("%s%05u.dat", prefix, fileIndex);
            if (boost::filesystem::exists(alternatePath))
                return alternatePath;
        }
    }
    return path;
}

FastBlock Blocks::DB::loadBlock(CDiskBlockPos pos)
{
    return FastBlock(d->loadBlock(pos, ForwardBlock, 0));
}

FastUndoBlock Blocks::DB::loadUndoBlock(CDiskBlockPos pos, const uint256 &origBlockHash)
{
    return FastUndoBlock(d->loadBlock(pos, RevertBlock, &origBlockHash));
}

Streaming::ConstBuffer Blocks::DB::loadBlockFile(int fileIndex)
{
    size_t fileSize;
    auto buf = d->mapFile(fileIndex, ForwardBlock, &fileSize);
    if (buf.get() == nullptr)
        return Streaming::ConstBuffer(); // got pruned
    return Streaming::ConstBuffer(buf, buf.get(), buf.get() + fileSize - 1);
}

FastBlock Blocks::DB::writeBlock(const FastBlock &block, CDiskBlockPos &pos)
{
    assert(block.isFullBlock());
    return FastBlock(d->writeBlock(block.data(), pos, ForwardBlock, 0));
}

FastUndoBlock Blocks::DB::writeUndoBlock(const FastUndoBlock &block, const uint256 &blockHash, int fileIndex, uint32_t *posInFile)
{
    assert(block.size() > 0);
    CDiskBlockPos pos(fileIndex, 0);
    FastUndoBlock answer(d->writeBlock(block.data(), pos, RevertBlock, &blockHash));
    if (posInFile)
        *posInFile = pos.nPos;
    return answer;
}

bool Blocks::DB::appendHeader(CBlockIndex *block)
{
    assert(block);
    assert(block->phashBlock);
    bool found = false;
    const bool valid = (block->nStatus & BLOCK_FAILED_MASK) == 0;
    assert(valid || block->pprev);  // can't mark the genesis as invalid.
    if (valid && d->headersChain.Contains(block)) // nothing to do.
        return false;
    CBlockIndex *validPrev = valid ? block : block->pprev;
    while (validPrev->nStatus & BLOCK_FAILED_MASK) {
        validPrev = validPrev->pprev;
    }
    for (auto i = d->headerChainTips.begin(); i != d->headerChainTips.end(); ++i) {
        CBlockIndex *tip = *i;
        CBlockIndex *parent = block->GetAncestor(tip->nHeight);
        if (parent == tip) {
            d->headerChainTips.erase(i);
            d->headerChainTips.push_back(validPrev);
            if (tip == d->headersChain.Tip()) {
                d->headersChain.SetTip(validPrev);
                pindexBestHeader = validPrev;
                return true;
            }
            found = true;
            break;
        }
    }

    bool modifyingMainChain = false;
    if (!found) {
        bool modified = false;
        bool alreadyContains = false; // true if a second chain already contains our new validPrev
        auto i = d->headerChainTips.begin();
        while (i != d->headerChainTips.end()) {
            if ((*i)->GetAncestor(block->nHeight) == block) { // known in this chain.
                if (valid)
                    return false;
                modified = true;
                const bool mainChain = d->headersChain.Contains(*i);
                // it is invalid, remove it (and all children).
                i = d->headerChainTips.erase(i);
                if (mainChain)
                    d->headersChain.SetTip(validPrev);
                modifyingMainChain |= mainChain;
            } else {
                if ((*i)->GetAncestor(validPrev->nHeight) == validPrev) {
                    // the new best argument is already present on another chain, this means
                    // an entire chain will end up being removed. Lets check if we need to
                    // switch main-chain.
                    alreadyContains = true;
                    if (validPrev->nChainWork < (*i)->nChainWork)
                        validPrev = *i;
                }
                ++i;
            }
        }
        if (modified && !alreadyContains) // at least one chain was removed, then add back the correct tip
            d->headerChainTips.push_back(validPrev);
        if (valid) {
            d->headerChainTips.push_back(block);
            if (d->headersChain.Height() == -1) { // add genesis
                d->headersChain.SetTip(block);
                pindexBestHeader = block;
                return true;
            }
        }
    }
    assert(d->headersChain.Tip());
    assert(validPrev);
    if (d->headersChain.Tip()->nChainWork < validPrev->nChainWork) {
        // we changed what is to be considered the main-chain. Update the CChain instance.
        d->headersChain.SetTip(validPrev);
        pindexBestHeader = block;
        modifyingMainChain = true;
    }
    return modifyingMainChain;
}

bool Blocks::DB::appendBlock(CBlockIndex *block, int lastBlockFile)
{
    std::vector<std::pair<int, const CBlockFileInfo*> > files;
    std::vector<const CBlockIndex*> blocks;
    blocks.push_back(block);
    return WriteBatchSync(files, lastBlockFile, blocks);
}

const CChain &Blocks::DB::headerChain()
{
    return d->headersChain;
}

const std::list<CBlockIndex *> &Blocks::DB::headerChainTips()
{
    return d->headerChainTips;
}

void Blocks::DB::loadConfig()
{
    d->blocksDataDirs.clear();

    for (auto dir : mapMultiArgs["-blockdatadir"]) {
        if (boost::filesystem::is_directory(boost::filesystem::path(dir) / "blocks")) {
            d->blocksDataDirs.push_back(dir);
        } else {
            logCritical(4000) << "invalid blockdatadir passed. No 'blocks' subdir found, skipping:"<< dir;
        }
    }
}

///////////////////////////////////////////////

bool Blocks::Index::empty()
{
    auto priv = Blocks::DB::instance()->priv();
    std::lock_guard<std::mutex> lock_(priv->blockIndexLock);
    return priv->indexMap.empty();
}

const uint256 *Blocks::Index::insert(const uint256 &hash, CBlockIndex *index)
{
    assert(index);
    auto priv = Blocks::DB::instance()->priv();
    std::lock_guard<std::mutex> lock_(priv->blockIndexLock);
    auto iterator = priv->indexMap.insert(std::make_pair(hash, index)).first;
    return &((*iterator).first);
}

bool Blocks::Index::exists(const uint256 &hash)
{
    auto priv = Blocks::DB::instance()->priv();
    std::lock_guard<std::mutex> lock_(priv->blockIndexLock);
    auto mi = priv->indexMap.find(hash);
    return mi != priv->indexMap.end();
}

CBlockIndex *Blocks::Index::get(const uint256 &hash)
{
    auto priv = Blocks::DB::instance()->priv();
    std::lock_guard<std::mutex> lock_(priv->blockIndexLock);
    auto mi = priv->indexMap.find(hash);
    if (mi == priv->indexMap.end())
        return nullptr;
    return mi->second;
}

int Blocks::Index::size()
{
    auto priv = Blocks::DB::instance()->priv();
    std::lock_guard<std::mutex> lock_(priv->blockIndexLock);
    return priv->indexMap.size();
}

bool Blocks::Index::reconsiderBlock(CBlockIndex *pindex) {
    auto priv = Blocks::DB::instance()->priv();
    std::lock_guard<std::mutex> lock_(priv->blockIndexLock);

    int nHeight = pindex->nHeight;

    // Remove the invalidity flag from this block and all its descendants.
    auto it = priv->indexMap.begin();
    while (it != priv->indexMap.end()) {
        if (!it->second->IsValid() && it->second->GetAncestor(nHeight) == pindex) {
            it->second->nStatus &= ~BLOCK_FAILED_MASK;
            MarkIndexUnsaved(it->second);
        }
        it++;
    }

    // Remove the invalidity flag from all ancestors too.
    while (pindex != NULL) {
        if (pindex->nStatus & BLOCK_FAILED_MASK) {
            pindex->nStatus &= ~BLOCK_FAILED_MASK;
            MarkIndexUnsaved(pindex);
        }
        pindex = pindex->pprev;
    }
    return true;
}

std::set<int> Blocks::Index::fileIndexes()
{
    auto priv = Blocks::DB::instance()->priv();
    std::lock_guard<std::mutex> lock_(priv->blockIndexLock);

    std::set<int> setBlkDataFiles;
    BOOST_FOREACH(const PAIRTYPE(uint256, CBlockIndex*)& item, priv->indexMap) {
        CBlockIndex* pindex = item.second;
        if (pindex->nStatus & BLOCK_HAVE_DATA) {
            setBlkDataFiles.insert(pindex->nFile);
        }
    }
    return setBlkDataFiles;
}

void Blocks::Index::unload()
{
    auto instance = Blocks::DB::instance();
    if (instance == nullptr)
        return;
    instance->priv()->unloadIndexMap();
}

std::vector<std::pair<int, CBlockIndex *> > Blocks::Index::allByHeight()
{
    auto priv = Blocks::DB::instance()->priv();
    std::lock_guard<std::mutex> lock_(priv->blockIndexLock);

    std::vector<std::pair<int, CBlockIndex*> > vSortedByHeight;
    vSortedByHeight.reserve(priv->indexMap.size());
    BOOST_FOREACH(const PAIRTYPE(uint256, CBlockIndex*)& item, priv->indexMap)
    {
        CBlockIndex* pindex = item.second;
        vSortedByHeight.push_back(std::make_pair(pindex->nHeight, pindex));
    }
    std::sort(vSortedByHeight.begin(), vSortedByHeight.end());
    return vSortedByHeight;
}


////////////////////////////////

Blocks::DBPrivate::DBPrivate()
{
}

Blocks::DBPrivate::~DBPrivate()
{
    unloadIndexMap();
    // this class is mostly lock-free, which means that this destructor can be called well before
    // all the users of the datafiles are deleted.
    //  The design is that the objects stored in the datafils/revertDatafiles will be deleted when the last
    // users stop using them. So don't delete them here, it would cause issues.
    std::lock_guard<std::recursive_mutex> lock_(lock);
    datafiles.clear();
    revertDatafiles.clear();
    fileHistory.clear();
}

Streaming::ConstBuffer Blocks::DBPrivate::loadBlock(CDiskBlockPos pos, BlockType type, const uint256 *blockHash)
{
    if (pos.nPos < 4)
        throw std::runtime_error("Blocks::loadBlock got Database corruption");
    size_t fileSize;
    auto buf = mapFile(pos.nFile, type, &fileSize);
    if (buf.get() == nullptr)
        throw std::runtime_error("Failed to memmap block");
    if (pos.nPos >= fileSize)
        throw std::runtime_error("position outside of file");
    uint32_t blockSize = le32toh(*((uint32_t*)(buf.get() + pos.nPos - 4)));
    if (pos.nPos + blockSize + (blockHash ? 32 : 0) > fileSize)
        throw std::runtime_error("block sized bigger than file");
    if (blockHash) {
        assert(type == RevertBlock);
        // Verify checksum
        CHashWriter hasher(SER_GETHASH, PROTOCOL_VERSION);
        hasher << *blockHash;
        hasher.write(buf.get() + pos.nPos, blockSize);
        uint256 hashChecksum(buf.get() + pos.nPos + blockSize);

        if (hashChecksum != hasher.GetHash())
            throw std::runtime_error("BlocksDB::loadUndoBlock, checkum mismatch");
    }
    return Streaming::ConstBuffer(buf, buf.get() + pos.nPos, buf.get() + pos.nPos + blockSize);
}

Streaming::ConstBuffer Blocks::DBPrivate::writeBlock(const Streaming::ConstBuffer &block, CDiskBlockPos &pos, BlockType type, const uint256 *blockHash)
{
    const int blockSize = block.size();
    assert(blockSize < (int) MAX_BLOCKFILE_SIZE - 8);
    LOCK(cs_LastBlockFile);

    bool newFile = false;
    const bool useBlk = type == ForwardBlock;
    if ((int) vinfoBlockFile.size() <= nLastBlockFile) { // first file.
        newFile = true;
        vinfoBlockFile.resize(nLastBlockFile + 1);
    } else if (useBlk && vinfoBlockFile[nLastBlockFile].nSize + blockSize + 8 > MAX_BLOCKFILE_SIZE) {
        // previous file full.
        newFile = true;
        vinfoBlockFile.resize(++nLastBlockFile + 1);
    } else if (!useBlk && nLastBlockFile < pos.nFile) { // Want new revert file to be created
        // We can get our nLastBlockFile out of sync in a resync where the revert files are written
        // without there having been blk files written first.
        newFile = true;
        nLastBlockFile = std::max(nLastBlockFile + 1, pos.nFile);
        vinfoBlockFile.resize(nLastBlockFile + 1);
    }
    if (useBlk) // revert files get to tell us which file they want to be in
        pos.nFile = nLastBlockFile;
    assert(pos.nFile <= nLastBlockFile);
    assert((int) vinfoBlockFile.size() > pos.nFile);
    CBlockFileInfo &info = vinfoBlockFile[pos.nFile];
    if (newFile || (!useBlk && info.nUndoSize == 0)) { // create new file on disk
        const auto path = getFilepathForIndex(pos.nFile, useBlk ? "blk" : "rev");
        logDebug(Log::DB) << "Starting new file" << path.string();
        std::lock_guard<std::recursive_mutex> lock_(lock);
        size_t newFileSize = std::max(blockSize + 8, (int) (useBlk ? BLOCKFILE_CHUNK_SIZE : UNDOFILE_CHUNK_SIZE));
#ifdef WIN32
        // due to the fact that on Windows we can't re-map, we skip the growing steps.
        newFileSize = MAX_BLOCKFILE_SIZE;
#endif
        boost::filesystem::ofstream file(path);
        file.close();
        boost::filesystem::resize_file(path, newFileSize);
    }
    size_t fileSize;
    bool writable;
    auto buf = mapFile(pos.nFile, type, &fileSize, &writable);
    if (buf.get() == nullptr) {
        logFatal(Log::DB).nospace() << "Wanting to write to DB file " << (newFile ? "(new)":"") << "blk0..." << pos.nFile << ".dat failed, could not open";
        throw std::runtime_error("Failed to open file");
    }
    if (!writable) {
        logFatal(Log::DB).nospace() << "Wanting to write to DB file blk0..." << pos.nFile << ".dat failed, file read-only";
        throw std::runtime_error("File is not writable");
    }
    uint32_t *posInFile = useBlk ? &info.nSize : &info.nUndoSize;
#ifndef WIN32
    while (*posInFile + blockSize + 8 + (useBlk ? 0 : 32) >= fileSize) {
        const auto path = getFilepathForIndex(pos.nFile, useBlk ? "blk" : "rev");
        logDebug(Log::DB) << "File" << path.string() << "needs to be resized";
        const size_t newFileSize = fileSize + (useBlk ? BLOCKFILE_CHUNK_SIZE : UNDOFILE_CHUNK_SIZE);
        { // scope the lock
            std::lock_guard<std::recursive_mutex> lock_(lock);
            useBlk ? fileHasGrown(pos.nFile) : revertFileHasGrown(pos.nFile);
            boost::filesystem::resize_file(path, newFileSize);
        }
        buf = mapFile(pos.nFile, type, &fileSize, &writable);
        if (buf.get() == nullptr) {
            logFatal(Log::DB) << "Failed resize";
            throw std::runtime_error("Failed to open resized file");
        }
        if (!writable) {
            logFatal(Log::DB) << "Resized file no longer readable";
            throw std::runtime_error("Resized file no longer readable");
        }
    }
#endif
    pos.nPos = *posInFile + 8;
    char *data = buf.get() + *posInFile;
    memcpy(data, Params().MessageStart(), 4);
    data += 4;
    uint32_t networkSize = htole32(blockSize);
    memcpy(data, &networkSize, 4);
    data += 4;
    memcpy(data, block.begin(), blockSize);
    if (type == ForwardBlock) {
        info.AddBlock();
    } else {
        assert(type == RevertBlock);
        assert(blockHash);
        // calculate & write checksum
        CHashWriter hasher(SER_GETHASH, PROTOCOL_VERSION);
        hasher << *blockHash;
        hasher.write(block.begin(), block.size());
        uint256 hash = hasher.GetHash();
        memcpy(data + blockSize, hash.begin(), 256 / 8);
        *posInFile += 32;
    }
    *posInFile += blockSize + 8;
    setDirtyFileInfo.insert(pos.nFile);
    return Streaming::ConstBuffer(buf, data, data + blockSize);
}

void Blocks::DBPrivate::unloadIndexMap()
{
    std::lock_guard<std::mutex> lock_(blockIndexLock);

    for (auto entry : indexMap) {
        delete entry.second;
    }
    indexMap.clear();
}

void Blocks::DBPrivate::foundBlockFile(int index, const CBlockFileInfo &info)
{
    LOCK(cs_LastBlockFile);
    if (nLastBlockFile < index)
        nLastBlockFile = index;
    if ((int) vinfoBlockFile.size() <= nLastBlockFile)
        vinfoBlockFile.resize(nLastBlockFile + 1);
    // copy all but the undosize since that may have been assigned already.
    vinfoBlockFile[index].nBlocks = info.nBlocks;
    vinfoBlockFile[index].nSize = info.nSize;
    logCritical(Log::DB) << "Registring block file info" << index << info.nBlocks << "blocks with a total of" << info.nSize << "bytes";
}

std::shared_ptr<char> Blocks::DBPrivate::mapFile(int fileIndex, Blocks::BlockType type, size_t *size_out, bool *isWritable)
{
    const bool useBlk = type == ForwardBlock;
    std::vector<DataFile*> &list = useBlk ? datafiles : revertDatafiles;
    const char *prefix = useBlk ? "blk" : "rev";

    std::lock_guard<std::recursive_mutex> lock_(lock);
    if ((int) list.size() <= fileIndex)
        list.resize(fileIndex + 10);
    DataFile *df = list.at(fileIndex);
    if (df == nullptr) {
        df = new DataFile();
        list[fileIndex] = df;
    }
    std::shared_ptr<char> buf = df->buffer.lock();
    if (buf.get() == nullptr) {
        auto path = getFilepathForIndex(fileIndex, prefix, true);
        const auto mode = std::ios_base::binary | std::ios_base::in;
        const auto modeRW = mode | std::ios_base::out;
        try { // auto open read-write when last block, or any revert file.
            df->file.open(path, (useBlk && fileIndex != nLastBlockFile) ? mode : modeRW);
        } catch (...) {
            // try to open again, read-only now.
            // the user may have moved the files to a read-only medium.
            try { df->file.open(path, mode); } catch (...) {} // avoid throwing here.
        }
        if (df->file.is_open()) {
            auto weakThis = std::weak_ptr<DBPrivate>(shared_from_this());
            auto cleanupLambda = [useBlk,fileIndex,df,weakThis] (char *buf) {
                std::shared_ptr<DBPrivate> d = weakThis.lock();
                if (d) {   // mutex scope...
                    std::lock_guard<std::recursive_mutex> lockG(d->lock);
                    std::vector<DataFile*> &list = useBlk ? d->datafiles : d->revertDatafiles;
                    assert(fileIndex >= 0 && fileIndex < (int) list.size());
                    if (df == list.at(fileIndex)) {
                        // invalidate entry -- note that it's possible
                        // df != list[fileIndex] if we resized the file
                        list[fileIndex] = nullptr;
                    }
                }
                // no need to hold lock on delete -- auto-closes mmap'd file.
                delete df;
            };
            buf = std::shared_ptr<char>(const_cast<char*>(df->file.const_data()), cleanupLambda);
            df->buffer = std::weak_ptr<char>(buf);
            df->filesize = df->file.size();
        } else {
            logCritical(Log::DB) << "Blocks::DB: failed to memmap data-file" << path.string();
            list[fileIndex] = nullptr;
            delete df;
            if (size_out) *size_out = 0;
            return std::shared_ptr<char>();
        }
        // keep the last 10 used referenced to avoid closing and opening files all the time
        fileHistory.push_back(buf);
        if (fileHistory.size() > 10)
            fileHistory.erase(fileHistory.begin());
    }
    if (size_out) *size_out = df->filesize;
    if (isWritable)
        *isWritable = df->file.flags() == boost::iostreams::mapped_file::readwrite;
    return buf;
}

// we expect the mutex `lock` to be locked before calling this method
void Blocks::DBPrivate::fileHasGrown(int fileIndex)
{
    assert(fileIndex >= 0 && fileIndex < static_cast<int>(datafiles.size()));
    // unconditionally invalidate the pointer.
    // This doesn't leak memory because if ptr existed, there are
    // extant shard_ptr buffers.  When they get deleted, ptr will also.
    // (see cleanupLambda in mapFile() above)
    datafiles[fileIndex] = nullptr;
}

// we expect the mutex `lock` to be locked before calling this method
void Blocks::DBPrivate::revertFileHasGrown(int fileIndex)
{
    assert(fileIndex >= 0 && fileIndex < static_cast<int>(revertDatafiles.size()));
    // unconditionally invalidate the pointer.
    // This doesn't leak memory because if ptr existed, there are
    // extant shard_ptr buffers.  When they get deleted, ptr will also.
    // (see cleanupLambda in mapFile() above)
    revertDatafiles[fileIndex] = nullptr;
}
