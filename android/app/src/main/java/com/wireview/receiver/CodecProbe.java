package com.wireview.receiver;

import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Build;
import android.util.Log;

final class CodecProbe {
    private static final String TAG = "WireViewCodec";

    private CodecProbe() {
    }

    static Result findBestAvcDecoder() {
        return findBestAvcDecoder(null);
    }

    static Result findBestAvcDecoder(String preferredName) {
        Result best = null;
        Result preferred = null;
        int listKind = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q
                ? MediaCodecList.ALL_CODECS : MediaCodecList.REGULAR_CODECS;
        for (MediaCodecInfo codec : new MediaCodecList(listKind).getCodecInfos()) {
            if (codec.isEncoder()
                    || (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && codec.isAlias())
                    || !supportsAvc(codec)) {
                continue;
            }

            try {
                MediaCodecInfo.CodecCapabilities capabilities =
                        codec.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC);
                MediaCodecInfo.VideoCapabilities video = capabilities.getVideoCapabilities();
                boolean supports1080p60 = video != null
                        && video.areSizeAndRateSupported(1920, 1080, 60.0);
                String name = codec.getName();
                boolean softwareOnly = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q
                        ? codec.isSoftwareOnly()
                        : name.startsWith("OMX.google.") || name.startsWith("c2.android.");
                Result candidate = new Result(
                        name,
                        Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && codec.isHardwareAccelerated(),
                        softwareOnly,
                        Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
                                && capabilities.isFeatureSupported(
                                        MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency),
                        supports1080p60);
                Log.i(TAG, "AVC decoder: " + candidate.summary());

                if (best == null || candidate.score() > best.score()) {
                    best = candidate;
                }
                if (name.equals(preferredName)) {
                    preferred = candidate;
                }
            } catch (IllegalArgumentException error) {
                Log.w(TAG, "Cannot inspect AVC decoder " + codec.getName(), error);
            }
        }

        if (best == null) {
            best = new Result("未找到 AVC 解码器", false, false, false, false);
        }
        if (preferredName != null && preferred == null) {
            Log.w(TAG, "Requested AVC decoder is unavailable: " + preferredName);
        } else if (preferred != null) {
            best = preferred;
        }
        Log.i(TAG, "Selected AVC decoder: " + best.summary());
        return best;
    }

    private static boolean supportsAvc(MediaCodecInfo codec) {
        for (String type : codec.getSupportedTypes()) {
            if (MediaFormat.MIMETYPE_VIDEO_AVC.equalsIgnoreCase(type)) {
                return true;
            }
        }
        return false;
    }

    static final class Result {
        final String name;
        final boolean hardwareAccelerated;
        final boolean softwareOnly;
        final boolean lowLatency;
        final boolean supports1080p60;

        Result(
                String name,
                boolean hardwareAccelerated,
                boolean softwareOnly,
                boolean lowLatency,
                boolean supports1080p60) {
            this.name = name;
            this.hardwareAccelerated = hardwareAccelerated;
            this.softwareOnly = softwareOnly;
            this.lowLatency = lowLatency;
            this.supports1080p60 = supports1080p60;
        }

        String summary() {
            return name
                    + " | " + (hardwareAccelerated ? "硬件" : (softwareOnly ? "软件" : "未知实现"))
                    + " | 标准低延迟标记=" + yesNo(lowLatency)
                    + " | 1080p60=" + yesNo(supports1080p60);
        }

        private int score() {
            int score = 0;
            if (hardwareAccelerated) {
                score += 4;
            }
            if (supports1080p60) {
                score += 2;
            }
            if (lowLatency) {
                score += 1;
            }
            // When otherwise tied, prefer the modern Codec2 hardware path. On the tested
            // Qualcomm tablet it starts output immediately, while the legacy OMX path
            // buffers roughly two seconds before the first frame.
            if (hardwareAccelerated && name.startsWith("c2.")) {
                score += 1;
            }
            if (softwareOnly) {
                score -= 8;
            }
            return score;
        }

        private static String yesNo(boolean value) {
            return value ? "是" : "否";
        }
    }
}
