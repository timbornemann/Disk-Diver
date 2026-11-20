#pragma once

#include <QByteArrayView>

namespace diver {

enum class Frame { More, Invalid, Preview, Live, VerifiedPreview };

inline Frame frameHeader(QByteArrayView bytes, bool hasPreview, bool hasLive) {
    if (bytes.size() < 5) return Frame::More;
    if (hasLive || bytes.first(4) != QByteArrayView("DDV1")) return Frame::Invalid;
    switch (bytes[4]) {
    case 6: return hasPreview ? Frame::Invalid : Frame::Preview;
    case 7: return Frame::Live;
    case 8:
        if (!hasPreview) return Frame::Invalid;
        if (bytes.size() < 6) return Frame::More;
        return bytes[5] == 5 ? Frame::VerifiedPreview : Frame::Invalid;
    default: return Frame::Invalid;
    }
}

}
