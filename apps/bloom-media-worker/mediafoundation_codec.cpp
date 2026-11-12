// The Media Foundation decoders and encoders of the FFmpeg worker's Windows generation: see
// platform_codec.hpp.
#include "platform_codec.hpp"

#if !defined(_WIN32)
#error "mediafoundation_codec.cpp requires Windows"
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <objbase.h>

#include <d3d11.h>
#include <d3d11_4.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <strmif.h>
#include <wrl/client.h>

// The encoder property identifiers are declared, not defined, by the SDK header; this file is
// their one definition.
#include <initguid.h>

#include <codecapi.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/bsf.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
}

namespace bloom::media::ffmpeg {
namespace {

using Microsoft::WRL::ComPtr;
using provider::Error;
using provider::Unavailable;

[[noreturn]] void fail(const Error error, std::string detail) {
    throw Unavailable{error, std::move(detail)};
}
void require(const bool condition, const char* detail, const Error error = Error::Corrupt) {
    if (!condition)
        fail(error, detail);
}
void succeeded(const HRESULT result, const char* detail, const Error error = Error::Unavailable) {
    if (FAILED(result))
        fail(error, detail);
}

// Media Foundation for the process, started once and left running until the process ends: the
// worker exits through its message loop, and a transform must not outlive a shutdown.
[[nodiscard]] bool started() {
    static const bool ready = [] {
        const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE)
            return false;
        return SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    }();
    return ready;
}

[[nodiscard]] ComPtr<IMFSample> sampleOf(const std::uint8_t* data, const std::size_t size) {
    ComPtr<IMFMediaBuffer> buffer;
    succeeded(MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer), "Media allocation limit",
              Error::Oversized);
    BYTE* target = nullptr;
    succeeded(buffer->Lock(&target, nullptr, nullptr), "Media allocation limit", Error::Oversized);
    std::memcpy(target, data, size);
    buffer->Unlock();
    succeeded(buffer->SetCurrentLength(static_cast<DWORD>(size)), "Media allocation limit",
              Error::Oversized);
    ComPtr<IMFSample> sample;
    succeeded(MFCreateSample(&sample), "Media allocation limit", Error::Oversized);
    succeeded(sample->AddBuffer(buffer.Get()), "Media allocation limit", Error::Oversized);
    return sample;
}

// A locked view of a sample's bytes, with the row pitch when the buffer reports one.
class Locked final {
  public:
    explicit Locked(IMFSample& sample) {
        succeeded(sample.ConvertToContiguousBuffer(&buffer_), "Coded data is not readable",
                  Error::Corrupt);
        // A picture on the graphics device: its chroma rows follow the rows of its texture.
        if (ComPtr<IMFDXGIBuffer> surface; SUCCEEDED(buffer_.As(&surface))) {
            ComPtr<ID3D11Texture2D> texture;
            if (SUCCEEDED(surface->GetResource(IID_PPV_ARGS(&texture)))) {
                D3D11_TEXTURE2D_DESC description{};
                texture->GetDesc(&description);
                rows_ = description.Height;
            }
        }
        if (SUCCEEDED(buffer_.As(&planar_)) && SUCCEEDED(planar_->Lock2D(&data_, &pitch_))) {
            DWORD length = 0;
            static_cast<void>(buffer_->GetCurrentLength(&length));
            length_ = length;
            return;
        }
        planar_.Reset();
        DWORD length = 0;
        succeeded(buffer_->Lock(&data_, nullptr, &length), "Coded data is not readable",
                  Error::Corrupt);
        length_ = length;
    }
    ~Locked() {
        if (planar_)
            planar_->Unlock2D();
        else if (buffer_)
            buffer_->Unlock();
    }
    Locked(const Locked&) = delete;
    Locked& operator=(const Locked&) = delete;
    [[nodiscard]] const std::uint8_t* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t length() const noexcept { return length_; }
    // Zero when the buffer reports no pitch (rows are then packed at the frame's width).
    [[nodiscard]] long pitch() const noexcept { return pitch_; }
    // The rows of the luma plane when the picture is a texture; zero otherwise.
    [[nodiscard]] std::uint32_t rows() const noexcept { return rows_; }

  private:
    ComPtr<IMFMediaBuffer> buffer_;
    ComPtr<IMF2DBuffer> planar_;
    BYTE* data_ = nullptr;
    LONG pitch_ = 0;
    std::size_t length_ = 0;
    std::uint32_t rows_ = 0;
};

// The graphics device the decoders run on (Direct3D 11 video decoding), made once for the
// process. Without one, `reason` says why and the decoders run on the processor.
struct Graphics final {
    ComPtr<IMFDXGIDeviceManager> manager;
    std::string reason;
};
[[nodiscard]] const Graphics& graphics() {
    static const Graphics instance = [] {
        Graphics made;
        if (!started()) {
            made.reason = "the Windows media framework did not start";
            return made;
        }
        ComPtr<ID3D11Device> device;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                     D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 2,
                                     D3D11_SDK_VERSION, &device, nullptr, nullptr))) {
            made.reason = "this computer has no Direct3D 11 device that decodes video";
            return made;
        }
        // The decoder's own threads use the device.
        if (ComPtr<ID3D11Multithread> threads; SUCCEEDED(device.As(&threads)))
            threads->SetMultithreadProtected(TRUE);
        UINT token = 0;
        ComPtr<IMFDXGIDeviceManager> manager;
        if (FAILED(MFCreateDXGIDeviceManager(&token, &manager)) ||
            FAILED(manager->ResetDevice(device.Get(), token))) {
            made.reason = "the Direct3D 11 device could not be shared with the decoder";
            return made;
        }
        made.manager = std::move(manager);
        return made;
    }();
    return instance;
}

