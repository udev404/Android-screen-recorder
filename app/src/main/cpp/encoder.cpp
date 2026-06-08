#include "encoder.h"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <chrono>
#include <cassert>

#define LOG_TAG "ScreenCast_Encoder"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

namespace screencast {

// ─── NativeEncoder ───────────────────────────────────────────────────────────

NativeEncoder::NativeEncoder(const EncoderConfig& cfg) : cfg_(cfg) {}

NativeEncoder::~NativeEncoder() { release(); }

bool NativeEncoder::init() {
    LOGI("Initializing encoder: %dx%d @ %d fps, %d bps, codec=%s",
         cfg_.width, cfg_.height, cfg_.fps, cfg_.bitrate_bps, cfg_.mime);

    codec_ = AMediaCodec_createEncoderByType(cfg_.mime);
    if (!codec_) {
        LOGE("Failed to create MediaCodec encoder for %s", cfg_.mime);
        return false;
    }

    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME,           cfg_.mime);
    AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_WIDTH,          cfg_.width);
    AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_HEIGHT,         cfg_.height);
    AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_FRAME_RATE,     cfg_.fps);
    AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_BIT_RATE,       cfg_.bitrate_bps);
    AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, cfg_.i_frame_interval);
    AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT,   cfg_.color_format);

    // H.264 profile/level
    if (std::string(cfg_.mime) == "video/avc") {
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_PROFILE, cfg_.profile);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_LEVEL,   cfg_.level);
    }

    // Bitrate mode: VBR (2) for quality, CBR (0) for streaming
    AMediaFormat_setInt32(fmt, "bitrate-mode", 2);

    // Priority: real-time
    AMediaFormat_setInt32(fmt, "priority", 0);

    media_status_t status = AMediaCodec_configure(
        codec_, fmt, nullptr, nullptr,
        AMEDIACODEC_CONFIGURE_FLAG_ENCODE);

    AMediaFormat_delete(fmt);

    if (status != AMEDIA_OK) {
        LOGE("AMediaCodec_configure failed: %d", status);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        return false;
    }

    // Create input surface (used by MediaProjection virtual display)
    ANativeWindow* win = nullptr;
    status = AMediaCodec_createInputSurface(codec_, &win);
    if (status != AMEDIA_OK || !win) {
        LOGE("Failed to create encoder input surface: %d", status);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        return false;
    }
    surface_ = win;

    status = AMediaCodec_start(codec_);
    if (status != AMEDIA_OK) {
        LOGE("AMediaCodec_start failed: %d", status);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        return false;
    }

    running_ = true;
    encoder_thread_ = std::thread(&NativeEncoder::encoderLoop, this);

    LOGI("Encoder started successfully");
    return true;
}

void NativeEncoder::release() {
    if (!running_) return;
    running_ = false;

    signalEndOfStream();

    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }

    if (surface_) {
        ANativeWindow_release(surface_);
        surface_ = nullptr;
    }

    if (codec_) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }

    LOGI("Encoder released");
}

jobject NativeEncoder::getInputSurface(JNIEnv* env) {
    if (!surface_) return nullptr;
    return ANativeWindow_toSurface(env, surface_);
}

void NativeEncoder::signalEndOfStream() {
    eos_requested_ = true;
    if (codec_) {
        // Signal EOS via input surface (surface mode)
        AMediaCodec_signalEndOfInputStream(codec_);
    }
}

void NativeEncoder::encoderLoop() {
    LOGI("Encoder loop started");
    const int64_t TIMEOUT_US = 10'000; // 10ms

    while (running_ || !eos_requested_) {
        processOutputBuffers();
        // Small sleep to avoid busy-wait if codec has no output
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    // Drain remaining output after EOS
    for (int drain = 0; drain < 200; ++drain) {
        processOutputBuffers();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    LOGI("Encoder loop exited. Frames=%llu Bytes=%llu",
         (unsigned long long)frames_encoded_.load(),
         (unsigned long long)bytes_encoded_.load());
}

void NativeEncoder::processOutputBuffers() {
    if (!codec_) return;

    AMediaCodecBufferInfo info{};
    ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 5'000);

    if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        AMediaFormat* fmt = AMediaCodec_getOutputFormat(codec_);
        char* desc = AMediaFormat_toString(fmt);
        LOGI("Output format changed: %s", desc);
        AMediaFormat_delete(fmt);
        return;
    }

    if (idx < 0) return; // AGAIN or error

    uint8_t* buf = AMediaCodec_getOutputBuffer(codec_, (size_t)idx, nullptr);
    if (buf && info.size > 0 && on_frame_encoded_) {
        bool is_key = (info.flags & AMEDIACODEC_BUFFER_FLAG_KEY_FRAME) != 0;

        // Update stats
        frames_encoded_++;
        bytes_encoded_ += info.size;

        // Bitrate estimation (rolling per-second)
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        bytes_last_sec_ += info.size;
        if (now_ms - last_bps_time_ >= 1000) {
            current_bps_ = (int)(bytes_last_sec_ * 8);
            bytes_last_sec_ = 0;
            last_bps_time_ = now_ms;
        }

        VideoFrame frame{
            .data        = buf,
            .size        = (size_t)info.size,
            .timestamp_us = info.presentationTimeUs,
            .is_keyframe  = is_key
        };
        on_frame_encoded_(frame);
    }

    bool is_eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
    AMediaCodec_releaseOutputBuffer(codec_, (size_t)idx, false);

    if (is_eos) {
        LOGI("EOS received from encoder");
        running_ = false;
    }
}

int NativeEncoder::getCurrentBitrate() const {
    return current_bps_;
}

} // namespace screencast
