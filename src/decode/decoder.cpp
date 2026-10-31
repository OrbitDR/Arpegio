#include "decode/decoder.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include "decode/sacd_iso.h"

namespace arpegio {

std::unique_ptr<Decoder> open_dsf_decoder(const std::filesystem::path& path);
std::unique_ptr<Decoder> open_dff_decoder(const std::filesystem::path& path);
std::unique_ptr<Decoder> open_wav_decoder(const std::filesystem::path& path);
std::unique_ptr<Decoder> open_aiff_decoder(const std::filesystem::path& path);
std::unique_ptr<Decoder> open_flac_decoder(const std::filesystem::path& path, bool ogg);
std::unique_ptr<Decoder> open_wavpack_decoder(const std::filesystem::path& path);
std::unique_ptr<Decoder> open_ape_decoder(const std::filesystem::path& path);
std::unique_ptr<Decoder> open_mp3_decoder(const std::filesystem::path& path);
std::unique_ptr<Decoder> open_vorbis_decoder(const std::filesystem::path& path);
std::unique_ptr<Decoder> open_media_foundation_decoder(const std::filesystem::path& path);

namespace {

bool has(const uint8_t* data, const char* text) {
    return std::memcmp(data, text, std::strlen(text)) == 0;
}

std::wstring lower_extension(const std::filesystem::path& path) {
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
    return extension;
}

// Bytes taken by an ID3v2 tag at the start of the file, 0 without one.
int64_t id3_size(const uint8_t* magic) {
    constexpr int kHeader = 10;
    constexpr uint8_t kFooterFlag = 0x10;
    if (!has(magic, "ID3")) return 0;
    const int64_t body = (magic[6] & 0x7F) << 21 | (magic[7] & 0x7F) << 14 | (magic[8] & 0x7F) << 7 | (magic[9] & 0x7F);
    return kHeader + body + (magic[5] & kFooterFlag ? kHeader : 0);
}

}  // namespace

std::unique_ptr<Decoder> open_decoder(const std::filesystem::path& path) {
    // Contents decide first: extensions in a real collection are not always right.
    uint8_t magic[64] = {};
    int64_t tag_size = 0;
    if (std::FILE* file = _wfopen(path.c_str(), L"rb")) {
        std::fread(magic, 1, sizeof(magic), file);
        // Taggers put ID3v2 in front of formats that are not MP3, so what identifies the file is
        // behind it. libFLAC and the APE SDK skip the tag on their own.
        tag_size = id3_size(magic);
        if (tag_size) {
            std::memset(magic, 0, sizeof(magic));
            _fseeki64(file, tag_size, SEEK_SET);
            std::fread(magic, 1, sizeof(magic), file);
        }
        std::fclose(file);
    } else {
        throw std::runtime_error("cannot open file");
    }

    if (has(magic, "DSD ")) return open_dsf_decoder(path);
    if (has(magic, "FRM8")) return open_dff_decoder(path);
    if (has(magic, "fLaC")) return open_flac_decoder(path, false);
    if ((has(magic, "RIFF") || has(magic, "RF64")) && has(magic + 8, "WAVE")) return open_wav_decoder(path);
    if (has(magic, "FORM") && (has(magic + 8, "AIFF") || has(magic + 8, "AIFC"))) return open_aiff_decoder(path);
    if (has(magic, "wvpk")) return open_wavpack_decoder(path);
    if (has(magic, "MAC ")) return open_ape_decoder(path);
    if (has(magic, "OggS")) {
        return has(magic + 29, "FLAC") ? open_flac_decoder(path, true) : open_vorbis_decoder(path);
    }

    const std::wstring extension = lower_extension(path);
    // A hybrid .iso.wv is a disc image whose payload is WavPack. libwavpack finds the stream itself.
    if (extension == L".wv") return open_wavpack_decoder(path);
    if (extension == L".iso" && is_sacd_image(path)) return open_sacd_track(path, 0, 0);
    if (has(magic + 4, "ftyp") || extension == L".m4a" || extension == L".mp4" || extension == L".aac" ||
        extension == L".wma") {
        return open_media_foundation_decoder(path);
    }
    if (tag_size || extension == L".mp3") return open_mp3_decoder(path);
    throw std::runtime_error("unsupported file format");
}

}  // namespace arpegio