// Transforms registered for `subtype`, in the system's order of preference, each with the
// system's name for it and whether it is a hardware one.
struct Candidate final {
    ComPtr<IMFActivate> activate;
    std::string name;
    bool hardware = false;
};
[[nodiscard]] std::vector<Candidate> candidates(const GUID& category, const GUID& subtype,
                                                const bool encoder, const UINT32 flags) {
    std::vector<Candidate> found;
    if (!started())
        return found;
    MFT_REGISTER_TYPE_INFO type{MFMediaType_Video, subtype};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnumEx(category, flags, encoder ? nullptr : &type, encoder ? &type : nullptr,
                         &activates, &count)))
        return found;
    for (UINT32 index = 0; index < count; ++index) {
        Candidate candidate;
        candidate.activate.Attach(activates[index]);
        wchar_t wide[256] = {};
        UINT32 length = 0;
        if (SUCCEEDED(
                candidate.activate->GetString(MFT_FRIENDLY_NAME_Attribute, wide, 256, &length))) {
            char narrow[768] = {};
            const int written = WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length),
                                                    narrow, sizeof(narrow) - 1, nullptr, nullptr);
            candidate.name.assign(narrow, static_cast<std::size_t>(std::max(written, 0)));
        }
        UINT32 kind = 0;
        static_cast<void>(candidate.activate->GetUINT32(MF_TRANSFORM_FLAGS_Attribute, &kind));
        candidate.hardware = (kind & MFT_ENUM_FLAG_HARDWARE) != 0;
        found.push_back(std::move(candidate));
    }
    CoTaskMemFree(activates);
    return found;
}

