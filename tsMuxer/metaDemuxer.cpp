#include "metaDemuxer.h"

#include <fs/directory.h>
#include <fs/systemlog.h>

#include <fs/textfile.h>
#include <types/types.h>
#include <climits>

#include "aacStreamReader.h"
#include "ac3StreamReader.h"
#include "bufferedReaderManager.h"
#include "combinedH264Demuxer.h"
#include "dtsStreamReader.h"
#include "dvbSubStreamReader.h"
#include "h264StreamReader.h"
#include "hevcStreamReader.h"
#include "lpcmStreamReader.h"
#include "matroskaDemuxer.h"
#include "mlpStreamReader.h"
#include "movDemuxer.h"
#include "mpeg2StreamReader.h"
#include "mpegAudioStreamReader.h"
#include "mpegStreamReader.h"
#include "pgsStreamReader.h"
#include "programStreamDemuxer.h"
#include "srtStreamReader.h"
#include "subTrackFilter.h"
#include "tsDemuxer.h"
#include "vc1StreamReader.h"
#include "vodCoreException.h"
#include "vod_common.h"
#include "vvcStreamReader.h"

using namespace std;

static constexpr int MAX_DEMUX_BUFFER_SIZE = 1024 * 1024 * 192;
static constexpr int MIN_READED_BLOCK = 16384;

METADemuxer::METADemuxer(const BufferedReaderManager& readManager)
    : m_containerReader(*this, readManager), m_readManager(readManager)
{
    m_flushDataMode = false;
    m_HevcFound = false;
    m_totalSize = 0;
    m_lastProgressY = 0;
    m_lastReadRez = 0;
}

METADemuxer::~METADemuxer()
{
    readClose();
    for (FileListIterator*& m_iterator : m_iterators) delete m_iterator;
}

int64_t METADemuxer::getDemuxedSize()
{
    int64_t rez = 0;
    for (const StreamInfo& si : m_codecInfo)
        rez += si.m_streamReader->getProcessedSize();  // m_codecInfo[i].m_dataProcessed;
    return rez + m_containerReader.getDiscardedSize();
}

int METADemuxer::readPacket(AVPacket& avPacket)

{
    avPacket.stream_index = 0;
    avPacket.data = nullptr;
    avPacket.size = 0;
    avPacket.codec = nullptr;
    m_lastReadRez = 0;
    while (true)
    {
        int minDtsIndex = -1;
        int64_t minDts = LLONG_MAX;
        bool allDataDelayed = true;
        while (allDataDelayed)
        {
            for (int i = 0; i < static_cast<int>(m_codecInfo.size()); i++)
            {
                StreamInfo& streamInfo = m_codecInfo[i];
                if (!m_flushDataMode)
                {
                    streamInfo.lastReadRez = streamInfo.read();
                    if (streamInfo.lastReadRez == BufferedFileReader::DATA_DELAYED)
                        continue;  // skip stream
                    allDataDelayed = false;
                    if (streamInfo.lastReadRez == BufferedFileReader::DATA_NOT_READY)
                    {
                        m_lastReadRez = BufferedFileReader::DATA_NOT_READY;
                        return BufferedFileReader::DATA_NOT_READY;
                    }
                    if (streamInfo.lastReadRez != BufferedFileReader::DATA_EOF2)
                    {
                        if (streamInfo.m_lastDTS < minDts)
                        {
                            minDtsIndex = i;
                            minDts = streamInfo.m_lastDTS;
                        }
                    }
                    else if (streamInfo.m_lastDTS < minDts && !streamInfo.m_flushed)
                    {
                        minDtsIndex = i;
                        minDts = streamInfo.m_lastDTS;
                    }
                }
                else
                {
                    allDataDelayed = false;
                    if (streamInfo.m_lastDTS < minDts && !streamInfo.m_flushed)
                    {
                        minDtsIndex = i;
                        minDts = streamInfo.m_lastDTS;
                    }
                }
            }
            if (allDataDelayed)
                for (const StreamInfo& si : m_codecInfo)
                {
                    const auto cReader = dynamic_cast<ContainerToReaderWrapper*>(si.m_dataReader);
                    if (cReader)
                        cReader->resetDelayedMark();
                }
        }
        if (minDtsIndex != -1)
        {
            if (!m_flushDataMode)
            {
                if (m_codecInfo[minDtsIndex].lastReadRez != BufferedFileReader::DATA_EOF2)
                {
                    const int res = m_codecInfo[minDtsIndex].m_streamReader->readPacket(avPacket);
                    m_codecInfo[minDtsIndex].m_lastAVRez = res;
                }
                else
                {
                    // flush single stream
                    m_codecInfo[minDtsIndex].m_streamReader->flushPacket(avPacket);
                    m_codecInfo[minDtsIndex].m_flushed = true;
                }
                // add time shift from external sync source
                // add static time shift
                avPacket.dts += m_codecInfo[minDtsIndex].m_timeShift;
                avPacket.pts += m_codecInfo[minDtsIndex].m_timeShift;
                m_codecInfo[minDtsIndex].m_lastDTS = avPacket.dts + avPacket.duration;
            }
            else
            {  // flush all streams
                m_codecInfo[minDtsIndex].m_streamReader->flushPacket(avPacket);
                m_codecInfo[minDtsIndex].m_flushed = true;
            }
            updateReport(true);
            return 0;
        }
        if (!m_flushDataMode)
            m_flushDataMode = true;
        else
        {
            updateReport(false);
            m_lastReadRez = BufferedFileReader::DATA_EOF;
            return BufferedReader::DATA_EOF;
        }
    }
}

void METADemuxer::openFile(const string& streamName)
{
    m_streamName = streamName;
    readClose();

    TextFile file(m_streamName.c_str(), File::ofRead);
    string str;
    file.readLine(str);
    while (str.length() > 0)
    {
        str = trimStr(str);
        if (str.length() == 0 || str[0] == '#')
        {
            file.readLine(str);
            continue;
        }
        if (strStartWith(str, "MUXOPT"))
        {
            file.readLine(str);
            continue;
        }
        vector<string> params = splitQuotedStr(str.c_str(), ',');
        if (params.size() < 2)
            THROW(ERR_INVALID_CODEC_FORMAT, "Invalid codec format: " << str)
        map<string, string> addParams;
        for (unsigned i = 2; i < params.size(); i++)
        {
            // params[i] = strToLowerCase ( params[i] );
            vector<string> tmp = splitStr(params[i].c_str(), '=');
            addParams[trimStr(tmp[0])] = trimStr(tmp.size() > 1 ? tmp[1] : "");
        }
        string codec = trimStr(params[0]);
        string codecStreamName = trimStr(params[1]);
        codec = strToUpperCase(codec);
        if (!m_HevcFound)
            m_HevcFound = (codec.find("HEVC") == 12);
        addStream(codec, codecStreamName, addParams);
        file.readLine(str);
    }

    auto primarySEI = H264StreamReader::SeiMethod::SEI_NotDefined;
    for (const auto& si : m_codecInfo)
    {
        const auto reader = dynamic_cast<H264StreamReader*>(si.m_streamReader);
        if (reader && !reader->isSubStream())
        {
            primarySEI = reader->getInsertSEI();
            break;
        }
    }

    if (primarySEI != H264StreamReader::SeiMethod::SEI_NotDefined)
    {
        bool warned = false;
        for (const auto& i : m_codecInfo)
        {
            const auto reader = dynamic_cast<H264StreamReader*>(i.m_streamReader);
            if (reader && reader->isSubStream())
            {
                if (!warned && reader->getInsertSEI() != primarySEI)
                {
                    LTRACE(LT_INFO, 2,
                           "Parameter 'insertSEI' for MVC dependent view differs from same param for AVC base view. "
                           "Ignore and apply same value");
                    warned = true;
                }
                reader->setInsertSEI(primarySEI);
            }
        }
    }
}

