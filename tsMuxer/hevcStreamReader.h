#ifndef HEVC_STREAM_READER_H_
#define HEVC_STREAM_READER_H_

#include <map>

#include "abstractDemuxer.h"
#include "hevc.h"
#include "mpegStreamReader.h"

class HEVCStreamReader final : public MPEGStreamReader
{
   public:
    HEVCStreamReader();
    ~HEVCStreamReader() override;
    int getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors) override;
    int setDoViDescriptor(uint8_t* dstBuff) const;
    CheckStreamRez checkStream(uint8_t* buffer, int len);
    [[nodiscard]] bool needSPSForSplit() const override { return false; }

   protected:
    const CodecInfo& getCodecInfo() override { return hevcCodecInfo; }
    int intDecodeNAL(uint8_t* buff) override;

    double getStreamFPS(void* curNalUnit) override;
    [[nodiscard]] unsigned getStreamWidth() const override;
    [[nodiscard]] unsigned getStreamHeight() const override;
    [[nodiscard]] int getStreamHDR() const override;
    bool getInterlaced() override { return false; }
    bool isIFrame() override { return m_lastIFrame; }

    void updateStreamFps(void* nalUnit, uint8_t* buff, uint8_t* nextNal, int oldSpsLen) override;
    int getFrameDepth() override { return m_frameDepth; }
    int writeAdditionData(uint8_t* dstBuffer, uint8_t* dstEnd, AVPacket& avPacket,
                          PriorityDataInfo* priorityData) override;
    void onSplitEvent() override { m_firstFileFrame = true; }
    bool skipNal(uint8_t* nal) override;

   private:
    [[nodiscard]] bool isSlice(HevcUnit::NalType nalType) const;
    [[nodiscard]] bool isSuffix(HevcUnit::NalType nalType) const;
    void incTimings();
    int toFullPicOrder(const HevcSliceHeader* slice, unsigned pic_bits);
    static void storeBuffer(MemoryBlock& dst, const uint8_t* data, const uint8_t* dataEnd);
    uint8_t* writeBuffer(MemoryBlock& srcData, uint8_t* dstBuffer, const uint8_t* dstEnd) const;
    uint8_t* writeNalPrefix(uint8_t* curPos) const;

    typedef std::map<int, HevcVpsUnit*> VPSMap;

    HevcVpsUnit* m_vps;
    HevcSpsUnit* m_sps;
    HevcPpsUnit* m_pps;
    HevcHdrUnit* m_hdr;
    HevcSliceHeader* m_slice;
    bool m_firstFrame;

    int m_frameNum;
    int m_fullPicOrder;
    int m_picOrderBase;
    int m_frameDepth;

    int m_picOrderMsb;
    int m_prevPicOrder;
    bool m_lastIFrame;

    MemoryBlock m_vpsBuffer;
    MemoryBlock m_spsBuffer;
    MemoryBlock m_ppsBuffer;
    bool m_firstFileFrame;
    int m_vpsCounter;
    int m_vpsSizeDiff;
};

#endif  // _HEVC_STREAM_READER_H_