// H.273 code points as Media Foundation names them. Zero where it has no name for one.
[[nodiscard]] UINT32 primariesName(const int code) {
    switch (code) {
    case 1:
        return MFVideoPrimaries_BT709;
    case 4:
        return MFVideoPrimaries_BT470_2_SysM;
    case 5:
        return MFVideoPrimaries_BT470_2_SysBG;
    case 6:
        return MFVideoPrimaries_SMPTE170M;
    case 7:
        return MFVideoPrimaries_SMPTE240M;
    case 9:
        return MFVideoPrimaries_BT2020;
    default:
        return 0;
    }
}
[[nodiscard]] UINT32 transferName(const int code) {
    switch (code) {
    case 1:
        return MFVideoTransFunc_709;
    case 4:
        return MFVideoTransFunc_22;
    case 5:
        return MFVideoTransFunc_28;
    case 7:
        return MFVideoTransFunc_240M;
    case 8:
        return MFVideoTransFunc_10;
    case 13:
        return MFVideoTransFunc_sRGB;
    case 16:
        return MFVideoTransFunc_2084;
    case 18:
        return MFVideoTransFunc_HLG;
    default:
        return 0;
    }
}
[[nodiscard]] UINT32 matrixName(const int code) {
    switch (code) {
    case 1:
        return MFVideoTransferMatrix_BT709;
    case 5:
    case 6:
        return MFVideoTransferMatrix_BT601;
    case 7:
        return MFVideoTransferMatrix_SMPTE240M;
    case 9:
        return MFVideoTransferMatrix_BT2020_10;
    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------------------------
// Decode.

// The presentation time of a packet crosses the decoder as a sample time; the offset keeps the
// times of a stream that starts before zero positive.
constexpr std::int64_t kTimeOffset = std::int64_t{1} << 40;

class Decoder final : public PlatformDecoder {
  public:
    Decoder(const AVStream& stream, const bool hardware) {
        const auto& parameters = *stream.codecpar;
        hevc_ = parameters.codec_id == AV_CODEC_ID_HEVC;
        if (!hevc_ && parameters.codec_id != AV_CODEC_ID_H264)
            fail(Error::Unavailable, "The Windows media framework does not read this video codec");
        format_ = static_cast<AVPixelFormat>(parameters.format);
        const bool eight = format_ == AV_PIX_FMT_YUV420P || format_ == AV_PIX_FMT_YUVJ420P;
        if (!eight && !(hevc_ && format_ == AV_PIX_FMT_YUV420P10LE))
            fail(Error::Unavailable,
                 "The Windows media framework does not read this stream's sample layout");
        depth_ = eight ? 8U : 10U;
        // The system decoders return fields woven into one picture; the software generation
        // deinterlaces, so an interlaced stream stays with it. A stream that does not say (HEVC
        // states it per picture) is caught by the proof against the software decoder's frame.
        if (parameters.field_order != AV_FIELD_PROGRESSIVE &&
            parameters.field_order != AV_FIELD_UNKNOWN)
            fail(Error::Unavailable,
                 "The Windows media framework does not read an interlaced picture");
        require(parameters.width > 0 && parameters.height > 0, "Invalid video extent");
        width_ = static_cast<std::uint32_t>(parameters.width);
        height_ = static_cast<std::uint32_t>(parameters.height);
        primaries_ = parameters.color_primaries;
        transfer_ = parameters.color_trc;
        matrix_ = parameters.color_space;
        range_ = parameters.color_range;
        siting_ = parameters.chroma_location;

        // The stream's own packets as a byte stream with start codes and its parameter sets
        // ahead of every random-access point: libavcodec's filter, which leaves a stream that
        // already is one unchanged.
        const auto* filter = av_bsf_get_by_name(hevc_ ? "hevc_mp4toannexb" : "h264_mp4toannexb");
        require(filter != nullptr, "The byte stream filter is not in the intake",
                Error::Unavailable);
        require(av_bsf_alloc(filter, &filter_) >= 0, "Media allocation limit", Error::Oversized);
        filter_->time_base_in = stream.time_base;
        require(avcodec_parameters_copy(filter_->par_in, &parameters) >= 0 &&
                    av_bsf_init(filter_) >= 0,
                "Invalid codec parameters");
        packet_ = av_packet_alloc();
        require(packet_ != nullptr, "Media allocation limit", Error::Oversized);

        // On the graphics device when the computer and the decoder take it, on the processor
        // otherwise; the note says which, and why not the device.
        if (hardware) {
            if (const auto& device = graphics(); device.manager) {
                try {
                    open(device.manager.Get());
                    hardware_ = true;
                    note_ = "Direct3D 11 hardware decode";
                    return;
                } catch (const Unavailable& refused) {
                    note_ = refused.detail;
                }
                transform_.Reset();
            } else {
                note_ = device.reason;
            }
        } else {
            note_ = "hardware decode was not asked for";
        }
        open(nullptr);
        note_ = "software decode (" + note_ + ")";
    }
    ~Decoder() override {
        av_packet_free(&packet_);
        av_bsf_free(&filter_);
    }
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // The color description and sample range of a frame are the coded stream's own (its
    // sequence parameters), which the container's description may lack or contradict. The
    // software decoder reports the stream's, so the stream's first packet is decoded once by it
    // and its frame's description is used for every picture.
    void describe(const AVPacket& packet) {
        described_ = true;
        const auto* codec = avcodec_find_decoder(filter_->par_in->codec_id);
        AVCodecContext* context = codec != nullptr ? avcodec_alloc_context3(codec) : nullptr;
        AVFrame* frame = av_frame_alloc();
        if (context != nullptr && frame != nullptr &&
            avcodec_parameters_to_context(context, filter_->par_in) >= 0) {
            context->thread_count = 1;
            if (avcodec_open2(context, codec, nullptr) >= 0 &&
                avcodec_send_packet(context, &packet) >= 0 &&
                avcodec_send_packet(context, nullptr) >= 0 &&
                avcodec_receive_frame(context, frame) >= 0) {
                primaries_ = frame->color_primaries;
                transfer_ = frame->color_trc;
                matrix_ = frame->colorspace;
                range_ = frame->color_range;
                siting_ = frame->chroma_location;
                const auto format = static_cast<AVPixelFormat>(frame->format);
                if (format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P ||
                    (hevc_ && format == AV_PIX_FMT_YUV420P10LE && depth_ == 10))
                    format_ = format;
            }
        }
        av_frame_free(&frame);
        avcodec_free_context(&context);
    }

    void send(const AVPacket* packet) override {
        if (packet != nullptr && !described_)
            describe(*packet);
        if (packet != nullptr) {
            require(packet->pts != AV_NOPTS_VALUE && packet->pts > -kTimeOffset,
                    "Video packet timestamp unavailable", Error::Unavailable);
            require(av_packet_ref(packet_, packet) >= 0, "Media allocation limit",
                    Error::Oversized);
            require(av_bsf_send_packet(filter_, packet_) >= 0, "Invalid media packet");
        } else {
            require(av_bsf_send_packet(filter_, nullptr) >= 0, "Invalid media packet");
        }
        while (true) {
            const int status = av_bsf_receive_packet(filter_, packet_);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF)
                break;
            require(status >= 0, "Invalid media packet");
            give(*packet_);
            av_packet_unref(packet_);
        }
        if (packet == nullptr) {
            static_cast<void>(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0));
            succeeded(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0),
                      "The decoder did not finish", Error::Corrupt);
            ended_ = true;
        }
        while (take()) {
        }
    }

    [[nodiscard]] Step receive(AVFrame& frame) override {
        if (pictures_.empty())
            return ended_ ? Step::End : Step::NeedsPacket;
        av_frame_unref(&frame);
        av_frame_move_ref(&frame, pictures_.front().get());
        pictures_.pop_front();
        return Step::Frame;
    }

    void flush() override {
        static_cast<void>(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0));
        if (ended_) {
            static_cast<void>(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
            ended_ = false;
        }
        av_bsf_flush(filter_);
        pictures_.clear();
        starting_ = true;
    }

    [[nodiscard]] bool hardware() const override { return hardware_; }
    [[nodiscard]] const std::string& note() const override { return note_; }

  private:
    // Starts the system decoder for the stream, on `device` when one is given.
    void open(IMFDXGIDeviceManager* device) {
        const GUID subtype = hevc_ ? MFVideoFormat_HEVC : MFVideoFormat_H264;
        for (auto& candidate : candidates(MFT_CATEGORY_VIDEO_DECODER, subtype, false,
                                          MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
                                              MFT_ENUM_FLAG_SORTANDFILTER))
            if (SUCCEEDED(candidate.activate->ActivateObject(IID_PPV_ARGS(&transform_))))
                break;
        if (!transform_)
            fail(Error::Unavailable,
                 hevc_ ? "The Windows media framework does not read HEVC on this computer (the "
                         "system's HEVC decoder is not installed or cannot start)"
                       : "The Windows media framework has no H.264 decoder on this computer");
        if (device != nullptr) {
            ComPtr<IMFAttributes> attributes;
            UINT32 aware = 0;
            if (FAILED(transform_->GetAttributes(&attributes)) ||
                FAILED(attributes->GetUINT32(MF_SA_D3D11_AWARE, &aware)) || aware == 0)
                fail(Error::Unavailable, "the system decoder does not decode on Direct3D 11");
            if (FAILED(transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                                  reinterpret_cast<ULONG_PTR>(device))))
                fail(Error::Unavailable, "the system decoder refused the Direct3D 11 device");
        }
        ComPtr<IMFMediaType> input;
        succeeded(MFCreateMediaType(&input), "Media allocation limit", Error::Oversized);
        succeeded(input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Decoder setup failed");
        succeeded(input->SetGUID(MF_MT_SUBTYPE, subtype), "Decoder setup failed");
        succeeded(MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, width_, height_),
                  "Decoder setup failed");
        succeeded(input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive),
                  "Decoder setup failed");
        if (FAILED(transform_->SetInputType(0, input.Get(), 0)))
            fail(Error::Unavailable,
                 "The Windows media framework does not read this stream (its decoder refused it)");
        negotiate();
        succeeded(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0),
                  "Decoder did not start");
        succeeded(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0),
                  "Decoder did not start");
    }

    // Chooses the decoder's own layout for the stream's depth and reads the picture's extent.
    void negotiate() {
        const GUID wanted = depth_ == 8 ? MFVideoFormat_NV12 : MFVideoFormat_P010;
        for (DWORD index = 0;; ++index) {
            ComPtr<IMFMediaType> type;
            if (FAILED(transform_->GetOutputAvailableType(0, index, &type)))
                fail(Error::Unavailable, "The Windows media framework does not read this stream "
                                         "(its decoder does not return the stream's layout)");
            GUID subtype{};
            if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != wanted)
                continue;
            succeeded(transform_->SetOutputType(0, type.Get(), 0), "Decoder setup failed");
            UINT32 width = 0, height = 0;
            succeeded(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height),
                      "Decoder setup failed");
            codedWidth_ = width;
            codedHeight_ = height;
            left_ = top_ = 0;
            std::uint32_t shownWidth = width, shownHeight = height;
            MFVideoArea area{};
            if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,
                                        reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr))) {
                left_ = static_cast<std::uint32_t>(area.OffsetX.value);
                top_ = static_cast<std::uint32_t>(area.OffsetY.value);
                shownWidth = static_cast<std::uint32_t>(area.Area.cx);
                shownHeight = static_cast<std::uint32_t>(area.Area.cy);
            }
            // The picture the software decoder returns is the stream's own extent.
            if (shownWidth != width_ || shownHeight != height_ || left_ % 2U != 0 ||
                top_ % 2U != 0 || left_ + width_ > codedWidth_ || top_ + height_ > codedHeight_)
                fail(Error::Unavailable, "The Windows media framework does not read this stream "
                                         "(its decoder returns another picture extent)");
            MFT_OUTPUT_STREAM_INFO information{};
            succeeded(transform_->GetOutputStreamInfo(0, &information), "Decoder setup failed");
            provides_ = (information.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                                MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            outputBytes_ = information.cbSize;
            return;
        }
    }

    // Whether the access unit (a byte stream with start codes) holds an instantaneous decoder
    // refresh picture.
    [[nodiscard]] bool refresh(const AVPacket& packet) const {
        const auto* data = packet.data;
        const auto size = static_cast<std::size_t>(packet.size);
        for (std::size_t at = 0; at + 3 < size; ++at) {
            if (data[at] != 0 || data[at + 1] != 0 || data[at + 2] != 1)
                continue;
            const unsigned header = data[at + 3];
            const unsigned type = hevc_ ? (header >> 1U) & 0x3FU : header & 0x1FU;
            if (hevc_ ? (type == 19 || type == 20) : type == 5)
                return true;
            at += 2;
        }
        return false;
    }

    void give(const AVPacket& packet) {
        require(packet.size > 0, "Invalid media packet");
        if (starting_) {
            // The system decoders begin at an instantaneous decoder refresh picture and return
            // nothing from any other random-access point (a recovery point of an open group of
            // pictures), where the software decoder does begin. Such a start is refused, and the
            // software generation reads the source.
            if (!refresh(packet))
                fail(Error::Unavailable,
                     "The Windows media framework does not begin at this stream's random-access "
                     "points (open groups of pictures)");
            starting_ = false;
        }
        const auto input = sampleOf(packet.data, static_cast<std::size_t>(packet.size));
        succeeded(input->SetSampleTime(packet.pts + kTimeOffset), "Decoder setup failed");
        if ((packet.flags & AV_PKT_FLAG_KEY) != 0)
            static_cast<void>(input->SetUINT32(MFSampleExtension_CleanPoint, TRUE));
        while (true) {
            const auto status = transform_->ProcessInput(0, input.Get(), 0);
            if (status == MF_E_NOTACCEPTING) {
                require(take(), "The decoder accepts no more packets and returns no picture");
                continue;
            }
            succeeded(status, "The decoder refused a packet", Error::Corrupt);
            break;
        }
    }

    // Takes one picture out of the decoder. False when it has none ready.
    [[nodiscard]] bool take() {
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            MFT_OUTPUT_DATA_BUFFER output{};
            ComPtr<IMFSample> own;
            if (!provides_) {
                ComPtr<IMFMediaBuffer> buffer;
                succeeded(MFCreateMemoryBuffer(outputBytes_, &buffer), "Media allocation limit",
                          Error::Oversized);
                succeeded(MFCreateSample(&own), "Media allocation limit", Error::Oversized);
                succeeded(own->AddBuffer(buffer.Get()), "Media allocation limit", Error::Oversized);
                output.pSample = own.Get();
            }
            DWORD flags = 0;
            const auto status = transform_->ProcessOutput(0, 1, &output, &flags);
            ComPtr<IMFSample> sample;
            if (provides_)
                sample.Attach(output.pSample);
            else
                sample = own;
            if (output.pEvents != nullptr)
                output.pEvents->Release();
            if (status == MF_E_TRANSFORM_NEED_MORE_INPUT)
                return false;
            if (status == MF_E_TRANSFORM_STREAM_CHANGE) {
                negotiate();
                continue;
            }
            succeeded(status, "The decoder failed", Error::Corrupt);
            require(sample != nullptr, "The decoder returned no picture");
            // Copied out at once: the decoder's own picture (a texture of its pool, on the
            // graphics device) goes back to it here.
            Picture picture(av_frame_alloc());
            require(picture != nullptr, "Media allocation limit", Error::Oversized);
            fill(*picture, *sample.Get());
            pictures_.push_back(std::move(picture));
            return true;
        }
        fail(Error::Corrupt, "The decoder keeps changing its output");
    }

    // The picture as the software decoder lays it out: one plane per component.
    void fill(AVFrame& frame, IMFSample& sample) const {
        LONGLONG time = 0;
        succeeded(sample.GetSampleTime(&time), "The decoder returned an untimed picture",
                  Error::Corrupt);
        av_frame_unref(&frame);
        frame.format = format_;
        frame.width = static_cast<int>(width_);
        frame.height = static_cast<int>(height_);
        require(av_frame_get_buffer(&frame, 0) >= 0, "Media allocation limit", Error::Oversized);
        frame.pts = frame.best_effort_timestamp = time - kTimeOffset;
        frame.color_primaries = primaries_;
        frame.color_trc = transfer_;
        frame.colorspace = matrix_;
        frame.color_range = range_;
        frame.chroma_location = siting_;

        const Locked locked(sample);
        const unsigned bytes = depth_ == 8 ? 1 : 2;
        require(locked.pitch() >= 0, "Decoded picture has an unexpected layout");
        const auto pitch = locked.pitch() != 0 ? static_cast<std::size_t>(locked.pitch())
                                               : static_cast<std::size_t>(codedWidth_) * bytes;
        require(pitch >= static_cast<std::size_t>(codedWidth_) * bytes,
                "Decoded picture has an unexpected layout");
        if (locked.pitch() == 0)
            require(locked.length() >= pitch * codedHeight_ + pitch * ((codedHeight_ + 1U) / 2U),
                    "Decoded picture is shorter than its layout");
        const auto chromaWidth = (width_ + 1U) / 2U, chromaHeight = (height_ + 1U) / 2U;
        const auto* luma = locked.data() + static_cast<std::size_t>(top_) * pitch;
        const auto lumaRows = locked.rows() != 0 ? locked.rows() : codedHeight_;
        require(lumaRows >= top_ + height_, "Decoded picture has an unexpected layout");
        const auto* chroma =
            locked.data() + pitch * lumaRows + static_cast<std::size_t>(top_ / 2U) * pitch;
        // A deeper sample's significant bits are the high bits of its 16-bit word.
        const unsigned shift = 16U - depth_;
        const auto copy = [&](const std::uint8_t* source, const std::size_t first,
                              const std::size_t step, const std::uint32_t width,
                              const std::uint32_t height, const int plane) {
            for (std::uint32_t row = 0; row < height; ++row) {
                const auto* from = source + static_cast<std::size_t>(row) * pitch;
                auto* to =
                    frame.data[plane] + static_cast<std::ptrdiff_t>(row) * frame.linesize[plane];
                for (std::uint32_t column = 0; column < width; ++column) {
                    const auto position = (first + static_cast<std::size_t>(column) * step) * bytes;
                    if (bytes == 1) {
                        to[column] = from[position];
                    } else {
                        std::uint16_t word = 0;
                        std::memcpy(&word, from + position, 2);
                        word = static_cast<std::uint16_t>(word >> shift);
                        std::memcpy(to + static_cast<std::size_t>(column) * 2U, &word, 2);
                    }
                }
            }
        };
        copy(luma, left_, 1, width_, height_, 0);
        copy(chroma, left_, 2, chromaWidth, chromaHeight, 1);
        copy(chroma, left_ + 1U, 2, chromaWidth, chromaHeight, 2);
    }

    bool hevc_ = false;
    unsigned depth_ = 8;
    AVPixelFormat format_ = AV_PIX_FMT_NONE;
    std::uint32_t width_ = 0, height_ = 0;
    AVColorPrimaries primaries_ = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic transfer_ = AVCOL_TRC_UNSPECIFIED;
    AVColorSpace matrix_ = AVCOL_SPC_UNSPECIFIED;
    AVColorRange range_ = AVCOL_RANGE_UNSPECIFIED;
    AVChromaLocation siting_ = AVCHROMA_LOC_UNSPECIFIED;
    AVBSFContext* filter_ = nullptr;
    AVPacket* packet_ = nullptr;
    ComPtr<IMFTransform> transform_;
    bool provides_ = false;
    DWORD outputBytes_ = 0;
    std::uint32_t codedWidth_ = 0, codedHeight_ = 0, left_ = 0, top_ = 0;
    struct FreeFrame final {
        void operator()(AVFrame* frame) const noexcept { av_frame_free(&frame); }
    };
    using Picture = std::unique_ptr<AVFrame, FreeFrame>;
    std::deque<Picture> pictures_;
    bool ended_ = false;
    // The next packet is the first of the stream or the first after a flush.
    bool starting_ = true;
    // The stream's own color description has been read (describe()).
    bool described_ = false;
    bool hardware_ = false;
    std::string note_;
};