std::string METADemuxer::mplsTrackToFullName(const std::string& mplsFileName, const std::string& mplsNum)
{
    string path = toNativeSeparators(extractFilePath(mplsFileName));
    const size_t tmp = path.find_last_of(getDirSeparator());
    if (tmp == string::npos)
        return {};
    path = path.substr(0, tmp + 1) + string("STREAM") + getDirSeparator();

    const string mplsExt = strToLowerCase(extractFileExt(mplsFileName));
    const string m2tsExt = (mplsExt == "mpls") ? "m2ts" : "mts";

    return path + mplsNum + string(".") + m2tsExt;
}

std::string METADemuxer::mplsTrackToSSIFName(const std::string& mplsFileName, const std::string& mplsNum)
{
    string path = toNativeSeparators(extractFilePath(mplsFileName));
    const size_t tmp = path.find_last_of(getDirSeparator());
    if (tmp == string::npos)
        return {};
    path = path.substr(0, tmp + 1) + string("STREAM") + getDirSeparator() + string("SSIF") + getDirSeparator();

    const string mplsExt = strToLowerCase(extractFileExt(mplsFileName));
    const string ssifExt = mplsExt == "mpls" ? "ssif" : "sif";

    return path + mplsNum + string(".") + ssifExt;
}

int METADemuxer::addPGSubStream(const string& codec, const string& _codecStreamName,
                                const map<string, string>& addParams, const MPLSStreamInfo* subStream)
{
    map<string, string> params = addParams;
    params["track"] = int32ToStr(subStream->streamPID);
    if (subStream->type == 2)
        params["subClip"] = "1";
    return addStream(codec, _codecStreamName, params);
}

std::vector<MPLSPlayItem> METADemuxer::mergePlayItems(const std::vector<MPLSParser>& mplsInfoList)
{
    std::vector<MPLSPlayItem> result;
    for (auto& i : mplsInfoList)
    {
        for (auto& j : i.m_playItems) result.push_back(j);
    }
    return result;
}

