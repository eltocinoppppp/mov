#ifndef TEXT_SUBTITLES_H_
#define TEXT_SUBTITLES_H_

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN  // Exclude rarely-used stuff from Windows headers
#include "windows.h"
#endif

#include <types/types.h>

#include <map>
#include <string>

#include "textSubtitlesRender.h"

namespace text_subtitles
{
enum class CompositionMode
{
    Start,
    Update,
    Finish
};

struct TextAnimation
{
    TextAnimation() : fadeInDuration(0.0), fadeOutDuration(0.0) {}

    float fadeInDuration;
    float fadeOutDuration;
};

static constexpr uint8_t PCS_DEF_SEGMENT = 0x16;
static constexpr uint8_t WINDOWS_DEF_SEGMENT = 0x17;
static constexpr uint8_t PALETTE_DEF_SEGMENT = 0x14;
static constexpr uint8_t OBJECT_DEF_SEGMENT = 0x15;
static constexpr uint8_t END_DEF_SEGMENT = 0x80;

static constexpr uint8_t EPOTH_NORMAL = 0;
static constexpr uint8_t EPOTH_START = 2;

static constexpr double PIXEL_DECODING_RATE = 128.0 / 8 * 1000000;  // in bytes
static constexpr double PIXEL_COMPOSITION_RATE = 256.0 / 8 * 1000000;

class TextToPGSConverter  //: public TextSubtitlesRenderWin32
{
   public:
    typedef std::map<uint8_t, YUVQuad> Palette;

    TextToPGSConverter(bool sourceIsText);
    ~TextToPGSConverter();
    void setVideoInfo(uint16_t width, uint16_t height, double fps);
    void enlargeCrop(uint16_t width, uint16_t height, uint16_t* newWidth, uint16_t* newHeight) const;
    void setBottomOffset(const int offset) { m_bottomOffset = offset; }
    uint8_t* doConvert(const std::string& text, const TextAnimation& animation, double inTimeSec, double outTimeSec,
                       uint32_t& dstBufSize);
    TextSubtitlesRender* m_textRender;
    static YUVQuad RGBAToYUVA(uint32_t data);
    static RGBQUAD YUVAToRGBA(const YUVQuad& yuv);
    void setImageBuffer(uint8_t* value) { m_imageBuffer = value; }

    int m_rleLen;
    int m_bottomOffset;
    uint16_t m_composition_number;
    uint16_t m_videoWidth;
    uint16_t m_videoHeight;
    double m_videoFps;
    uint8_t* m_pgsBuffer;
    uint8_t* m_imageBuffer;
    long composePresentationSegment(uint8_t* buff, CompositionMode mode, int64_t pts, int64_t dts, uint16_t top,
                                    bool needPgHeader, bool forced);
    long composeWindowDefinition(uint8_t* buff, int64_t pts, int64_t dts, uint16_t top, uint16_t height,
                                 bool needPgHeader = true) const;
    long composeWindow(uint8_t* buff, uint16_t top, uint16_t height) const;
    long composePaletteDefinition(const Palette& palette, uint8_t* buff, int64_t pts, int64_t dts,
                                  bool needPgHeader = true) const;
    long composeObjectDefinition(uint8_t* buff, int64_t pts, int64_t dts, int firstLine, int lastLine,
                                 bool needPgHeader) const;
    long composeVideoDescriptor(uint8_t* buff) const;
    static long composeCompositionDescriptor(uint8_t* buff, uint16_t number, uint8_t state);
    long composeEnd(uint8_t* buff, int64_t pts, int64_t dts, bool needPgHeader = true);
    static long writePGHeader(uint8_t* buff, int64_t pts, int64_t dts);
    [[nodiscard]] double alignToGrid(double value) const;
    bool rlePack(uint32_t colorMask);
    void reduceColors(uint8_t mask) const;
    static int getRepeatCnt(const uint32_t* pos, const uint32_t* end, uint32_t colorMask);
    uint8_t color32To8(const uint32_t* buff, uint32_t colorMask);
    Palette buildPalette(float opacity);
    [[nodiscard]] uint16_t renderedHeight() const;
    [[nodiscard]] uint16_t minLine() const;
    [[nodiscard]] uint16_t maxLine() const;

    std::map<YUVQuad, uint8_t> m_paletteYUV;
    uint8_t* m_renderedData;

    Palette m_paletteByColor;
    bool palette_update_flag;
    uint8_t m_paletteID;
    uint8_t m_paletteVersion;
    uint16_t m_minLine;
    uint16_t m_maxLine;
};
};  // namespace text_subtitles

#endif