// ---------------------------------------------------------------------------------------------
// Encode.

constexpr std::int64_t kSecond = 10000000; // Media Foundation counts time in 100 ns.

class Encoder final : public PlatformEncoder {
  public:
    explicit Encoder(const PlatformEncoding& encoding) : encoding_(encoding) {
        const bool hevc = encoding.codec == AV_CODEC_ID_HEVC;
        require(hevc || encoding.codec == AV_CODEC_ID_H264,
                "The Windows media framework does not write this video codec", Error::Unavailable);
        deep_ = encoding.format == AV_PIX_FMT_YUV420P10LE;
        require((deep_ && hevc) || encoding.format == AV_PIX_FMT_YUV420P,
                "The Windows media framework does not write this sample layout",
                Error::Unavailable);
        require(encoding.width > 0 && encoding.height > 0 && encoding.width % 2 == 0 &&
                    encoding.height % 2 == 0 && encoding.rate.num > 0 && encoding.rate.den > 0 &&
                    encoding.bitRate > 0 && encoding.bitRate <= 0xFFFFFFFFLL,
                "The Windows media framework does not write this picture extent or rate",
                Error::Unavailable);
        // The tags are written as they are asked for: one Media Foundation has no name for is
        // refused, never replaced.
        const auto& tags = encoding.colour;
        const auto primaries = primariesName(tags.primaries);
        const auto transfer = transferName(tags.transfer);
        const auto matrix = matrixName(tags.matrix);
        require(primaries != 0 && transfer != 0 && matrix != 0 &&
                    (tags.range == 1 || tags.range == 2),
                "The Windows media framework has no name for these color tags", Error::Unavailable);

        const GUID subtype = hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264;
        const auto describe = [&](const GUID& format) {
            ComPtr<IMFMediaType> type;
            succeeded(MFCreateMediaType(&type), "Media allocation limit", Error::Oversized);
            const bool set =
                SUCCEEDED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) &&
                SUCCEEDED(type->SetGUID(MF_MT_SUBTYPE, format)) &&
                SUCCEEDED(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE,
                                             static_cast<UINT32>(encoding.width),
                                             static_cast<UINT32>(encoding.height))) &&
                SUCCEEDED(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE,
                                              static_cast<UINT32>(encoding.rate.num),
                                              static_cast<UINT32>(encoding.rate.den))) &&
                SUCCEEDED(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1)) &&
                SUCCEEDED(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) &&
                SUCCEEDED(type->SetUINT32(MF_MT_VIDEO_PRIMARIES, primaries)) &&
                SUCCEEDED(type->SetUINT32(MF_MT_TRANSFER_FUNCTION, transfer)) &&
                SUCCEEDED(type->SetUINT32(MF_MT_YUV_MATRIX, matrix)) &&
                SUCCEEDED(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE,
                                          tags.range == 2 ? MFNominalRange_0_255
                                                          : MFNominalRange_16_235)) &&
                SUCCEEDED(
                    type->SetUINT32(MF_MT_VIDEO_CHROMA_SITING, MFVideoChromaSubsampling_MPEG2));
            require(set, "Encoder setup failed", Error::Unavailable);
            return type;
        };
        const auto coded = describe(subtype);
        succeeded(coded->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(encoding.bitRate)),
                  "Encoder setup failed");
        succeeded(
            coded->SetUINT32(MF_MT_MPEG2_PROFILE, hevc ? (deep_ ? eAVEncH265VProfile_Main_420_10
                                                                : eAVEncH265VProfile_Main_420_8)
                                                       : eAVEncH264VProfile_High),
            "Encoder setup failed");
        const auto raw = describe(deep_ ? MFVideoFormat_P010 : MFVideoFormat_NV12);

        // A hardware encoder when the computer has one that takes the stream, a software one
        // otherwise, in the system's order.
        for (auto& candidate :
             candidates(MFT_CATEGORY_VIDEO_ENCODER, subtype, true,
                        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SYNCMFT |
                            MFT_ENUM_FLAG_SORTANDFILTER)) {
            if (open(candidate, *coded.Get(), *raw.Get())) {
                name_ = candidate.name.empty() ? std::string("Media Foundation encoder")
                                               : candidate.name;
                hardware_ = candidate.hardware;
                break;
            }
            close();
            static_cast<void>(candidate.activate->ShutdownObject());
        }
        if (!transform_)
            fail(Error::Unavailable,
                 hevc ? "The Windows media framework has no HEVC encoder that takes this stream "
                        "on this computer"
                      : "The Windows media framework has no H.264 encoder that takes this stream "
                        "on this computer");
    }

    void send(const AVFrame* frame) override {
        if (frame == nullptr) {
            static_cast<void>(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0));
            succeeded(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0),
                      "The encoder did not finish", Error::Corrupt);
            if (events_) {
                while (!drained_)
                    static_cast<void>(handle(true));
            } else {
                while (take()) {
                }
            }
            return;
        }
        require(frame->format == encoding_.format && frame->width == encoding_.width &&
                    frame->height == encoding_.height && frame->pts >= 0,
                "Frame does not match the encoder", Error::IdentityMismatch);
        const auto input = picture(*frame);
        const auto time = av_rescale(frame->pts, kSecond * encoding_.rate.den, encoding_.rate.num);
        const auto next =
            av_rescale(frame->pts + 1, kSecond * encoding_.rate.den, encoding_.rate.num);
        succeeded(input->SetSampleTime(time), "Encoder setup failed");
        succeeded(input->SetSampleDuration(next - time), "Encoder setup failed");
        sent_[time] = frame->pts;
        if (events_) {
            // The transform says when it wants a frame; what it has finished meanwhile is taken.
            while (wanted_ == 0)
                static_cast<void>(handle(true));
            succeeded(transform_->ProcessInput(0, input.Get(), 0), "The encoder refused a frame",
                      Error::Corrupt);
            --wanted_;
            while (handle(false)) {
            }
            return;
        }
        while (true) {
            const auto status = transform_->ProcessInput(0, input.Get(), 0);
            if (status == MF_E_NOTACCEPTING) {
                require(take(), "The encoder accepts no more frames and returns no picture");
                continue;
            }
            succeeded(status, "The encoder refused a frame", Error::Corrupt);
            break;
        }
        while (take()) {
        }
    }

    [[nodiscard]] bool receive(AVPacket& packet) override {
        if (coded_.empty())
            return false;
        auto next = std::move(coded_.front());
        coded_.pop_front();
        av_packet_unref(&packet);
        require(av_new_packet(&packet, static_cast<int>(next.bytes.size())) >= 0,
                "Media allocation limit", Error::Oversized);
        std::memcpy(packet.data, next.bytes.data(), next.bytes.size());
        packet.pts = packet.dts = next.frame;
        packet.duration = 1;
        if (next.key)
            packet.flags |= AV_PKT_FLAG_KEY;
        return true;
    }

    [[nodiscard]] std::span<const std::uint8_t> parameterSets() const override {
        return parameterSets_;
    }
    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] bool hardware() const override { return hardware_; }

  private:
    struct Coded final {
        std::vector<std::uint8_t> bytes;
        std::int64_t frame = 0;
        bool key = false;
    };

    void close() {
        events_.Reset();
        transform_.Reset();
        parameterSets_.clear();
        wanted_ = 0;
    }

    // Starts one registered encoder on the stream. False when it does not take it.
    [[nodiscard]] bool open(Candidate& candidate, IMFMediaType& coded, IMFMediaType& raw) {
        if (FAILED(candidate.activate->ActivateObject(IID_PPV_ARGS(&transform_))))
            return false;
        ComPtr<IMFAttributes> attributes;
        UINT32 asynchronous = 0;
        if (SUCCEEDED(transform_->GetAttributes(&attributes)) && attributes &&
            SUCCEEDED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &asynchronous)) &&
            asynchronous != 0) {
            if (FAILED(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE)) ||
                FAILED(transform_.As(&events_)))
                return false;
        }
        // One second between random-access points, a constant bit rate at the target and no
        // pictures coded out of order, as the other delivery encoders are set.
        ComPtr<ICodecAPI> properties;
        if (SUCCEEDED(transform_.As(&properties))) {
            const auto set = [&](const GUID& property, const ULONG value) {
                VARIANT variant;
                VariantInit(&variant);
                variant.vt = VT_UI4;
                variant.ulVal = value;
                static_cast<void>(properties->SetValue(&property, &variant));
            };
            set(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
            set(CODECAPI_AVEncCommonMeanBitRate, static_cast<ULONG>(encoding_.bitRate));
            set(CODECAPI_AVEncMPVGOPSize, static_cast<ULONG>(std::max(encoding_.interval, 1)));
            set(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        }
        if (FAILED(transform_->SetOutputType(0, &coded, 0)) ||
            FAILED(transform_->SetInputType(0, &raw, 0)))
            return false;
        MFT_OUTPUT_STREAM_INFO information{};
        if (FAILED(transform_->GetOutputStreamInfo(0, &information)))
            return false;
        provides_ = (information.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                            MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        outputBytes_ = std::max<DWORD>(
            information.cbSize, static_cast<DWORD>(encoding_.width) *
                                    static_cast<DWORD>(encoding_.height) * (deep_ ? 6U : 3U));
        if (FAILED(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
            FAILED(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)))
            return false;
        readParameterSets();
        return true;
    }

    void readParameterSets() {
        if (!parameterSets_.empty())
            return;
        ComPtr<IMFMediaType> type;
        UINT8* blob = nullptr;
        UINT32 size = 0;
        if (SUCCEEDED(transform_->GetOutputCurrentType(0, &type)) &&
            SUCCEEDED(type->GetAllocatedBlob(MF_MT_MPEG_SEQUENCE_HEADER, &blob, &size))) {
            parameterSets_.assign(blob, blob + size);
            CoTaskMemFree(blob);
        }
    }

    // One event of an asynchronous transform. False when none is waiting and `wait` is off.
    [[nodiscard]] bool handle(const bool wait) {
        ComPtr<IMFMediaEvent> event;
        const auto status = events_->GetEvent(wait ? 0 : MF_EVENT_FLAG_NO_WAIT, &event);
        if (status == MF_E_NO_EVENTS_AVAILABLE)
            return false;
        succeeded(status, "The encoder failed", Error::Corrupt);
        MediaEventType type = MEUnknown;
        succeeded(event->GetType(&type), "The encoder failed", Error::Corrupt);
        HRESULT outcome = S_OK;
        if (SUCCEEDED(event->GetStatus(&outcome)))
            succeeded(outcome, "The encoder failed", Error::Corrupt);
        if (type == METransformNeedInput)
            ++wanted_;
        else if (type == METransformHaveOutput)
            require(take(), "The encoder announced a picture and returned none");
        else if (type == METransformDrainComplete)
            drained_ = true;
        return true;
    }

    // Takes one coded picture out of the encoder. False when it has none ready.
    [[nodiscard]] bool take() {
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            MFT_OUTPUT_DATA_BUFFER output{};
            ComPtr<IMFSample> own;
            if (!provides_) {
                ComPtr<IMFMediaBuffer> buffer;
                succeeded(MFCreateMemoryBuffer(outputBytes_, &buffer), "Media allocation limit",
                          Error::Oversized);
                succeeded(MFCreateSample(&own), "Media allocation limit", Error::Oversized);
                succeeded(own->AddBuffer(buffer.Get()), "Media allocation limit", Error::Oversized);
                output.pSample = own.Get();
            }
            DWORD flags = 0;
            const auto status = transform_->ProcessOutput(0, 1, &output, &flags);
            ComPtr<IMFSample> sample;
            if (provides_)
                sample.Attach(output.pSample);
            else
                sample = own;
            if (output.pEvents != nullptr)
                output.pEvents->Release();
            if (status == MF_E_TRANSFORM_NEED_MORE_INPUT)
                return false;
            if (status == MF_E_TRANSFORM_STREAM_CHANGE) {
                // The encoder restates its output (some do once they know the parameter sets).
                ComPtr<IMFMediaType> type;
                succeeded(transform_->GetOutputAvailableType(0, 0, &type), "The encoder failed",
                          Error::Corrupt);
                succeeded(transform_->SetOutputType(0, type.Get(), 0), "The encoder failed",
                          Error::Corrupt);
                continue;
            }
            succeeded(status, "The encoder failed", Error::Corrupt);
            require(sample != nullptr, "The encoder returned no picture");
            readParameterSets();
            LONGLONG time = 0;
            succeeded(sample->GetSampleTime(&time), "The encoder returned an untimed picture",
                      Error::Corrupt);
            const auto found = sent_.find(time);
            require(found != sent_.end(),
                    "The encoder returned a picture with a time it was not given");
            Coded coded;
            coded.frame = found->second;
            sent_.erase(found);
            // Pictures come back in the order they went in: nothing is coded out of order.
            require(coded.frame == returned_, "The encoder returned pictures out of order");
            ++returned_;
            UINT32 clean = 0;
            coded.key =
                SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean != 0;
            {
                const Locked locked(*sample.Get());
                require(locked.length() > 0, "The encoder returned an empty picture");
                coded.bytes.assign(locked.data(), locked.data() + locked.length());
            }
            coded_.push_back(std::move(coded));
            return true;
        }
        fail(Error::Corrupt, "The encoder keeps changing its output");
    }

    // The frame's planes as the encoder takes them: luma, then interleaved Cb and Cr.
    [[nodiscard]] ComPtr<IMFSample> picture(const AVFrame& frame) const {
        const auto width = static_cast<std::size_t>(encoding_.width);
        const auto height = static_cast<std::size_t>(encoding_.height);
        const std::size_t bytes = deep_ ? 2 : 1;
        std::vector<std::uint8_t> packed(width * height * bytes * 3U / 2U);
        auto* to = packed.data();
        for (std::size_t row = 0; row < height; ++row) {
            const auto* from = frame.data[0] + static_cast<std::ptrdiff_t>(row) * frame.linesize[0];
            if (!deep_) {
                std::memcpy(to, from, width);
            } else {
                for (std::size_t column = 0; column < width; ++column) {
                    std::uint16_t word = 0;
                    std::memcpy(&word, from + column * 2U, 2);
                    word = static_cast<std::uint16_t>(word << 6U);
                    std::memcpy(to + column * 2U, &word, 2);
                }
            }
            to += width * bytes;
        }
        for (std::size_t row = 0; row < height / 2U; ++row) {
            const auto* blue = frame.data[1] + static_cast<std::ptrdiff_t>(row) * frame.linesize[1];
            const auto* red = frame.data[2] + static_cast<std::ptrdiff_t>(row) * frame.linesize[2];
            for (std::size_t column = 0; column < width / 2U; ++column) {
                if (!deep_) {
                    to[column * 2U] = blue[column];
                    to[column * 2U + 1U] = red[column];
                } else {
                    std::uint16_t first = 0, second = 0;
                    std::memcpy(&first, blue + column * 2U, 2);
                    std::memcpy(&second, red + column * 2U, 2);
                    first = static_cast<std::uint16_t>(first << 6U);
                    second = static_cast<std::uint16_t>(second << 6U);
                    std::memcpy(to + column * 4U, &first, 2);
                    std::memcpy(to + column * 4U + 2U, &second, 2);
                }
            }
            to += width * bytes;
        }
        return sampleOf(packed.data(), packed.size());
    }

    PlatformEncoding encoding_;
    bool deep_ = false;
    ComPtr<IMFTransform> transform_;
    ComPtr<IMFMediaEventGenerator> events_;
    std::string name_;
    bool hardware_ = false;
    bool provides_ = false;
    DWORD outputBytes_ = 0;
    std::vector<std::uint8_t> parameterSets_;
    // Frames an asynchronous transform has asked for and not been given.
    unsigned wanted_ = 0;
    bool drained_ = false;
    std::map<LONGLONG, std::int64_t> sent_;
    std::int64_t returned_ = 0;
    std::deque<Coded> coded_;
};

} // namespace