int METADemuxer::addStream(const string& codec, const string& codecStreamName, const map<string, string>& addParams)
{
    int32_t pid = 0;
    auto tmpitr = addParams.find("track");
    if (tmpitr != addParams.end())
        pid = strToInt32(tmpitr->second.c_str());

    tmpitr = addParams.find("subTrack");
    if (tmpitr != addParams.end())
        pid = SubTrackFilter::pidToSubPid(pid, strToInt32(tmpitr->second));

    vector<string> fileList;
    string unquotedStreamName = unquoteStr(codecStreamName);
    string fileExt = strToLowerCase(extractFileExt(codecStreamName));
    std::vector<MPLSParser> mplsInfoList;

    // filds for SS PG
    int leftEyeSubStreamIdx = -1;
    int rightEyeSubStreamIdx = -1;
    bool isSSPG = false;
    int ssPGOffset = 0xff;
    ////

    bool isSubStream = (codec == h264DepCodecInfo.programName) || addParams.find("subClip") != addParams.end();
    if (fileExt == "mpls" || fileExt == "mpl")
    {
        mplsInfoList = getMplsInfo(codecStreamName);
        vector<string> mplsNames = splitQuotedStr(codecStreamName.c_str(), '+');
        for (size_t k = 0; k < mplsInfoList.size(); ++k)
        {
            MPLSParser& mplsInfo = mplsInfoList[k];
            unquotedStreamName = unquoteStr(mplsNames[k]);
            for (size_t i = 0; i < mplsInfo.m_playItems.size(); ++i)
            {
                string playItemName;
                if (isSubStream)
                {
                    if (mplsInfo.m_mvcFiles.empty())
                    {
                        THROW(ERR_INVALID_CODEC_FORMAT,
                              "Current playlist file doesn't has MVC track info. Please, remove MVC track from the "
                              "track list")
                    }
                    if (mplsInfo.m_mvcFiles.size() <= i)
                        THROW(ERR_INVALID_CODEC_FORMAT,
                              "Bad playlist file: number of CLPI files for AVC and VMC parts do not match")
                    playItemName = mplsInfo.m_mvcFiles[i];
                }
                else
                {
                    playItemName = mplsInfo.m_playItems[i].fileName;
                }
                string fileName = mplsTrackToFullName(unquotedStreamName, playItemName);
                if (mplsInfo.isDependStreamExist && !fileExists(fileName))
                    fileName = mplsTrackToSSIFName(unquotedStreamName, mplsInfo.m_playItems[i].fileName);
                fileList.push_back(fileName);
            }

            MPLSStreamInfo streamInfo = mplsInfo.getStreamByPID(pid);
            if (streamInfo.stream_coding_type == StreamType::SUB_PGS && streamInfo.isSSPG)
            {
                // add refs to addition tracks for stereo subtitles
                leftEyeSubStreamIdx = addPGSubStream(codec, unquotedStreamName, addParams, streamInfo.leftEye);
                rightEyeSubStreamIdx = addPGSubStream(codec, unquotedStreamName, addParams, streamInfo.rightEye);

                isSSPG = streamInfo.isSSPG;
                ssPGOffset = streamInfo.SS_PG_offset_sequence_id;
            }
        }
    }
    else
    {
        fileList = extractFileList(codecStreamName);
    }

    for (int i = static_cast<int>(fileList.size()) - 1; i >= 0; --i)
    {
        string trackKey = fileList[i] + string("_#") + int32ToStr(i) + string("_");
        if (addParams.find("subClip") == addParams.end())
            trackKey += string("_track_");
        else
            trackKey += string("_subtrack_");
        trackKey += int32ToStr(pid);
        if (m_processedTracks.find(trackKey) != m_processedTracks.end())
            fileList.erase(fileList.begin() + i);
        else
            m_processedTracks.insert(trackKey);
    }
    if (fileList.empty())
        return -1;

    FileListIterator* listIterator = nullptr;
    if (fileList.size() > 1)
    {
        listIterator = new FileListIterator();
        m_iterators.push_back(listIterator);
        for (const string& fileName : fileList) listIterator->addFile(fileName);
    }

    int64_t fileSize = 0;

    if (m_containerReader.m_demuxers.find(fileList[0]) == m_containerReader.m_demuxers.end())
    {
        File tmpFile;
        for (const string& fileName : fileList)
        {
            if (!tmpFile.open(fileName.c_str(), File::ofRead))
                THROW(ERR_INVALID_CODEC_FORMAT, "Can't open file: " << fileName.c_str())
            int64_t tmpSize = 0;
            tmpFile.size(&tmpSize);
            fileSize += tmpSize;
            tmpFile.close();
        }
    }

    AbstractStreamReader* codecReader = createCodec(codec, addParams, fileList[0], mergePlayItems(mplsInfoList));
    codecReader->setStreamIndex(static_cast<int>(m_codecInfo.size() + 1));
    codecReader->setTimeOffset(m_timeOffset);

    if (m_codecInfo.empty())
        codecReader->m_flags |= AVPacket::PCR_STREAM;

    AbstractReader* dataReader;
    string tmpname = strToLowerCase(fileList[0]);
    tmpname = unquoteStr(trimStr(tmpname));

    if (strEndWith(tmpname, ".h264") || strEndWith(tmpname, ".264") || strEndWith(tmpname, ".mvc"))
    {
        if (pid)
        {
            dataReader = &m_containerReader;
            codecReader->setSrcContainerType(AbstractStreamReader::ContainerType::ctMultiH264);
            if (listIterator)
                dynamic_cast<ContainerToReaderWrapper*>(dataReader)->setFileIterator(fileList[0].c_str(), listIterator);
        }
        else
        {
            dataReader = m_readManager.getReader(fileList[0].c_str());
        }
    }
    else if (strEndWith(tmpname, ".ts") || strEndWith(tmpname, ".m2ts") || strEndWith(tmpname, ".mts") ||
             strEndWith(tmpname, ".ssif"))
    {
        if (pid)
            dataReader = &m_containerReader;
        else
            THROW(ERR_INVALID_CODEC_FORMAT, "For streams inside TS/M2TS container need track parameter.")
        if (listIterator)
            dynamic_cast<ContainerToReaderWrapper*>(dataReader)->setFileIterator(fileList[0].c_str(), listIterator);
        if (strEndWith(tmpname, ".ts"))
            codecReader->setSrcContainerType(AbstractStreamReader::ContainerType::ctTS);
        else
            codecReader->setSrcContainerType(AbstractStreamReader::ContainerType::ctM2TS);
    }
    else if (strEndWith(tmpname, ".vob") || strEndWith(tmpname, ".evo") || strEndWith(tmpname, ".mpg"))
    {
        if (pid)
            dataReader = &m_containerReader;
        else
            THROW(ERR_INVALID_CODEC_FORMAT, "For streams inside MPG/VOB/EVO container need track parameter.")
        if (listIterator)
            dynamic_cast<ContainerToReaderWrapper*>(dataReader)->setFileIterator(fileList[0].c_str(), listIterator);
        if (strEndWith(tmpname, ".evo") || strEndWith(tmpname, ".evo\""))
            codecReader->setSrcContainerType(AbstractStreamReader::ContainerType::ctEVOB);
        else
            codecReader->setSrcContainerType(AbstractStreamReader::ContainerType::ctVOB);
    }
    else if (strEndWith(tmpname, ".mkv") || strEndWith(tmpname, ".mka") || strEndWith(tmpname, ".mks"))
    {
        if (pid)
            dataReader = &m_containerReader;
        else
            THROW(ERR_INVALID_CODEC_FORMAT, "For streams inside MKV container need track parameter.")
        if (listIterator)
            dynamic_cast<ContainerToReaderWrapper*>(dataReader)->setFileIterator(fileList[0].c_str(), listIterator);
        codecReader->setSrcContainerType(AbstractStreamReader::ContainerType::ctMKV);
    }
    else if (strEndWith(tmpname, ".mov") || strEndWith(tmpname, ".mp4") || strEndWith(tmpname, ".m4v") ||
             strEndWith(tmpname, ".m4a"))
    {
        if (pid)
            dataReader = &m_containerReader;
        else
            THROW(ERR_INVALID_CODEC_FORMAT, "For streams inside MOV/MP4 container need track parameter.")
        if (listIterator)
            dynamic_cast<ContainerToReaderWrapper*>(dataReader)->setFileIterator(fileList[0].c_str(), listIterator);
        codecReader->setSrcContainerType(AbstractStreamReader::ContainerType::ctMOV);
    }
    else
    {
        dataReader = m_readManager.getReader(fileList[0].c_str());
    }
    if (dataReader == nullptr)
    {
        delete codecReader;
        THROW(ERR_INVALID_CODEC_FORMAT, "This version do not support multicast or other network steams for muxing")
    }

    m_codecInfo.emplace_back(dataReader, codecReader, fileList[0], codecStreamName, pid, isSubStream);
    if (listIterator)
    {
        auto fileReader = dynamic_cast<BufferedFileReader*>(dataReader);
        if (fileReader)
            fileReader->setFileIterator(listIterator, m_codecInfo.rbegin()->m_readerID);
    }

    StreamInfo& streamInfo = *m_codecInfo.rbegin();
    streamInfo.m_codec = codec;
    streamInfo.m_addParams = addParams;
    int streamIndex = codecReader->getStreamIndex();

    // fields for SS PG stream
    auto pgStream = dynamic_cast<PGSStreamReader*>(codecReader);
    if (pgStream)
    {
        pgStream->isSSPG = isSSPG;
        pgStream->ssPGOffset = ssPGOffset;
        pgStream->leftEyeSubStreamIdx = leftEyeSubStreamIdx;
        pgStream->rightEyeSubStreamIdx = rightEyeSubStreamIdx;
    }
    //

    auto itr = addParams.find("timeshift");
    if (itr != addParams.end())
    {
        string timeShift = itr->second;
        size_t pos = 0;
        int64_t coeff = 1;
        int64_t value = 0;
        if ((pos = timeShift.find("ms")) != std::string::npos)
        {
            coeff = 1000000;
            value = strToInt32(timeShift.substr(0, pos).c_str());
        }
        else if ((pos = timeShift.find('s')) != std::string::npos)
        {
            coeff = 1000000000;
            value = strToInt32(timeShift.substr(0, pos).c_str());
        }
        else if ((pos = timeShift.find("ns")) != std::string::npos)
        {
            coeff = 1;
            value = strToInt32(timeShift.substr(0, pos).c_str());
        }
        else
        {
            coeff = 1000000;
            value = strToInt32(timeShift.c_str());
        }
        value = value * coeff / 1000 * INTERNAL_PTS_FREQ / 1000000;
        streamInfo.m_timeShift = value;
        if (value > 0)
            streamInfo.m_lastDTS = value;
    }

    itr = addParams.find("lang");
    if (itr != addParams.end())
        streamInfo.m_lang = itr->second;
    m_totalSize += fileSize;

    return streamIndex;
}

void METADemuxer::readClose()
{
    for (const auto& codecInfo : m_codecInfo)
    {
        codecInfo.m_dataReader->deleteReader(codecInfo.m_readerID);
        delete codecInfo.m_streamReader;
    }
    m_codecInfo.clear();
}

