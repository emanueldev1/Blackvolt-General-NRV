#pragma once

#include "FFmpeg.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bv::media {

struct SubtitleCue {
    double start = 0, end = 0; // seconds
    std::string text;          // UTF-8, lines separated by '\n', formatting removed
    bool openEnded = false;    // the stream gave no end: closed by the next cue
};

// Text subtitles (SubRip, ASS/SSA, mov_text, WebVTT...) as plain text cues. FFmpeg hands every
// text format over as ASS events; tags are stripped. Bitmap subtitles (PGS, DVD) are not text.
class SubtitleDecoder {
public:
    static std::optional<SubtitleDecoder> open(const AVCodecParameters* params, AVRational timeBase);
    SubtitleDecoder(SubtitleDecoder&&) noexcept = default;
    SubtitleDecoder& operator=(SubtitleDecoder&&) noexcept = default;

    // Appends the cues in `packet` (zero or more).
    void decode(const AVPacket* packet, std::vector<SubtitleCue>& out);
    void flush();

    static bool isText(AVCodecID id);

private:
    SubtitleDecoder() = default;
    CodecCtxPtr ctx_;
    AVRational timeBase_{1, 1000};
};

// One ASS event ("ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text") or a bare
// ASS text, to plain text: override blocks {...} removed, \N \n -> newline, \h -> space.
std::string assToPlain(std::string_view event);

} // namespace bv::media
