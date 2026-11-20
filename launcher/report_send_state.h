// What the Bug Report page shows about the report being sent: derived from what is in flight and the last receipt.
#pragma once

#include "incident_client.h"

namespace Sotb {

enum class SendState { kIdle, kSending, kUploadingDump, kSent, kSentDumpFailed, kFailed };

struct SendFacts {
    bool delivering = false;
    bool uploading_dump = false;  // the report was filed and its dump follows in the same delivery
    bool retrying_dump = false;
    const ReportReceipt* result = nullptr;
};

inline SendState ClassifySend(const SendFacts& facts) {
    if (facts.retrying_dump || (facts.delivering && facts.uploading_dump)) return SendState::kUploadingDump;
    if (facts.delivering) return SendState::kSending;
    if (!facts.result) return SendState::kIdle;
    if (facts.result->outcome != ReportOutcome::kSent) return SendState::kFailed;
    return facts.result->dump == DumpUpload::kFailed ? SendState::kSentDumpFailed : SendState::kSent;
}

inline bool SendBusy(SendState state) { return state == SendState::kSending || state == SendState::kUploadingDump; }

inline bool CanSend(SendState state, bool service_configured) { return service_configured && !SendBusy(state); }

// The dump goes again under the Report ID the service already gave, so the report itself is never filed twice.
inline bool CanRetryDump(SendState state, bool dump_kept, bool has_credential) {
    return state == SendState::kSentDumpFailed && dump_kept && has_credential;
}

} // namespace Sotb
