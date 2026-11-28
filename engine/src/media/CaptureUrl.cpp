#include "CaptureUrl.h"

#include <charconv>

namespace bv::media {
namespace {

constexpr std::string_view kScheme = "capture:";
constexpr std::string_view kScreen = "screen:";

bool parseSize(std::string_view v, int& w, int& h) {
    const auto x = v.find('x');
    if (x == std::string_view::npos) return false;
    const auto a = std::from_chars(v.data(), v.data() + x, w);
    const auto b = std::from_chars(v.data() + x + 1, v.data() + v.size(), h);
    return a.ec == std::errc{} && b.ec == std::errc{} && w > 0 && h > 0 && w <= 8192 && h <= 8192;
}

} // namespace

bool isCaptureUrl(std::string_view url) {
    return url.substr(0, kScheme.size()) == kScheme || url.substr(0, kScreen.size()) == kScreen;
}

namespace {
std::optional<CaptureUrl> parseScreenUrl(std::string_view url, std::string* error) {
    auto fail = [&](const char* msg) -> std::optional<CaptureUrl> {
        if (error) *error = msg;
        return std::nullopt;
    };
#if !defined(_WIN32)
    (void)url;
    return fail("la captura de pantalla solo está disponible en Windows");
#else
    url.remove_prefix(kScreen.size());
    std::string_view query;
    if (const auto q = url.rfind('?'); q != std::string_view::npos && url.substr(0, 6) != "title=") {
        query = url.substr(q + 1);
        url = url.substr(0, q);
    } else if (url.substr(0, 6) == "title=") {
        // A window title may contain '?': the query starts at the last "?fps=" / "?cursor=".
        for (const auto* key : {"?fps=", "?cursor="}) {
            if (const auto pos = url.rfind(key); pos != std::string_view::npos) {
                query = url.substr(pos + 1);
                url = url.substr(0, pos);
                break;
            }
        }
    }
    CaptureUrl out;
    out.inputFormat = "gdigrab";
    if (url == "desktop") out.target = "desktop";
    else if (url.substr(0, 6) == "title=" && url.size() > 6) out.target = std::string(url);
    else return fail("pantalla o ventana no válida");
    std::string fps = "60";
    std::string cursor = "1";
    while (!query.empty()) {
        const auto amp = query.find('&');
        const auto pair = query.substr(0, amp);
        query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
        const auto eq = pair.find('=');
        if (eq == std::string_view::npos) continue;
        const auto key = pair.substr(0, eq), value = pair.substr(eq + 1);
        if (key == "fps") {
            int v = 0;
            const auto r = std::from_chars(value.data(), value.data() + value.size(), v);
            if (r.ec != std::errc{} || v <= 0 || v > 240) return fail("fps de captura inválidos");
            fps = std::string(value);
        } else if (key == "cursor") {
            cursor = value == "0" ? "0" : "1";
        } else if (key == "region") {
            int v[4] = {};
            auto s = value;
            for (int i = 0; i < 4; ++i) {
                const auto comma = s.find(',');
                const auto part = s.substr(0, comma);
                const auto r = std::from_chars(part.data(), part.data() + part.size(), v[i]);
                if (r.ec != std::errc{}) return fail("región de captura inválida"); // offsets may be negative
                s = comma == std::string_view::npos ? std::string_view{} : s.substr(comma + 1);
            }
            if (v[2] <= 0 || v[3] <= 0 || v[2] > 16384 || v[3] > 16384) return fail("región de captura inválida");
            out.options.emplace_back("offset_x", std::to_string(v[0]));
            out.options.emplace_back("offset_y", std::to_string(v[1]));
            out.options.emplace_back("video_size", std::to_string(v[2] & ~1) + "x" + std::to_string(v[3] & ~1));
        }
    }
    out.options.emplace_back("framerate", fps);
    out.options.emplace_back("draw_mouse", cursor);
    return out;
#endif
}
} // namespace

std::optional<CaptureUrl> parseCaptureUrl(std::string_view url, std::string* error) {
    auto fail = [&](const char* msg) -> std::optional<CaptureUrl> {
        if (error) *error = msg;
        return std::nullopt;
    };
    if (url.substr(0, kScreen.size()) == kScreen) return parseScreenUrl(url, error);
    if (!isCaptureUrl(url)) return fail("no es una URL de captura");
    url.remove_prefix(kScheme.size());

    std::string_view query;
    if (const auto q = url.rfind('?'); q != std::string_view::npos) {
        query = url.substr(q + 1);
        url = url.substr(0, q);
    }
    if (url.substr(0, 6) != "video=" || url.size() <= 6) return fail("falta el dispositivo de video");

    CaptureUrl out;
#if defined(_WIN32)
    out.inputFormat = "dshow";
    out.target = std::string(url);
    // Real-time buffer large enough for 4K60 bursts; small audio buffer for low latency.
    out.options = {{"rtbufsize", "512M"}, {"audio_buffer_size", "40"}};
#else
    out.inputFormat = "v4l2";
    auto video = url.substr(6);
    if (const auto a = video.find(":audio="); a != std::string_view::npos) video = video.substr(0, a);
    out.target = std::string(video);
#endif

    while (!query.empty()) {
        const auto amp = query.find('&');
        const auto pair = query.substr(0, amp);
        query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
        const auto eq = pair.find('=');
        if (eq == std::string_view::npos) continue;
        const auto key = pair.substr(0, eq), value = pair.substr(eq + 1);
        if (key == "size") {
            int w = 0, h = 0;
            if (!parseSize(value, w, h)) return fail("tamaño de captura inválido");
            out.options.emplace_back("video_size", std::string(value));
        } else if (key == "fps") {
            int fps = 0;
            const auto r = std::from_chars(value.data(), value.data() + value.size(), fps);
            if (r.ec != std::errc{} || fps <= 0 || fps > 480) return fail("fps de captura inválidos");
            out.options.emplace_back("framerate", std::string(value));
        } else if (key == "format") {
            // dshow: vcodec=mjpeg|h264 for compressed formats, pixel_format for raw ones.
            if (value == "mjpeg" || value == "h264")
                out.options.emplace_back(out.inputFormat == "dshow" ? "vcodec" : "input_format", std::string(value));
            else if (value == "nv12" || value == "yuyv422" || value == "p010")
                out.options.emplace_back(out.inputFormat == "dshow" ? "pixel_format" : "input_format", std::string(value));
            else
                return fail("formato de captura desconocido");
        }
    }
    return out;
}

std::string makeCaptureUrl(std::string_view video, std::string_view audio, int width, int height, int fps) {
    std::string url(kScheme);
    url += "video=";
    url += video;
    if (!audio.empty()) {
        url += ":audio=";
        url += audio;
    }
    char sep = '?';
    if (width > 0 && height > 0) {
        url += sep;
        url += "size=" + std::to_string(width) + "x" + std::to_string(height);
        sep = '&';
    }
    if (fps > 0) {
        url += sep;
        url += "fps=" + std::to_string(fps);
    }
    return url;
}

} // namespace bv::media