std::unique_ptr<PlatformDecoder> platformDecoder(const AVStream& stream, const bool hardware) {
    return std::make_unique<Decoder>(stream, hardware);
}

namespace {

// What the coded stream itself says about its color, read by the software decoder from one frame
// a system encoder wrote. The export's tags must be in the stream, not only in the container: a
// QuickTime file has no place for the range, and a reader of the stream alone sees no other.
// Some system encoders write none of it, or only a full range (the software H.264 encoder of
// Windows); an encoder that does not write what was asked for is refused, never corrected.
void requireStreamTags(const PlatformEncoding& encoding) {
    constexpr const char* kRefusal =
        "The Windows media framework's encoder does not write these color tags into the stream";
    Encoder probe(encoding);
    struct Frames final {
        AVFrame* picture = av_frame_alloc();
        AVFrame* decoded = av_frame_alloc();
        AVPacket* packet = av_packet_alloc();
        AVCodecContext* context = nullptr;
        ~Frames() {
            avcodec_free_context(&context);
            av_packet_free(&packet);
            av_frame_free(&decoded);
            av_frame_free(&picture);
        }
    } held;
    require(held.picture != nullptr && held.decoded != nullptr && held.packet != nullptr,
            "Media allocation limit", Error::Oversized);
    held.picture->format = encoding.format;
    held.picture->width = encoding.width;
    held.picture->height = encoding.height;
    require(av_frame_get_buffer(held.picture, 0) >= 0, "Media allocation limit", Error::Oversized);
    // One mid-gray picture.
    const bool deep = encoding.format == AV_PIX_FMT_YUV420P10LE;
    for (int plane = 0; plane < 3; ++plane) {
        const int rows = plane == 0 ? encoding.height : (encoding.height + 1) / 2;
        const int columns = plane == 0 ? encoding.width : (encoding.width + 1) / 2;
        for (int row = 0; row < rows; ++row) {
            auto* to = held.picture->data[plane] +
                       static_cast<std::ptrdiff_t>(row) * held.picture->linesize[plane];
            if (!deep) {
                std::memset(to, 128, static_cast<std::size_t>(columns));
                continue;
            }
            const std::uint16_t gray = 512;
            for (int column = 0; column < columns; ++column)
                std::memcpy(to + static_cast<std::ptrdiff_t>(column) * 2, &gray, 2);
        }
    }
    held.picture->pts = 0;
    probe.send(held.picture);
    probe.send(nullptr);

    const auto* codec = avcodec_find_decoder(encoding.codec);
    held.context = codec != nullptr ? avcodec_alloc_context3(codec) : nullptr;
    require(held.context != nullptr, kRefusal, Error::Unavailable);
    held.context->thread_count = 1;
    require(avcodec_open2(held.context, codec, nullptr) >= 0, kRefusal, Error::Unavailable);
    bool decoded = false;
    // The parameter sets an encoder states apart from its pictures go first.
    if (const auto sets = probe.parameterSets(); !sets.empty()) {
        require(av_new_packet(held.packet, static_cast<int>(sets.size())) >= 0,
                "Media allocation limit", Error::Oversized);
        std::memcpy(held.packet->data, sets.data(), sets.size());
        static_cast<void>(avcodec_send_packet(held.context, held.packet));
        av_packet_unref(held.packet);
    }
    while (!decoded && probe.receive(*held.packet)) {
        static_cast<void>(avcodec_send_packet(held.context, held.packet));
        av_packet_unref(held.packet);
        decoded = avcodec_receive_frame(held.context, held.decoded) >= 0;
    }
    if (!decoded) {
        static_cast<void>(avcodec_send_packet(held.context, nullptr));
        decoded = avcodec_receive_frame(held.context, held.decoded) >= 0;
    }
    require(decoded, kRefusal, Error::Unavailable);
    const auto& tags = encoding.colour;
    const int range = held.decoded->color_range == AVCOL_RANGE_JPEG   ? 2
                      : held.decoded->color_range == AVCOL_RANGE_MPEG ? 1
                                                                      : 0;
    require(static_cast<int>(held.decoded->color_primaries) == tags.primaries &&
                static_cast<int>(held.decoded->color_trc) == tags.transfer &&
                static_cast<int>(held.decoded->colorspace) == tags.matrix && range == tags.range,
            kRefusal, Error::Unavailable);
}

} // namespace

std::unique_ptr<PlatformEncoder> platformEncoder(const PlatformEncoding& encoding) {
    requireStreamTags(encoding);
    return std::make_unique<Encoder>(encoding);
}

} // namespace bloom::media::ffmpeg
