// bt-migrate, torrent state migration tool
// Copyright (C) 2014 Mike Gelfand <mikedld@mikedld.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.

#include "uTorrentStateStore.h"

#include "BencodeCodec.h"
#include "Box.h"
#include "BoxHelper.h"
#include "Exception.h"
#include "IFileStreamProvider.h"
#include "IForwardIterator.h"
#include "Throw.h"
#include "Util.h"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <json/value.h>
#include <json/writer.h>

#include <iostream>

namespace fs = boost::filesystem;

namespace uTorrent
{

enum Priority
{
    DoNotDownloadPriority = 0,
    MinPriority = 4,
    MaxPriority = 12
};

enum TorrentState
{
    StoppedState = 0,
    StartedState = 2,
    PausedState = 3
};

std::string const ResumeFilename = "resume.dat";

} // namespace uTorrent

namespace
{

typedef std::unique_ptr<Json::Value> JsonValuePtr;

Box::LimitInfo FromStoreRatioLimit(Json::Value const& enabled, Json::Value const& storeLimit)
{
    Box::LimitInfo result;
    result.Mode = enabled.asInt() != 0 ? Box::LimitMode::Enabled : Box::LimitMode::Inherit;
    result.Value = storeLimit.asDouble() / 1000.;
    return result;
}

Box::LimitInfo FromStoreSpeedLimit(Json::Value const& storeLimit)
{
    Box::LimitInfo result;
    result.Mode = storeLimit.asInt() > 0 ? Box::LimitMode::Enabled : Box::LimitMode::Inherit;
    result.Value = storeLimit.asInt();
    return result;
}

class uTorrentTorrentStateIterator : public ITorrentStateIterator
{
public:
    uTorrentTorrentStateIterator(fs::path const& configDir, JsonValuePtr resume, IFileStreamProvider& fileStreamProvider);

public:
    // ITorrentStateIterator
    virtual bool GetNext(Box& nextBox);

private:
    fs::path const m_configDir;
    JsonValuePtr const m_resume;
    IFileStreamProvider& m_fileStreamProvider;
    Json::Value::iterator m_torrentIt;
    Json::Value::iterator const m_torrentEnd;
    BencodeCodec const m_bencoder;
};

uTorrentTorrentStateIterator::uTorrentTorrentStateIterator(fs::path const& configDir, JsonValuePtr resume,
    IFileStreamProvider& fileStreamProvider) :
    m_configDir(configDir),
    m_resume(std::move(resume)),
    m_fileStreamProvider(fileStreamProvider),
    m_torrentIt(m_resume->begin()),
    m_torrentEnd(m_resume->end()),
    m_bencoder()
{
    //
}

bool uTorrentTorrentStateIterator::GetNext(Box& nextBox)
{
    std::string torrentFilename;
    while (m_torrentIt != m_torrentEnd)
    {
        static std::string const TorrentFileException = ".torrent";

        std::string key = m_torrentIt.key().asString();
        if (key.size() > TorrentFileException.size() &&
            key.compare(key.size() - TorrentFileException.size(), TorrentFileException.size(), TorrentFileException) == 0)
        {
            torrentFilename = std::move(key);
            break;
        }

        ++m_torrentIt;
    }

    if (m_torrentIt == m_torrentEnd)
    {
        return false;
    }

    Json::Value const& resume = *m_torrentIt;

    Box box;

    {
        ReadStreamPtr const stream = m_fileStreamProvider.GetReadStream(m_configDir / torrentFilename);
        BoxHelper::LoadTorrent(*stream, box);
    }

    box.AddedAt = resume["added_on"].asInt();
    box.CompletedAt = resume["completed_on"].asInt();
    box.IsPaused = resume["started"].asInt() == uTorrent::PausedState || resume["started"].asInt() == uTorrent::StoppedState;
    box.DownloadedSize = resume["downloaded"].asUInt64();
    box.UploadedSize = resume["uploaded"].asUInt64();
    box.CorruptedSize = resume["corrupt"].asUInt64();
    box.SavePath = Util::GetPath(resume["path"].asString()).parent_path().string();
    box.BlockSize = resume["blocksize"].asUInt();
    box.RatioLimit = FromStoreRatioLimit(resume["override_seedsettings"], resume["wanted_ratio"]);
    box.DownloadSpeedLimit = FromStoreSpeedLimit(resume["downspeed"]);
    box.UploadSpeedLimit = FromStoreSpeedLimit(resume["upspeed"]);

    std::string const filePriorities = resume["prio"].asString();
    std::size_t const numberOfFiles = resume["modtimes"].size();
    for (Json::ArrayIndex i = 0; i < numberOfFiles; ++i)
    {
        int const filePriority = filePriorities[i];

        Box::FileInfo file;
        file.DoNotDownload = filePriority == uTorrent::DoNotDownloadPriority;
        file.Priority = file.DoNotDownload ? Box::NormalPriority : BoxHelper::Priority::FromStore(filePriority,
            uTorrent::MinPriority, uTorrent::MaxPriority);
        box.Files.push_back(std::move(file));
    }

    std::uint32_t const torrentPieceSize = box.Torrent["info"]["piece length"].asUInt64();
    // if (torrentPieceSize % box.BlockSize != 0)
    // {
    //     Throw<Exception>() << "Unsupported torrent piece size (" << torrentPieceSize << ")";
    // }

    std::string const pieces = resume["have"].asString();
    std::int32_t const blocksPerPiece = torrentPieceSize / box.BlockSize;
    box.ValidBlocks.reserve(pieces.size() * 8 * blocksPerPiece);
    for (unsigned char const c : pieces)
    {
        for (int i = 0; i < 8; ++i)
        {
            bool const isPieceValid = (c & (1 << i)) != 0;
            box.ValidBlocks.resize(box.ValidBlocks.size() + blocksPerPiece, isPieceValid);
        }
    }

    std::uint64_t const totalSize = Util::GetTotalTorrentSize(box.Torrent);
    std::uint32_t const totalBlockCount = (totalSize + box.BlockSize - 1) / box.BlockSize;
    if (box.ValidBlocks.size() < totalBlockCount)
    {
        Throw<Exception>() << "Unable to export valid pieces: " << box.ValidBlocks.size() << " vs. " << totalBlockCount;
    }

    box.ValidBlocks.resize(totalBlockCount);

    nextBox = std::move(box);
    ++m_torrentIt;
    return true;
}

} // namespace