DetectStreamRez METADemuxer::DetectStreamReader(const BufferedReaderManager& readManager, const string& fileName,
                                                bool calcDuration)
{
    AVChapters chapters;
    int64_t fileDuration = 0;
    vector<CheckStreamRez> streams, Vstreams;
    AbstractDemuxer* demuxer = nullptr;
    auto unquoted = unquoteStr(fileName);
    string fileExt = strToLowerCase(extractFileExt(unquoted));
    AbstractStreamReader::ContainerType containerType = AbstractStreamReader::ContainerType::ctNone;
    CLPIParser clpi;
    bool clpiParsed = false;
    if (fileExt == "m2ts" || fileExt == "mts" || fileExt == "ssif")
    {
        demuxer = new TSDemuxer(readManager, "");
        containerType = AbstractStreamReader::ContainerType::ctM2TS;
        string clpiFileName = findBluRayFile(extractFileDir(unquoted), "CLIPINF", extractFileName(unquoted) + ".clpi");
        if (!clpiFileName.empty())
            clpiParsed = clpi.parse(clpiFileName.c_str());
    }
    else if (fileExt == "ts")
    {
        demuxer = new TSDemuxer(readManager, "");
        containerType = AbstractStreamReader::ContainerType::ctTS;
    }
    else if (fileExt == "vob" || fileExt == "mpg")
    {
        demuxer = new ProgramStreamDemuxer(readManager);
        containerType = AbstractStreamReader::ContainerType::ctVOB;
    }
    else if (fileExt == "evo")
    {
        demuxer = new ProgramStreamDemuxer(readManager);
        containerType = AbstractStreamReader::ContainerType::ctEVOB;
    }
    else if (fileExt == "mkv" || fileExt == "mka" || fileExt == "mks")
    {
        demuxer = new MatroskaDemuxer(readManager);
        containerType = AbstractStreamReader::ContainerType::ctMKV;
    }
    else if (fileExt == "mp4" || fileExt == "m4v" || fileExt == "m4a" || fileExt == "mov")
    {
        demuxer = new MovDemuxer(readManager);
        containerType = AbstractStreamReader::ContainerType::ctMOV;
    }

    if (demuxer)
    {
        uint32_t fileBlockSize = demuxer->getFileBlockSize();

        demuxer->openFile(fileName);
        int64_t discardedSize = 0;
        DemuxedData demuxedData;
        map<int32_t, TrackInfo> acceptedPidMap;
        demuxer->getTrackList(acceptedPidMap);
        PIDSet acceptedPidSet;
        for (const auto& itr : acceptedPidMap) acceptedPidSet.insert(itr.first);
        for (auto& itr : demuxedData)
        {
            StreamData& vect = itr.second;
            vect.reserve(fileBlockSize);
        }

        for (unsigned i = 0; i < DETECT_STREAM_BUFFER_SIZE / fileBlockSize; i++)
            demuxer->simpleDemuxBlock(demuxedData, acceptedPidSet, discardedSize);

        for (auto& itr : demuxedData)
        {
            StreamData& vect = itr.second;
            CheckStreamRez trackRez = detectTrackReader(vect.data(), static_cast<int>(vect.size()), containerType,
                                                        acceptedPidMap[itr.first].m_trackType, itr.first);
            if (!trackRez.codecInfo.programName.empty())
            {
                if (trackRez.codecInfo.programName[0] != 'S')
                    trackRez.delay = demuxer->getTrackDelay(itr.first);
            }
            trackRez.trackID = itr.first;
            trackRez.lang = acceptedPidMap[trackRez.trackID].m_lang;
            if (clpiParsed)
            {
                map<int, CLPIStreamInfo>::const_iterator clpiStream = clpi.m_streamInfo.find(itr.first);
                if (clpiStream != clpi.m_streamInfo.end())
                    trackRez.lang = clpiStream->second.language_code;
            }
            // correct ISO 639-2/B codes to ISO 639-2/T
            static const std::string langB[24] = {
                "alb", "arm", "baq", "bur", "cze", "chi", "dut", "ger", "gre", "fre", "geo", "ice",
                "jaw", "mac", "mao", "may", "mol", "per", "rum", "scc", "scr", "slo", "tib", "wel",
            };
            static const std::string langT[24] = {
                "sqi", "hye", "eus", "mya", "ces", "zho", "nld", "deu", "ell", "fra", "kat", "isl",
                "jav", "mkd", "mri", "fas", "rom", "msa", "ron", "srp", "hrv", "slk", "bod", "cym",
            };
            for (int i = 0; i < 24; i++)
                if (trackRez.lang == langB[i])
                    trackRez.lang = langT[i];

            if (strStartWith(trackRez.codecInfo.programName, "A_") && dynamic_cast<TSDemuxer*>(demuxer))
            {
                if (trackRez.trackID >= 0x1A00)
                    trackRez.isSecondary = true;
            }

            if (strStartWith(trackRez.codecInfo.programName, "V_"))
                addTrack(Vstreams, trackRez);
            else
                addTrack(streams, trackRez);
        }
        chapters = demuxer->getChapters();
        if (calcDuration)
            fileDuration = demuxer->getFileDurationNano();
        delete demuxer;
    }
    else
    {
        File file;
        containerType = AbstractStreamReader::ContainerType::ctNone;
        if (!file.open(fileName.c_str(), File::ofRead))
            return {};
        auto tmpBuffer = new uint8_t[DETECT_STREAM_BUFFER_SIZE];
        int len = file.read(tmpBuffer, DETECT_STREAM_BUFFER_SIZE);
        if (fileExt == "sup")
            containerType = AbstractStreamReader::ContainerType::ctSUP;
        else if (fileExt == "pcm" || fileExt == "lpcm" || fileExt == "wav" || fileExt == "w64")
            containerType = AbstractStreamReader::ContainerType::ctLPCM;
        else if (fileExt == "srt")
            containerType = AbstractStreamReader::ContainerType::ctSRT;
        CheckStreamRez trackRez = detectTrackReader(tmpBuffer, len, containerType, 0, 0);

        if (strStartWith(trackRez.codecInfo.programName, "V_"))
            addTrack(Vstreams, trackRez);
        else
            addTrack(streams, trackRez);

        delete[] tmpBuffer;
    }
    Vstreams.insert(Vstreams.end(), streams.begin(), streams.end());

    DetectStreamRez rez;
    rez.chapters = chapters;
    rez.fileDurationNano = fileDuration;
    rez.streams = Vstreams;
    return rez;
}

void METADemuxer::addTrack(vector<CheckStreamRez>& rez, CheckStreamRez trackRez)
{
    if (trackRez.codecInfo.codecID == h264DepCodecInfo.codecID && trackRez.multiSubStream)
    {
        // split combined MVC/AVC track to substreams
        rez.push_back(trackRez);

        trackRez.codecInfo = h264CodecInfo;
        const size_t postfixPos = trackRez.streamDescr.find("3d-pg");
        if (postfixPos != string::npos)
            trackRez.streamDescr = trackRez.streamDescr.substr(0, postfixPos);

        rez.push_back(trackRez);
    }
    else
        rez.push_back(trackRez);
}

