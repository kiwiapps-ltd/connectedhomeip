#include "MatterBridgeWebRTCProviderDelegate.h"

#include "MatterBridgeAVStreamDelegate.h"
#include "MatterBridgeBackChannel.h"

#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>

namespace MatterBridge {

using namespace chip;
using namespace chip::app::Clusters;

namespace {

void EncodeICEServers(const chip::Optional<std::vector<WebRTCProviderDelegate::ICEServerDecodableStruct>> & iceServers,
                      Json::Value & out)
{
    out = Json::arrayValue;
    if (!iceServers.HasValue()) return;
    for (const auto & srv : iceServers.Value())
    {
        Json::Value entry(Json::objectValue);
        Json::Value urls(Json::arrayValue);
        auto urlsIter = srv.URLs.begin();
        while (urlsIter.Next())
        {
            const auto & u = urlsIter.GetValue();
            urls.append(std::string(u.data(), u.size()));
        }
        entry["urls"] = urls;
        if (srv.username.HasValue())
        {
            const auto & user = srv.username.Value();
            entry["username"] = std::string(user.data(), user.size());
        }
        if (srv.credential.HasValue())
        {
            const auto & cred = srv.credential.Value();
            entry["credential"] = std::string(cred.data(), cred.size());
        }
        out.append(entry);
    }
}

CHIP_ERROR ForwardOfferLike(const char * cmd, EndpointId ep,
                            const WebRTCProviderDelegate::OfferRequestArgs & args,
                            const std::string * sdp, // null for SolicitOffer, non-null for ProvideOffer
                            WebRTCProviderDelegate::WebRTCSessionStruct & outSession,
                            bool * outDeferred)
{
    Json::Value req(Json::objectValue);
    req["Name"]                  = cmd;
    req["Endpoint"]              = ep;
    req["SessionId"]             = args.sessionId;
    req["StreamUsage"]           = static_cast<int>(args.streamUsage);
    req["PeerNodeId"]            = static_cast<Json::UInt64>(args.peerNodeId);
    req["FabricIndex"]           = args.fabricIndex;
    req["OriginatingEndpointId"] = args.originatingEndpointId;

    if (args.videoStreamId.HasValue() && !args.videoStreamId.Value().IsNull())
        req["VideoStreamId"] = args.videoStreamId.Value().Value();
    if (args.audioStreamId.HasValue() && !args.audioStreamId.Value().IsNull())
        req["AudioStreamId"] = args.audioStreamId.Value().Value();
    if (sdp) req["SDP"] = *sdp;
    EncodeICEServers(args.iceServers, req["IceServers"]);
    if (args.iceTransportPolicy.HasValue()) req["IceTransportPolicy"] = args.iceTransportPolicy.Value();

    Json::Value resp;
    if (!GetBackChannel().Request(std::move(req), resp, /*timeoutMs=*/15000))
    {
        ChipLogError(NotSpecified, "WebRTC %s: back-channel timeout", cmd);
        return CHIP_ERROR_TIMEOUT;
    }
    if (resp.get("Status", "error").asString() != "ok")
    {
        ChipLogError(NotSpecified, "WebRTC %s: rejected by Swift: %s", cmd,
                     resp.get("Reason", "?").asString().c_str());
        return CHIP_ERROR_INTERNAL;
    }

    outSession.id          = static_cast<uint16_t>(resp.get("SessionId", args.sessionId).asUInt());
    outSession.peerNodeID  = args.peerNodeId;
    outSession.peerEndpointID = args.originatingEndpointId;
    outSession.streamUsage = args.streamUsage;
    outSession.videoStreamID = chip::app::DataModel::Nullable<uint16_t>();
    outSession.audioStreamID = chip::app::DataModel::Nullable<uint16_t>();
    if (resp.isMember("VideoStreamId") && resp["VideoStreamId"].isUInt())
        outSession.videoStreamID.SetNonNull(static_cast<uint16_t>(resp["VideoStreamId"].asUInt()));
    if (resp.isMember("AudioStreamId") && resp["AudioStreamId"].isUInt())
        outSession.audioStreamID.SetNonNull(static_cast<uint16_t>(resp["AudioStreamId"].asUInt()));
    outSession.metadataEnabled = false;
    outSession.fabricIndex     = args.fabricIndex;

    if (outDeferred) *outDeferred = resp.get("Deferred", false).asBool();
    return CHIP_NO_ERROR;
}

} // anonymous namespace

CHIP_ERROR WebRTCProviderDelegate::HandleSolicitOffer(const OfferRequestArgs & args, WebRTCSessionStruct & outSession,
                                                      bool & outDeferredOffer)
{
    return ForwardOfferLike("WebRTC.SolicitOffer", mEndpointId, args, /*sdp=*/nullptr, outSession, &outDeferredOffer);
}

CHIP_ERROR WebRTCProviderDelegate::HandleProvideOffer(const ProvideOfferRequestArgs & args, WebRTCSessionStruct & outSession)
{
    return ForwardOfferLike("WebRTC.ProvideOffer", mEndpointId, args, &args.sdp, outSession, /*outDeferred=*/nullptr);
}

CHIP_ERROR WebRTCProviderDelegate::HandleProvideAnswer(uint16_t sessionId, const std::string & sdpAnswer)
{
    Json::Value req(Json::objectValue);
    req["Name"]      = "WebRTC.ProvideAnswer";
    req["Endpoint"]  = mEndpointId;
    req["SessionId"] = sessionId;
    req["SDP"]       = sdpAnswer;
    Json::Value resp;
    if (!GetBackChannel().Request(std::move(req), resp, 5000)) return CHIP_ERROR_TIMEOUT;
    return resp.get("Status", "error").asString() == "ok" ? CHIP_NO_ERROR : CHIP_ERROR_INTERNAL;
}

CHIP_ERROR WebRTCProviderDelegate::HandleProvideICECandidates(uint16_t sessionId,
                                                              const std::vector<ICECandidateStruct> & candidates)
{
    Json::Value req(Json::objectValue);
    req["Name"]      = "WebRTC.ProvideICECandidates";
    req["Endpoint"]  = mEndpointId;
    req["SessionId"] = sessionId;
    Json::Value list(Json::arrayValue);
    for (const auto & c : candidates)
    {
        Json::Value entry(Json::objectValue);
        entry["candidate"] = std::string(c.candidate.data(), c.candidate.size());
        if (!c.SDPMid.IsNull())
        {
            const auto & m = c.SDPMid.Value();
            entry["sdpMid"] = std::string(m.data(), m.size());
        }
        if (!c.SDPMLineIndex.IsNull())
            entry["sdpMLineIndex"] = c.SDPMLineIndex.Value();
        list.append(entry);
    }
    req["Candidates"] = list;
    Json::Value resp;
    if (!GetBackChannel().Request(std::move(req), resp, 5000)) return CHIP_ERROR_TIMEOUT;
    return resp.get("Status", "error").asString() == "ok" ? CHIP_NO_ERROR : CHIP_ERROR_INTERNAL;
}

CHIP_ERROR WebRTCProviderDelegate::HandleEndSession(uint16_t sessionId, WebRTCEndReasonEnum reasonCode,
                                                    chip::app::DataModel::Nullable<uint16_t>,
                                                    chip::app::DataModel::Nullable<uint16_t>)
{
    Json::Value req(Json::objectValue);
    req["Name"]      = "WebRTC.EndSession";
    req["Endpoint"]  = mEndpointId;
    req["SessionId"] = sessionId;
    req["Reason"]    = static_cast<int>(reasonCode);
    Json::Value resp;
    GetBackChannel().Request(std::move(req), resp, 2000); // fire-and-forget but with response if available
    return CHIP_NO_ERROR;
}

CHIP_ERROR WebRTCProviderDelegate::ValidateStreamUsage(
    StreamUsageEnum streamUsage,
    chip::Optional<chip::app::DataModel::Nullable<uint16_t>> & videoStreamId,
    chip::Optional<chip::app::DataModel::Nullable<uint16_t>> & audioStreamId)
{
    if (streamUsage != Globals::StreamUsageEnum::kLiveView && streamUsage != Globals::StreamUsageEnum::kRecording)
        return CHIP_ERROR_NOT_FOUND;
    if (mAvStream == nullptr || !mAvStream->HasAnyVideoStream()) return CHIP_ERROR_NOT_FOUND;

    // Auto-pick the first allocated video stream when caller didn't specify.
    if (videoStreamId.HasValue() && videoStreamId.Value().IsNull())
    {
        videoStreamId.Value().SetNonNull(mAvStream->GetAllocatedVideoStreams().front().videoStreamID);
    }
    audioStreamId.ClearValue();
    return CHIP_NO_ERROR;
}

CHIP_ERROR WebRTCProviderDelegate::ValidateVideoStreamID(uint16_t videoStreamId)
{
    if (mAvStream && mAvStream->HasVideoStream(videoStreamId)) return CHIP_NO_ERROR;
    return CHIP_ERROR_NOT_FOUND;
}

CHIP_ERROR WebRTCProviderDelegate::IsStreamUsageSupported(StreamUsageEnum streamUsage)
{
    if (streamUsage == Globals::StreamUsageEnum::kLiveView || streamUsage == Globals::StreamUsageEnum::kRecording)
        return CHIP_NO_ERROR;
    return CHIP_ERROR_NOT_FOUND;
}

bool WebRTCProviderDelegate::HasAllocatedVideoStreams()
{
    return mAvStream != nullptr && mAvStream->HasAnyVideoStream();
}

} // namespace MatterBridge