uTorrentStateStore::uTorrentStateStore()
{
    //
}

uTorrentStateStore::~uTorrentStateStore()
{
    //
}

TorrentClient::Enum uTorrentStateStore::GetTorrentClient() const
{
    return TorrentClient::uTorrent;
}

fs::path uTorrentStateStore::GuessConfigDir() const
{
    throw NotImplementedException(__func__);
}

bool uTorrentStateStore::IsValidConfigDir(fs::path const& configDir) const
{
    boost::system::error_code dummy;
    return
        fs::is_regular_file(configDir / uTorrent::ResumeFilename, dummy);
}

ITorrentStateIteratorPtr uTorrentStateStore::Export(fs::path const& configDir, IFileStreamProvider& fileStreamProvider) const
{
    if (!IsValidConfigDir(configDir))
    {
        Throw<Exception>() << "Bad uTorrent configuration directory: " << configDir;
    }

    JsonValuePtr resume(new Json::Value());
    {
        ReadStreamPtr const stream = fileStreamProvider.GetReadStream(configDir / uTorrent::ResumeFilename);
        BencodeCodec().Decode(*stream, *resume);
    }

    return ITorrentStateIteratorPtr(new uTorrentTorrentStateIterator(configDir, std::move(resume), fileStreamProvider));
}

void uTorrentStateStore::Import(fs::path const& configDir, ITorrentStateIteratorPtr /*boxes*/,
    IFileStreamProvider& /*fileStreamProvider*/) const
{
    if (!IsValidConfigDir(configDir))
    {
        Throw<Exception>() << "Bad uTorrent configuration directory: " << configDir;
    }

    throw NotImplementedException(__func__);
}