CheckStreamRez METADemuxer::detectTrackReader(uint8_t* tmpBuffer, int len,
                                              AbstractStreamReader::ContainerType containerType, int containerDataType,
                                              int containerStreamIndex)
{
    CheckStreamRez rez;

    auto pgsReader = new PGSStreamReader();
    rez = pgsReader->checkStream(tmpBuffer, len, containerType, containerDataType, containerStreamIndex);
    delete pgsReader;
    if (rez.codecInfo.codecID)
        return rez;

    auto srtReader = new SRTStreamReader();
    rez = srtReader->checkStream(tmpBuffer, len, containerType, containerDataType, containerStreamIndex);
    delete srtReader;
    if (rez.codecInfo.codecID)
        return rez;

    if (len == 0)
        return rez;

    auto lpcmReader = new LPCMStreamReader();
    rez = lpcmReader->checkStream(tmpBuffer, len, containerType, containerDataType, containerStreamIndex);
    delete lpcmReader;
    if (rez.codecInfo.codecID)
        return rez;

    auto h264codec = new H264StreamReader();
    rez = h264codec->checkStream(tmpBuffer, len);
    delete h264codec;
    if (rez.codecInfo.codecID)
        return rez;

    auto dtscodec = new DTSStreamReader();
    rez = dtscodec->checkStream(tmpBuffer, len, containerType, containerDataType, containerStreamIndex);
    delete dtscodec;
    if (rez.codecInfo.codecID)
        return rez;

    auto ac3codec = new AC3StreamReader();
    rez = ac3codec->checkStream(tmpBuffer, len, containerType, containerDataType, containerStreamIndex);
    delete ac3codec;
    if (rez.codecInfo.codecID)
        return rez;

    auto mlpcodec = new MLPStreamReader();
    rez = mlpcodec->checkStream(tmpBuffer, len, containerType, containerDataType, containerStreamIndex);
    delete mlpcodec;
    if (rez.codecInfo.codecID)
        return rez;

    auto aaccodec = new AACStreamReader();
    rez = aaccodec->checkStream(tmpBuffer, len, containerType, containerDataType, containerStreamIndex);
    delete aaccodec;
    if (rez.codecInfo.codecID)
        return rez;

    auto vc1ccodec = new VC1StreamReader();
    rez = vc1ccodec->checkStream(tmpBuffer, len);
    delete vc1ccodec;
    if (rez.codecInfo.codecID)
        return rez;

    auto hevcCodec = new HEVCStreamReader();
    rez = hevcCodec->checkStream(tmpBuffer, len);
    delete hevcCodec;
    if (rez.codecInfo.codecID)
        return rez;

    auto vvcCodec = new VVCStreamReader();
    rez = vvcCodec->checkStream(tmpBuffer, len);
    delete vvcCodec;
    if (rez.codecInfo.codecID)
        return rez;

    auto mpeg2ccodec = new MPEG2StreamReader();
    rez = mpeg2ccodec->checkStream(tmpBuffer, len);
    delete mpeg2ccodec;
    if (rez.codecInfo.codecID)
        return rez;

    auto mpegAudioCodec = new MpegAudioStreamReader();
    rez = mpegAudioCodec->checkStream(tmpBuffer, len, containerType, containerDataType, containerStreamIndex);
    delete mpegAudioCodec;
    if (rez.codecInfo.codecID)
        return rez;

    auto supReader = new DVBSubStreamReader();
    rez = supReader->checkStream(tmpBuffer, len, containerType, containerDataType, containerStreamIndex);
    delete supReader;
    if (rez.codecInfo.codecID)
        return rez;

    return rez;
}

VideoAspectRatio arNameToCode(const string& arName)
{
    if (arName == "9x16" || arName == "16x9" || arName == "9:16" || arName == "16:9")
        return VideoAspectRatio::AR_16_9;
    if (arName == "3:4" || arName == "4:3" || arName == "4x3" || arName == "3x4")
        return VideoAspectRatio::AR_3_4;
    if (arName == "Square" || arName == "VGA" || arName == "1:1" || arName == "1x1" || arName == "1:1 (Square)")
        return VideoAspectRatio::AR_VGA;
    if (arName == "WIDE" || arName == "1x2,21" || arName == "1x2.21" || arName == "1:2.21" || arName == "1:2,21" ||
        arName == "2.21:1" || arName == "2,21:1" || arName == "2.21x1" || arName == "2,21x1")
        return VideoAspectRatio::AR_221_100;

    return VideoAspectRatio::AR_KEEP_DEFAULT;
}

PIPParams::PipCorner pipCornerFromStr(const std::string& value)
{
    const std::string v = trimStr(strToLowerCase(value));
    if (v == "topleft")
        return PIPParams::PipCorner::TopLeft;
    if (v == "topright")
        return PIPParams::PipCorner::TopRight;
    if (v == "bottomright")
        return PIPParams::PipCorner::BottomRight;

    return PIPParams::PipCorner::BottomLeft;
}

int pipScaleFromStr(const std::string& value)
{
    const std::string v = trimStr(strToLowerCase(value));

    if (v == "1/2" || v == "0.5")
        return 2;
    if (v == "1/4" || v == "0.25")
        return 3;
    if (v == "1.5")
        return 4;
    if (v == "fullscreen")
        return 5;

    return 1;  // default
}

