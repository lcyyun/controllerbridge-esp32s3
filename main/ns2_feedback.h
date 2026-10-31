#pragma once
#include "ns2_input.h"

namespace ns2 {
struct FeedbackStamp {
    uint32_t source_epoch=0;
    uint64_t received_us=0;
};
inline bool feedback_matches_source(const FeedbackStamp &stamp,const InputSnapshot &input,uint64_t now_us) {
    return input.valid && stamp.source_epoch==input.source_epoch && now_us>=stamp.received_us &&
           now_us-stamp.received_us<=static_cast<uint64_t>(kInputStaleMs)*1000;
}
}
