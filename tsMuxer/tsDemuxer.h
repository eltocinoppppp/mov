#ifndef TS_DEMUXER_H
#define TS_DEMUXER_H

#include <cmath>
#include <map>
#include <set>
#include <string>

#include "aac.h"
#include "abstractDemuxer.h"
#include "bufferedReader.h"
#include "bufferedReaderManager.h"
#include "tsPacket.h"

// typedef StreamReaderMap std::map<int, AbstractStreamReader*>;
class TSDemuxer final : public AbstractDemuxer
{
   public:
    TSDemuxer(const BufferedReaderManager& readManager, const char* streamName);
    ~TSDemuxer() override;
    void openFile(const std::string& streamName) override;
    void readClose() override;
    int64_t getDemuxedSize() override;
    int simpleDemuxBlock(DemuxedData& demuxedData, const PIDSet& acceptedPIDs, int64_t& discardSize) override;
    void getTrackList(std::map<int32_t, TrackInfo>& trackList) override;
    int getLastReadRez() override { return m_lastReadRez; }
    void setFileIterator(FileNameIterator* itr) override;
    int64_t getTrackDelay(const int32_t pid) override
    {
        if (m_firstPtsTime.find(pid) != m_firstPtsTime.end())
        {
            const int64_t clockTicks = m_firstPtsTime[pid] - (m_firstVideoPTS != -1 ? m_firstVideoPTS : m_firstPTS);
            return llround(static_cast<double>(clockTicks) / 90.0);  // convert to ms
        }

        return 0;
    }
    void setMPLSInfo(const std::vector<MPLSPlayItem>& mplsInfo) { m_mplsInfo = mplsInfo; }
    [[nodiscard]] int64_t getFileDurationNano() const override;

   private:
    [[nodiscard]] bool mvcContinueExpected() const;

    int64_t m_firstPCRTime;
    bool m_m2tsHdrDiscarded;
    int m_lastReadRez;
    bool m_m2tsMode;
    int m_scale;
    int m_nptPos;
    const BufferedReaderManager& m_readManager;
    std::string m_streamName;
    std::string m_streamNameLow;
    std::map<int, int64_t> m_lastPesPts;
    std::map<int, int64_t> m_lastPesDts;
    std::map<int, int64_t> m_firstPtsTime;
    std::map<int, int64_t> m_firstDtsTime;
    std::map<int, int64_t> m_fullPtsTime;
    std::map<int, int64_t> m_fullDtsTime;
    AbstractReader* m_bufferedReader;
    int m_readerID;
    uint8_t* m_curPos;
    int m_pmtPid;
    bool m_codecReady;
    bool m_firstCall;
    int64_t m_readCnt;
    int64_t m_dataProcessed;
    bool m_notificated;
    TS_program_map_section m_pmt;
    uint8_t m_tmpBuffer[TS_FRAME_SIZE + 4];
    int64_t m_tmpBufferLen;
    // int64_t m_firstDTS;
    int64_t m_firstPTS;
    int64_t m_lastPTS;
    int64_t m_prevFileLen;
    uint32_t m_curFileNum;
    int64_t m_firstVideoPTS;
    int64_t m_lastVideoPTS;
    int64_t m_lastVideoDTS;
    int64_t m_videoDtsGap;
    std::vector<MPLSPlayItem> m_mplsInfo;
    int64_t m_lastPCRVal;
    bool m_nonMVCVideoFound;

    // cache to improve speed
    uint8_t m_acceptedPidCache[8192];
    bool m_firstDemuxCall;

    static bool isVideoPID(StreamType streamType);
    bool checkForRealM2ts(const uint8_t* buffer, const uint8_t* end) const;
};

#endif