AbstractStreamReader* METADemuxer::createCodec(const string& codecName, const map<string, string>& addParams,
                                               const std::string& codecStreamName, const vector<MPLSPlayItem>& mplsInfo)
{
    AbstractStreamReader* rez = nullptr;
    if (codecName == "V_MPEG4/ISO/AVC" || codecName == "V_MPEG4/ISO/MVC")
    {
        auto h264Reader = new H264StreamReader();
        rez = h264Reader;
        if (codecName == "V_MPEG4/ISO/MVC")
            h264Reader->setIsSubStream(true);

        auto itr = addParams.find("fps");
        if (itr != addParams.end())
        {
            double fps = strToDouble(itr->second.c_str());
            fps = correctFps(fps);
            dynamic_cast<H264StreamReader*>(rez)->setFPS(fps);
        }

        itr = addParams.find("delPulldown");
        if (itr != addParams.end())
            dynamic_cast<H264StreamReader*>(rez)->setRemovePulldown(true);

        itr = addParams.find("level");
        if (itr != addParams.end())
            dynamic_cast<H264StreamReader*>(rez)->setForceLevel(static_cast<uint8_t>(strToDouble(itr->second.c_str())) *
                                                                10);
        itr = addParams.find("insertSEI");
        if (itr != addParams.end())
        {
            dynamic_cast<H264StreamReader*>(rez)->setInsertSEI(H264StreamReader::SeiMethod::SEI_InsertAuto);
        }
        itr = addParams.find("autoSEI");
        if (itr != addParams.end())
        {
            dynamic_cast<H264StreamReader*>(rez)->setInsertSEI(H264StreamReader::SeiMethod::SEI_InsertAuto);
        }
        itr = addParams.find("forceSEI");
        if (itr != addParams.end())
        {
            dynamic_cast<H264StreamReader*>(rez)->setInsertSEI(H264StreamReader::SeiMethod::SEI_InsertForce);
        }
        itr = addParams.find("contSPS");
        if (itr != addParams.end())
        {
            dynamic_cast<H264StreamReader*>(rez)->setH264SPSCont(true);
        }
    }
    else if (codecName == "V_MPEGH/ISO/HEVC")
    {
        rez = new HEVCStreamReader();
        auto itr = addParams.find("fps");
        if (itr != addParams.end())
        {
            double fps = strToDouble(itr->second.c_str());
            fps = correctFps(fps);
            dynamic_cast<HEVCStreamReader*>(rez)->setFPS(fps);
        }
    }
    else if (codecName == "V_MPEGI/ISO/VVC")
    {
        rez = new VVCStreamReader();
        auto itr = addParams.find("fps");
        if (itr != addParams.end())
        {
            double fps = strToDouble(itr->second.c_str());
            fps = correctFps(fps);
            dynamic_cast<VVCStreamReader*>(rez)->setFPS(fps);
        }
    }
    else if (codecName == "V_MS/VFW/WVC1")
    {
        rez = new VC1StreamReader();
        auto itr = addParams.find("fps");
        if (itr != addParams.end())
        {
            double fps = strToDouble(itr->second.c_str());
            fps = correctFps(fps);
            dynamic_cast<VC1StreamReader*>(rez)->setFPS(fps);
        }

        itr = addParams.find("delPulldown");
        if (itr != addParams.end())
            dynamic_cast<VC1StreamReader*>(rez)->setRemovePulldown(true);
    }
    else if (codecName == "V_MPEG-2")
    {
        rez = new MPEG2StreamReader();
        auto itr = addParams.find("fps");
        if (itr != addParams.end())
        {
            double fps = strToDouble(itr->second.c_str());
            fps = correctFps(fps);
            dynamic_cast<MPEG2StreamReader*>(rez)->setFPS(fps);
        }

        itr = addParams.find("ar");
        if (itr != addParams.end())
            dynamic_cast<MPEGStreamReader*>(rez)->setAspectRatio(arNameToCode(itr->second));

        itr = addParams.find("delPulldown");
        if (itr != addParams.end())
            dynamic_cast<MPEG2StreamReader*>(rez)->setRemovePulldown(true);
    }
    else if (codecName == "A_AAC")
        rez = new AACStreamReader();
    else if (codecName == "A_MP3")
        rez = new MpegAudioStreamReader();
    else if (codecName == "S_SUP")
        rez = new DVBSubStreamReader();
    else if (codecName == "A_LPCM")
        rez = new LPCMStreamReader();
    else if (codecName == "A_MLP")
        rez = new MLPStreamReader();
    else if (codecName == "A_DTS")
    {
        rez = new DTSStreamReader();
        auto itr = addParams.find("down-to-dts");
        if (itr != addParams.end())
            dynamic_cast<DTSStreamReader*>(rez)->setDownconvertToDTS(true);
    }
    else if (codecName == "A_AC3")
    {
        rez = new AC3StreamReader();
        auto itr = addParams.find("down-to-ac3");
        if (itr != addParams.end())
            dynamic_cast<AC3StreamReader*>(rez)->setDownconvertToAC3(true);
    }
    else if (codecName == "S_HDMV/PGS")
    {
        rez = new PGSStreamReader();
        auto itr = addParams.find("bottom-offset");
        if (itr != addParams.end())
            dynamic_cast<PGSStreamReader*>(rez)->setBottomOffset(strToInt32(itr->second.c_str()));

        itr = addParams.find("3d-plane");
        if (itr != addParams.end())
            dynamic_cast<PGSStreamReader*>(rez)->setOffsetId(strToInt8u(itr->second.c_str()));

        double fps = 0.0;
        uint16_t width = 0;
        uint16_t height = 0;
        itr = addParams.find("fps");
        if (itr != addParams.end())
            fps = strToDouble(itr->second.c_str());
        itr = addParams.find("video-width");
        if (itr != addParams.end())
            width = strToInt16u(itr->second.c_str());
        itr = addParams.find("video-height");
        if (itr != addParams.end())
            height = strToInt16u(itr->second.c_str());
        dynamic_cast<PGSStreamReader*>(rez)->setVideoInfo(width, height, fps);
        itr = addParams.find("font-border");
        if (itr != addParams.end())
            dynamic_cast<PGSStreamReader*>(rez)->setFontBorder(strToInt32(itr->second.c_str()));
    }
    else if (codecName == "S_TEXT/UTF8")
    {
        auto srtReader = new SRTStreamReader();
        rez = srtReader;
        text_subtitles::Font font;
        uint16_t srtWidth = 0, srtHeight = 0;
        double fps = 0.0;
        text_subtitles::TextAnimation animation;
        for (const auto& addParam : addParams)
        {
            if (addParam.first == "font-name")
            {
                font.m_name = unquoteStr(addParam.second);
            }
            else if (addParam.first == "font-size")
                font.m_size = strToInt32(addParam.second.c_str());
            else if (addParam.first == "font-bold")
                font.m_opts |= text_subtitles::Font::BOLD;
            else if (addParam.first == "font-italic")
                font.m_opts |= text_subtitles::Font::ITALIC;
            else if (addParam.first == "font-underline")
                font.m_opts |= text_subtitles::Font::UNDERLINE;
            else if (addParam.first == "font-strike-out")
                font.m_opts |= text_subtitles::Font::STRIKE_OUT;
            else if (addParam.first == "font-color")
            {
                const string& s = addParam.second;
                if (s.size() >= 2 && s[0] == '0' && s[1] == 'x')
                    font.m_color = strToInt32u(s.substr(2, s.size() - 2).c_str(), 16);
                else if (!s.empty() && s[0] == 'x')
                    font.m_color = strToInt32u(s.substr(1, s.size() - 1).c_str(), 16);
                else
                    font.m_color = strToInt32u(s.c_str());
                if ((font.m_color & 0xff000000u) == 0)
                    font.m_color |= 0xff000000u;
            }
            else if (addParam.first == "line-spacing")
                font.m_lineSpacing = strToFloat(addParam.second.c_str());
            else if (addParam.first == "font-charset")
                font.m_charset = strToInt32(addParam.second.c_str());
            else if (addParam.first == "font-border")
                font.m_borderWidth = strToFloat(addParam.second.c_str());
            else if (addParam.first == "fps")
            {
                fps = strToDouble(addParam.second.c_str());
            }
            else if (addParam.first == "video-width")
                srtWidth = strToInt16u(addParam.second.c_str());
            else if (addParam.first == "video-height")
                srtHeight = strToInt16u(addParam.second.c_str());
            else if (addParam.first == "bottom-offset")
                srtReader->setBottomOffset(strToInt32(addParam.second.c_str()));
            else if (addParam.first == "fadein-time")
                animation.fadeInDuration = strToFloat(addParam.second.c_str());
            else if (addParam.first == "fadeout-time")
                animation.fadeOutDuration = strToFloat(addParam.second.c_str());
        }
        if (srtWidth == 0 || srtHeight == 0 || fps == 0.0)
            THROW(ERR_COMMON, "video-width, video-height and fps parameters MUST be provided for SRT tracks")
        srtReader->setVideoInfo(srtWidth, srtHeight, fps);
        srtReader->setFont(font);
        srtReader->setAnimation(animation);
    }
    else
        THROW(ERR_UNKNOWN_CODEC, "Unsupported codec " << codecName)

    if (codecName[0] == 'A')
    {
        auto itr = addParams.find("stretch");
        if (itr != addParams.end())
        {
            double stretch;
            size_t divPos = itr->second.find('/');
            if (divPos == std::string::npos)
                stretch = strToDouble(itr->second.c_str());
            else
            {
                string firstStr = itr->second.substr(0, divPos);
                string secondStr = itr->second.substr(divPos + 1, itr->second.size() - divPos - 1);
                double dFirst = strToDouble(firstStr.c_str());
                double dSecond = strToDouble(secondStr.c_str());
                if (dSecond != 0.0)
                    stretch = dFirst / dSecond;
                else
                    THROW(ERR_COMMON, "Second argument at stretch parameter can not be 0")
            }
            if (itr != addParams.end())
                dynamic_cast<SimplePacketizerReader*>(rez)->setStretch(stretch);
        }
    }
    auto itr = addParams.find("secondary");
    if (itr != addParams.end())
        rez->setIsSecondary(true);

    PIPParams pipParams;
    itr = addParams.find("pipCorner");
    if (itr != addParams.end())
        pipParams.corner = pipCornerFromStr(itr->second);
    itr = addParams.find("pipHOffset");
    if (itr != addParams.end())
        pipParams.hOffset = strToInt32(itr->second);
    itr = addParams.find("pipVOffset");
    if (itr != addParams.end())
        pipParams.vOffset = strToInt32(itr->second);
    itr = addParams.find("pipLumma");
    if (itr != addParams.end())
        pipParams.lumma = strToInt32(itr->second);
    itr = addParams.find("pipScale");
    if (itr != addParams.end())
        pipParams.scaleIndex = pipScaleFromStr(itr->second);
    rez->setPipParams(pipParams);

    if (!mplsInfo.empty() && dynamic_cast<SimplePacketizerReader*>(rez))
        dynamic_cast<SimplePacketizerReader*>(rez)->setMPLSInfo(mplsInfo);

    return rez;
}

