#include "library/artwork.h"

#include <windows.h>

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

using Microsoft::WRL::ComPtr;

namespace arpegio {

namespace {

constexpr UINT kThumbnailSize = 512;

struct ImageType {
    const char* mime;
    const char* extension;
};

std::optional<ImageType> detect_type(const std::vector<uint8_t>& image) {
    const auto starts = [&image](const void* bytes, size_t size, size_t offset = 0) {
        return image.size() >= offset + size && std::memcmp(image.data() + offset, bytes, size) == 0;
    };
    if (starts("\xFF\xD8\xFF", 3)) return ImageType{"image/jpeg", ".jpg"};
    if (starts("\x89PNG", 4)) return ImageType{"image/png", ".png"};
    if (starts("GIF", 3)) return ImageType{"image/gif", ".gif"};
    if (starts("BM", 2)) return ImageType{"image/bmp", ".bmp"};
    if (starts("RIFF", 4) && starts("WEBP", 4, 8)) return ImageType{"image/webp", ".webp"};
    return std::nullopt;
}

std::string hash_bytes(const std::vector<uint8_t>& data) {
    uint64_t hash = 14695981039346656037ull;  // FNV-1a, only a cache key
    for (const uint8_t byte : data) hash = (hash ^ byte) * 1099511628211ull;
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

// Written beside the target and renamed, so a stopped scan never leaves half a file under its final name.
bool replace_file(const std::filesystem::path& path, const std::filesystem::path& temporary) {
    std::error_code code;
    std::filesystem::rename(temporary, path, code);
    if (code) std::filesystem::remove(temporary, code);
    return std::filesystem::exists(path, code);
}

bool write_original(const std::filesystem::path& path, const std::vector<uint8_t>& image) {
    std::filesystem::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary);
        output.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
        if (!output) return false;
    }
    return replace_file(path, temporary);
}

bool encode_jpeg(IWICImagingFactory* factory, IWICBitmapSource* source, UINT width, UINT height, const std::wstring& path) {
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    return SUCCEEDED(factory->CreateStream(&stream)) &&
           SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
           SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) &&
           SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
           SUCCEEDED(encoder->CreateNewFrame(&frame, &properties)) && SUCCEEDED(frame->Initialize(properties.Get())) &&
           SUCCEEDED(frame->SetSize(width, height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
           SUCCEEDED(frame->WriteSource(source, nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

bool write_thumbnail(IWICImagingFactory* factory, IWICBitmapSource* image, UINT width, UINT height,
                     const std::filesystem::path& path) {
    const double scale = std::min(1.0, static_cast<double>(kThumbnailSize) / std::max(width, height));
    const UINT scaled_width = std::max(1u, static_cast<UINT>(width * scale));
    const UINT scaled_height = std::max(1u, static_cast<UINT>(height * scale));

    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(image, scaled_width, scaled_height, WICBitmapInterpolationModeHighQualityCubic)) ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return false;
    }

    std::filesystem::path temporary = path;
    temporary += L".tmp";
    if (!encode_jpeg(factory, converter.Get(), scaled_width, scaled_height, temporary.wstring())) {
        std::error_code code;
        std::filesystem::remove(temporary, code);
        return false;
    }
    return replace_file(path, temporary);
}

}  // namespace

ArtworkCache::ArtworkCache(const std::filesystem::path& root) : root_(root) {
    std::filesystem::create_directories(root_ / "artwork");
    std::filesystem::create_directories(root_ / "thumbs");
}

std::optional<ArtworkFile> ArtworkCache::store(const std::vector<uint8_t>& image) {
    if (image.empty()) return std::nullopt;
    const std::string hash = hash_bytes(image);
    if (const auto known = seen_.find(hash); known != seen_.end()) return known->second;
    std::optional<ArtworkFile>& result = seen_[hash];

    const std::optional<ImageType> type = detect_type(image);
    if (!type) return std::nullopt;

    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT width = 0;
    UINT height = 0;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(image.data()), static_cast<DWORD>(image.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0) {
        return std::nullopt;
    }

    ArtworkFile artwork{hash, hash + type->extension, type->mime, static_cast<int>(width), static_cast<int>(height)};
    std::error_code code;
    const std::filesystem::path original = original_path(artwork);
    if (!std::filesystem::exists(original, code) && !write_original(original, image)) return std::nullopt;
    const std::filesystem::path thumbnail = root_ / "thumbs" / (hash + ".jpg");
    if (!std::filesystem::exists(thumbnail, code)) write_thumbnail(factory.Get(), frame.Get(), width, height, thumbnail);

    result = std::move(artwork);
    return result;
}

std::filesystem::path ArtworkCache::original_path(const ArtworkFile& artwork) const {
    return root_ / "artwork" / artwork.file;
}

std::filesystem::path ArtworkCache::thumbnail_path(const ArtworkFile& artwork) const {
    const std::filesystem::path thumbnail = root_ / "thumbs" / (artwork.hash + ".jpg");
    std::error_code code;
    return std::filesystem::exists(thumbnail, code) ? thumbnail : original_path(artwork);
}

}  // namespace arpegio