std::vector<MPLSParser> METADemuxer::getMplsInfo(const string& mplsFileName)
{
    std::vector<MPLSParser> result;

    std::vector<std::string> mplsFiles = splitQuotedStr(mplsFileName.c_str(), '+');
    for (auto& i : mplsFiles)
    {
        auto itr = m_mplsStreamMap.find(i);
        if (itr != m_mplsStreamMap.end())
            result.push_back(itr->second);
        else
        {
            MPLSParser parser;
            if (!parser.parse(unquoteStr(i).c_str()))
                THROW(ERR_COMMON, "Can't parse play list file " << i)
            m_mplsStreamMap[i] = parser;
            result.push_back(parser);
        }
    }
    return result;
}

string METADemuxer::findBluRayFile(const string& streamDir, const string& requestDir, const string& requestFile)
{
    string dirName = streamDir.substr(0, streamDir.size() - 1);
    if (strEndWith(strToLowerCase(dirName), "ssif"))
    {
        const size_t pos = dirName.find_last_of(getDirSeparator());
        if (pos > 0)
            dirName = streamDir.substr(0, pos);
    }

    const size_t tmp = dirName.find_last_of(getDirSeparator());
    if (tmp != std::string::npos)
    {
        dirName = streamDir.substr(0, tmp + 1);
        string fileName = dirName + requestDir + getDirSeparator() + requestFile;
        if (!fileExists(fileName))
        {
            fileName = dirName + string("BACKUP") + getDirSeparator() + requestDir + getDirSeparator() + requestFile;
            if (!fileExists(fileName))
                return "";

            return fileName;
        }

        return fileName;
    }

    return "";
}

void METADemuxer::updateReport(const bool checkTime)
{
    const auto currentTime = std::chrono::steady_clock::now();
    if (!checkTime || currentTime - m_lastReportTime > std::chrono::microseconds(250000))
    {
        double progress = 100.0;
        if (m_totalSize > 0)
        {
            const int64_t currentProcessedSize = getDemuxedSize();
            progress = static_cast<double>(currentProcessedSize) / static_cast<double>(m_totalSize) * 100.0;
            if (progress > 100.0)
                progress = 100.0;
        }
        lineBack();
        cout << doubleToStr(progress, 1) << "% complete" << std::endl;
        m_lastReportTime = currentTime;
    }
}

void METADemuxer::lineBack()
{
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    const HANDLE consoleOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    GetConsoleScreenBufferInfo(consoleOutput, &csbi);
    if (csbi.dwCursorPosition.Y == m_lastProgressY)
    {
        csbi.dwCursorPosition.X = 0;
        m_lastProgressY = csbi.dwCursorPosition.Y;
        csbi.dwCursorPosition.Y--;
        SetConsoleCursorPosition(consoleOutput, csbi.dwCursorPosition);
    }
    else
        m_lastProgressY = csbi.dwCursorPosition.Y + 1;
#else
    if (!sLastMsg)
        std::cout << "\x1bM";
    sLastMsg = false;
#endif
}

// ------------------- StreamInfo ---------------------
int StreamInfo::read()
{
    // m_readRez = 0;
    int readRez = 0;
    if (m_asyncMode && !m_notificated && !m_isEOF)
    {
        m_dataReader->notify(m_readerID, m_dataReader->getPreReadThreshold());
        m_notificated = true;
    }

    if (m_lastAVRez != 0)
    {
        if (m_isEOF)
        {
            m_blockSize = 0;
            return BufferedFileReader::DATA_EOF2;
        }
        m_lastAVRez = 0;

        m_data = m_dataReader->readBlock(m_readerID, m_blockSize, readRez);
        if (readRez == BufferedFileReader::DATA_NOT_READY || readRez == BufferedFileReader::DATA_DELAYED)
        {
            m_lastAVRez = readRez;
            return readRez;
        }
        if (readRez == BufferedFileReader::DATA_EOF)
            m_isEOF = true;
        m_streamReader->setBuffer(m_data, m_blockSize, m_isEOF);
        m_readCnt += m_blockSize;
        m_notificated = false;
    }
    return readRez;
}

// ------------------------------ ContainerToReaderWrapper --------------------------------

uint8_t* ContainerToReaderWrapper::readBlock(const int readerID, uint32_t& readCnt, int& rez, bool* firstBlockVar)
{
    rez = 0;
    uint8_t* data = nullptr;
    const auto itr = m_readerInfo.find(readerID);
    if (itr == m_readerInfo.end())
        return nullptr;

    DemuxerData& demuxerData = itr->second.m_demuxerData;
    const int pid = itr->second.m_pid;
    const uint32_t nFileBlockSize = demuxerData.m_demuxer->getFileBlockSize();

    if (demuxerData.m_firstRead)
    {
        for (auto itr1 = demuxerData.m_pids.begin(); itr1 != demuxerData.m_pids.end(); ++itr1)
        {
            MemoryBlock& vect = demuxerData.demuxedData[itr1->first];
            vect.reserve(static_cast<int>(nFileBlockSize + m_readBuffOffset));
            vect.resize(static_cast<int>(m_readBuffOffset));
        }
        demuxerData.m_firstRead = false;
    }
    StreamData& streamData = demuxerData.demuxedData[pid];

    const uint32_t lastReadCnt = demuxerData.lastReadCnt[pid];
    if (lastReadCnt > 0)
    {
        demuxerData.lastReadCnt[pid] = 0;
        const size_t currentSize = streamData.size() - m_readBuffOffset;
        assert(currentSize >= lastReadCnt);
        if (currentSize > lastReadCnt)
        {
            uint8_t* dataStart = streamData.data() + m_readBuffOffset;
            memmove(dataStart, dataStart + lastReadCnt, static_cast<size_t>(currentSize) - lastReadCnt);
        }
        streamData.resize(static_cast<int>(m_readBuffOffset + currentSize - lastReadCnt));
        demuxerData.lastReadCnt[pid] = 0;
    }

    readCnt = static_cast<uint32_t>((FFMIN(streamData.size(), nFileBlockSize) - m_readBuffOffset));
    const DemuxerReadPolicy policy = demuxerData.m_pids[pid];
    if ((readCnt > 0 && (policy == DemuxerReadPolicy::drpFragmented || demuxerData.lastReadCnt[pid] == DATA_EOF2 ||
                         demuxerData.lastReadCnt[pid] == DATA_EOF2)) ||
        readCnt >= MIN_READED_BLOCK)
    {
        data = streamData.data();
        demuxerData.lastReadCnt[pid] = readCnt;
        demuxerData.lastReadRez[pid] = 0;
    }
    else if (demuxerData.lastReadRez[pid] != DATA_DELAYED || demuxerData.m_allFragmented)
    {
        int demuxRez;
        do
        {
            int64_t discardSize = 0;
            demuxRez =
                demuxerData.m_demuxer->simpleDemuxBlock(demuxerData.demuxedData, demuxerData.m_pidSet, discardSize);
            for (auto itr1 = demuxerData.demuxedData.begin(); itr1 != demuxerData.demuxedData.end() && !m_terminated;
                 ++itr1)
            {
                if (itr1->second.size() > MAX_DEMUX_BUFFER_SIZE)
                {
                    string ext = strToUpperCase(extractFileExt(demuxerData.m_streamName));
                    if (ext != "MOV" && ext != "MP4" && ext != "M4V" && ext != "M4A")
                        THROW(ERR_CONTAINER_STREAM_NOT_SYNC,
                              "Reading buffer overflow. Possible container streams are not syncronized. Please, verify "
                              "stream fps. File name: "
                                  << demuxerData.m_streamName)
                }
            }
            m_discardedSize += discardSize;
            readCnt = static_cast<uint32_t>((FFMIN(streamData.size(), nFileBlockSize) - m_readBuffOffset));
        } while (demuxRez == 0 && readCnt < MIN_READED_BLOCK && policy != DemuxerReadPolicy::drpFragmented &&
                 !m_terminated);

        demuxerData.lastReadCnt[pid] = readCnt;
        data = streamData.data();
        if (readCnt > 0)
        {
            rez = demuxerData.m_demuxer->getLastReadRez();
        }
        else if (demuxerData.m_demuxer->getLastReadRez() == DATA_EOF)
            rez = DATA_EOF;
        else
        {
            if (policy == DemuxerReadPolicy::drpReadSequence)
                rez = DATA_NOT_READY;
            else
                rez = DATA_DELAYED;
        }
        demuxerData.lastReadRez[pid] = rez;
    }
    else
        rez = DATA_DELAYED;
    return data;
}

void ContainerToReaderWrapper::terminate()
{
    m_terminated = true;
    for (const auto& demuxer : m_demuxers) demuxer.second.m_demuxer->terminate();
}

void ContainerToReaderWrapper::resetDelayedMark() const
{
    for (auto& itr : m_readerInfo)
    {
        DemuxerData& demuxerData = itr.second.m_demuxerData;
        for (auto& itr2 : demuxerData.lastReadRez)
        {
            if (itr2.second == DATA_DELAYED)
                itr2.second = 0;
        }
    }
}

int ContainerToReaderWrapper::createReader(const int readBuffOffset)
{
    m_readBuffOffset = readBuffOffset;
    return ++m_readerCnt;
}

void ContainerToReaderWrapper::deleteReader(const int readerID)
{
    const auto itr = m_readerInfo.find(readerID);
    if (itr == m_readerInfo.end())
        return;
    const ReaderInfo& ri = itr->second;
    ri.m_demuxerData.m_pids.erase(ri.m_pid);
    if (ri.m_demuxerData.m_pids.empty())
    {
        delete ri.m_demuxerData.m_demuxer;
        m_demuxers.erase(ri.m_demuxerData.m_streamName);
    }
    m_readerInfo.erase(itr);
}

bool ContainerToReaderWrapper::openStream(int readerID, const char* streamName, int pid, const CodecInfo* codecInfo)
{
    AbstractDemuxer* demuxer = m_demuxers[streamName].m_demuxer;
    if (demuxer == nullptr)
    {
        string ext = strToUpperCase(extractFileExt(streamName));
        if ((ext == "264" || ext == "H264" || ext == "MVC") && pid)
        {
            demuxer = m_demuxers[streamName].m_demuxer = new CombinedH264Demuxer(m_readManager, "");
            m_demuxers[streamName].m_streamName = streamName;
        }
        else if (ext == "TS" || ext == "M2TS" || ext == "MTS" || ext == "M2T" || ext == "SSIF")
        {
            demuxer = m_demuxers[streamName].m_demuxer = new TSDemuxer(m_readManager, "");
            m_demuxers[streamName].m_streamName = streamName;
        }
        else if (ext == "EVO" || ext == "VOB" || ext == "MPG" || ext == "MPEG")
        {
            demuxer = m_demuxers[streamName].m_demuxer = new ProgramStreamDemuxer(m_readManager);
            m_demuxers[streamName].m_streamName = streamName;
        }
        else if (ext == "MKV" || ext == "MKA" || ext == "MKS")
        {
            demuxer = m_demuxers[streamName].m_demuxer = new MatroskaDemuxer(m_readManager);
            m_demuxers[streamName].m_streamName = streamName;
        }
        else if (ext == "MOV" || ext == "MP4" || ext == "M4V" || ext == "M4A")
        {
            demuxer = m_demuxers[streamName].m_demuxer = new MovDemuxer(m_readManager);
            m_demuxers[streamName].m_streamName = streamName;
        }
        else
            THROW(ERR_UNSUPPORTER_CONTAINER_FORMAT, "Unsupported container format: " << streamName)
        demuxer->setFileIterator(m_demuxers[streamName].m_iterator);

        demuxer->openFile(streamName);
    }

    if (SubTrackFilter::isSubTrack(pid))
    {
        if (!demuxer || !demuxer->isPidFilterSupported())
            THROW(ERR_INVALID_CODEC_FORMAT, "Unsupported parameter subTrack for format " << extractFileExt(streamName))
        int srcPID = pid >> 16;
        if (demuxer->getPidFilter(srcPID) == nullptr)
        {
            if (codecInfo->codecID == CODEC_V_MPEG4_H264 || codecInfo->codecID == CODEC_V_MPEG4_H264_DEP)
                demuxer->setPidFilter(srcPID, new CombinedH264Filter(srcPID));
            else
                THROW(ERR_INVALID_CODEC_FORMAT, "Unsupported parameter subTrack for codec " << codecInfo->displayName)
        }
    }

    auto tsDemuxer = dynamic_cast<TSDemuxer*>(demuxer);
    if (tsDemuxer)
    {
        auto itr = m_owner.m_mplsStreamMap.find(streamName);
        if (itr != m_owner.m_mplsStreamMap.end())
            tsDemuxer->setMPLSInfo(itr->second.m_playItems);
    }

    if (codecInfo &&
        (codecInfo->codecID == CODEC_S_PGS || codecInfo->codecID == CODEC_S_SUP || codecInfo->codecID == CODEC_S_SRT))
        m_demuxers[streamName].m_pids[pid] = DemuxerReadPolicy::drpFragmented;
    else
    {
        m_demuxers[streamName].m_pids[pid] = DemuxerReadPolicy::drpReadSequence;
        m_demuxers[streamName].m_allFragmented = false;
    }
    m_demuxers[streamName].m_pidSet.insert(pid);
    m_readerInfo.insert(std::make_pair(readerID, ReaderInfo(m_demuxers[streamName], pid)));
    return true;
}

void ContainerToReaderWrapper::setFileIterator(const char* streamName, FileNameIterator* itr)
{
    if (m_demuxers[streamName].m_iterator == nullptr)
        m_demuxers[streamName].m_iterator = itr;
}
