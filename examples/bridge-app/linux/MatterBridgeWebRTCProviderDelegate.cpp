#include "MatterBridgeWebRTCProviderDelegate.h"

#include "MatterBridgeAVStreamDelegate.h"
#include "MatterBridgeBackChannel.h"
#include "MatterBridgeCameraSlots.h"

#include <app/InteractionModelEngine.h>
#include <app/server/Server.h>
#include <controller/InvokeInteraction.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/CHIPDeviceLayer.h>

namespace MatterBridge {

using namespace chip;
using namespace chip::app;
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

} // anonymous namespace

CHIP_ERROR WebRTCProviderDelegate::HandleSolicitOffer(const OfferRequestArgs & args, WebRTCSessionStruct & outSession,
                                                      bool & outDeferredOffer)
{
    // Send everything we'd send for ProvideOffer minus the remote SDP, and
    // expect Swift to return us a freshly-generated local SDP offer that
    // we'll then ship to the controller via
    // `WebRTCTransportRequestor.Offer`. Same code path as
    // ForwardOfferLike but we extract the offer SDP to schedule the
    // outbound command.
    Json::Value req(Json::objectValue);
    req["Name"]                  = "WebRTC.SolicitOffer";
    req["Endpoint"]              = mEndpointId;
    req["SessionId"]             = args.sessionId;
    req["StreamUsage"]           = static_cast<int>(args.streamUsage);
    req["PeerNodeId"]            = static_cast<Json::UInt64>(args.peerNodeId);
    req["FabricIndex"]           = args.fabricIndex;
    req["OriginatingEndpointId"] = args.originatingEndpointId;
    {
        auto extId = FindExtIdByEndpoint(mEndpointId);
        if (!extId.empty()) req["CameraId"] = extId;
    }
    if (args.videoStreamId.HasValue() && !args.videoStreamId.Value().IsNull())
        req["VideoStreamId"] = args.videoStreamId.Value().Value();
    if (args.audioStreamId.HasValue() && !args.audioStreamId.Value().IsNull())
        req["AudioStreamId"] = args.audioStreamId.Value().Value();
    EncodeICEServers(args.iceServers, req["IceServers"]);
    if (args.iceTransportPolicy.HasValue()) req["IceTransportPolicy"] = args.iceTransportPolicy.Value();

    Json::Value resp;
    if (!GetBackChannel().Request(std::move(req), resp, /*timeoutMs=*/30000))
    {
        ChipLogError(NotSpecified, "WebRTC SolicitOffer: back-channel timeout");
        return CHIP_ERROR_TIMEOUT;
    }
    if (resp.get("Status", "error").asString() != "ok")
    {
        ChipLogError(NotSpecified, "WebRTC SolicitOffer: rejected by Swift: %s",
                     resp.get("Reason", "?").asString().c_str());
        return CHIP_ERROR_INTERNAL;
    }

    const uint16_t assignedSessionId = static_cast<uint16_t>(resp.get("SessionId", args.sessionId).asUInt());
    outSession.id              = assignedSessionId;
    outSession.peerNodeID      = args.peerNodeId;
    outSession.peerEndpointID  = args.originatingEndpointId;
    outSession.streamUsage     = args.streamUsage;
    outSession.videoStreamID   = chip::app::DataModel::Nullable<uint16_t>();
    outSession.audioStreamID   = chip::app::DataModel::Nullable<uint16_t>();
    outSession.metadataEnabled = false;
    outSession.fabricIndex     = args.fabricIndex;
    outDeferredOffer = resp.get("Deferred", true).asBool();

    const std::string sdpOffer = resp.get("SDP", "").asString();
    if (!sdpOffer.empty())
    {
        std::lock_guard<std::mutex> lk(mSessionsMutex);
        auto & state = mSessions[assignedSessionId];
        state.peerId                = ScopedNodeId(args.peerNodeId, args.fabricIndex);
        state.originatingEndpointId = args.originatingEndpointId;
        state.sdpAnswer             = sdpOffer; // reuse field for outbound SDP
        ScheduleSendOffer(assignedSessionId);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR WebRTCProviderDelegate::HandleProvideOffer(const ProvideOfferRequestArgs & args, WebRTCSessionStruct & outSession)
{
    // Build the same back-channel JSON request as ForwardOfferLike, but pull
    // the answer SDP out of the response so we can ship it back to the
    // controller via WebRTCTransportRequestor.Answer (the synchronous
    // ProvideOfferResponse only carries session metadata; per Matter 1.5 the
    // SDP travels in a separate inbound command).
    Json::Value req(Json::objectValue);
    req["Name"]                  = "WebRTC.ProvideOffer";
    req["Endpoint"]              = mEndpointId;
    req["SessionId"]             = args.sessionId;
    req["StreamUsage"]           = static_cast<int>(args.streamUsage);
    req["PeerNodeId"]            = static_cast<Json::UInt64>(args.peerNodeId);
    req["FabricIndex"]           = args.fabricIndex;
    req["OriginatingEndpointId"] = args.originatingEndpointId;
    {
        auto extId = FindExtIdByEndpoint(mEndpointId);
        if (!extId.empty()) req["CameraId"] = extId;
    }
    if (args.videoStreamId.HasValue() && !args.videoStreamId.Value().IsNull())
        req["VideoStreamId"] = args.videoStreamId.Value().Value();
    if (args.audioStreamId.HasValue() && !args.audioStreamId.Value().IsNull())
        req["AudioStreamId"] = args.audioStreamId.Value().Value();
    req["SDP"] = args.sdp;
    EncodeICEServers(args.iceServers, req["IceServers"]);
    if (args.iceTransportPolicy.HasValue()) req["IceTransportPolicy"] = args.iceTransportPolicy.Value();

    Json::Value resp;
    if (!GetBackChannel().Request(std::move(req), resp, /*timeoutMs=*/30000))
    {
        ChipLogError(NotSpecified, "WebRTC ProvideOffer: back-channel timeout");
        return CHIP_ERROR_TIMEOUT;
    }
    if (resp.get("Status", "error").asString() != "ok")
    {
        ChipLogError(NotSpecified, "WebRTC ProvideOffer: rejected by Swift: %s",
                     resp.get("Reason", "?").asString().c_str());
        return CHIP_ERROR_INTERNAL;
    }

    const uint16_t assignedSessionId = static_cast<uint16_t>(resp.get("SessionId", args.sessionId).asUInt());
    outSession.id            = assignedSessionId;
    outSession.peerNodeID    = args.peerNodeId;
    outSession.peerEndpointID = args.originatingEndpointId;
    outSession.streamUsage   = args.streamUsage;
    outSession.videoStreamID = chip::app::DataModel::Nullable<uint16_t>();
    outSession.audioStreamID = chip::app::DataModel::Nullable<uint16_t>();
    if (resp.isMember("VideoStreamId") && resp["VideoStreamId"].isUInt())
        outSession.videoStreamID.SetNonNull(static_cast<uint16_t>(resp["VideoStreamId"].asUInt()));
    if (resp.isMember("AudioStreamId") && resp["AudioStreamId"].isUInt())
        outSession.audioStreamID.SetNonNull(static_cast<uint16_t>(resp["AudioStreamId"].asUInt()));
    outSession.metadataEnabled = false;
    outSession.fabricIndex     = args.fabricIndex;

    const std::string sdpAnswer = resp.get("SDP", "").asString();
    if (sdpAnswer.empty())
    {
        ChipLogError(NotSpecified, "WebRTC ProvideOffer: Swift returned empty SDP — Answer command will not be sent");
        return CHIP_NO_ERROR;
    }

    {
        std::lock_guard<std::mutex> lk(mSessionsMutex);
        auto & state = mSessions[assignedSessionId];
        state.peerId                = ScopedNodeId(args.peerNodeId, args.fabricIndex);
        state.originatingEndpointId = args.originatingEndpointId;
        state.sdpAnswer             = sdpAnswer;
    }

    ScheduleSendAnswer(assignedSessionId);
    return CHIP_NO_ERROR;
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

// ---------------------------------------------------------------------------
//  Outbound WebRTCTransportRequestor.{Answer,ICECandidates,End}
// ---------------------------------------------------------------------------

WebRTCProviderDelegate::WebRTCProviderDelegate()
    : mOnConnectedCallback(WebRTCProviderDelegate::OnDeviceConnected, this),
      mOnConnectionFailureCallback(WebRTCProviderDelegate::OnDeviceConnectionFailure, this)
{}

WebRTCProviderDelegate::PendingSession * WebRTCProviderDelegate::GetSession(uint16_t sessionId)
{
    auto it = mSessions.find(sessionId);
    return it == mSessions.end() ? nullptr : &it->second;
}

void WebRTCProviderDelegate::ScheduleSendAnswer(uint16_t sessionId)
{
    DeviceLayer::SystemLayer().ScheduleLambda([this, sessionId]() {
        ScopedNodeId peerId;
        {
            std::lock_guard<std::mutex> lk(mSessionsMutex);
            auto * s = GetSession(sessionId);
            if (!s)
            {
                ChipLogError(NotSpecified, "ScheduleSendAnswer: no session %u", sessionId);
                return;
            }
            s->pendingCommand = PendingSession::Pending::kAnswer;
            peerId            = s->peerId;
            mPeerToSessionId[peerId] = sessionId;
        }
        ChipLogProgress(NotSpecified, "WebRTC: establishing CASE to send Answer for session %u", sessionId);
        auto * mgr = Server::GetInstance().GetCASESessionManager();
        VerifyOrReturn(mgr != nullptr, ChipLogError(NotSpecified, "ScheduleSendAnswer: no CASE manager"));
        mgr->FindOrEstablishSession(peerId, &mOnConnectedCallback, &mOnConnectionFailureCallback,
                                     TransportPayloadCapability::kLargePayload);
    });
}

void WebRTCProviderDelegate::ScheduleSendOffer(uint16_t sessionId)
{
    DeviceLayer::SystemLayer().ScheduleLambda([this, sessionId]() {
        ScopedNodeId peerId;
        {
            std::lock_guard<std::mutex> lk(mSessionsMutex);
            auto * s = GetSession(sessionId);
            if (!s)
            {
                ChipLogError(NotSpecified, "ScheduleSendOffer: no session %u", sessionId);
                return;
            }
            s->pendingCommand = PendingSession::Pending::kOffer;
            peerId            = s->peerId;
            mPeerToSessionId[peerId] = sessionId;
        }
        ChipLogProgress(NotSpecified, "WebRTC: establishing CASE to send Offer for session %u", sessionId);
        auto * mgr = Server::GetInstance().GetCASESessionManager();
        VerifyOrReturn(mgr != nullptr, ChipLogError(NotSpecified, "ScheduleSendOffer: no CASE manager"));
        mgr->FindOrEstablishSession(peerId, &mOnConnectedCallback, &mOnConnectionFailureCallback,
                                     TransportPayloadCapability::kLargePayload);
    });
}

void WebRTCProviderDelegate::ScheduleSendIceCandidates(uint16_t sessionId)
{
    DeviceLayer::SystemLayer().ScheduleLambda([this, sessionId]() {
        ScopedNodeId peerId;
        {
            std::lock_guard<std::mutex> lk(mSessionsMutex);
            auto * s = GetSession(sessionId);
            if (!s || s->pendingIce.empty())
            {
                return;
            }
            s->pendingCommand        = PendingSession::Pending::kIce;
            peerId                   = s->peerId;
            mPeerToSessionId[peerId] = sessionId;
        }
        ChipLogProgress(NotSpecified, "WebRTC: establishing CASE to send ICECandidates for session %u", sessionId);
        auto * mgr = Server::GetInstance().GetCASESessionManager();
        VerifyOrReturn(mgr != nullptr);
        mgr->FindOrEstablishSession(peerId, &mOnConnectedCallback, &mOnConnectionFailureCallback,
                                     TransportPayloadCapability::kLargePayload);
    });
}

void WebRTCProviderDelegate::EnqueueLocalIceCandidate(uint16_t sessionId, const std::string & candidate,
                                                      const std::string & sdpMid, int sdpMLineIndex)
{
    // This method is invoked from the back-channel reader thread (a non-chip
    // thread). All chip-stack API calls (timers, CASE manager, command
    // sender) must happen on the chip stack thread, so we just append the
    // candidate to our session under our own mutex and post a flush task to
    // the chip stack via `ScheduleLambda` (which IS thread-safe). The flush
    // task does the actual chip work.
    {
        std::lock_guard<std::mutex> lk(mSessionsMutex);
        auto * s = GetSession(sessionId);
        if (!s) { return; }

        s->iceCandidateStrings.push_back(candidate);
        if (!sdpMid.empty()) s->iceMidStrings.push_back(sdpMid);
        else                  s->iceMidStrings.emplace_back();

        Globals::Structs::ICECandidateStruct::Type ice;
        const auto & cstr = s->iceCandidateStrings.back();
        ice.candidate = chip::CharSpan(cstr.data(), cstr.size());
        const auto & mstr = s->iceMidStrings.back();
        if (!mstr.empty()) ice.SDPMid.SetNonNull(chip::CharSpan(mstr.data(), mstr.size()));
        else               ice.SDPMid.SetNull();
        if (sdpMLineIndex >= 0) ice.SDPMLineIndex.SetNonNull(static_cast<uint16_t>(sdpMLineIndex));
        else                    ice.SDPMLineIndex.SetNull();
        s->pendingIce.push_back(ice);
    }

    // ScheduleLambda hops onto the chip stack thread before running the
    // closure. Per-call (no debounce); each candidate triggers its own
    // ScheduleSendIceCandidates, which itself ScheduleLambda's. The
    // FindOrEstablishSession reuses an existing CASE session if available
    // so the cost is small.
    DeviceLayer::SystemLayer().ScheduleLambda([this, sessionId]() {
        ScheduleSendIceCandidates(sessionId);
    });
}

void WebRTCProviderDelegate::OnDeviceConnected(void * context, Messaging::ExchangeManager & exchangeMgr,
                                                const SessionHandle & sessionHandle)
{
    auto * self = static_cast<WebRTCProviderDelegate *>(context);
    VerifyOrReturn(self != nullptr);

    ScopedNodeId peer = sessionHandle->GetPeer();
    uint16_t sessionId = 0;
    PendingSession::Pending cmd = PendingSession::Pending::kNone;
    {
        std::lock_guard<std::mutex> lk(self->mSessionsMutex);
        auto it = self->mPeerToSessionId.find(peer);
        if (it == self->mPeerToSessionId.end())
        {
            ChipLogError(NotSpecified, "OnDeviceConnected: no pending session for peer fab=%u node=" ChipLogFormatX64,
                         peer.GetFabricIndex(), ChipLogValueX64(peer.GetNodeId()));
            return;
        }
        sessionId = it->second;
        auto * s  = self->GetSession(sessionId);
        if (s) cmd = s->pendingCommand;
    }

    switch (cmd)
    {
    case PendingSession::Pending::kAnswer:
        (void) self->SendAnswerCommand(exchangeMgr, sessionHandle, sessionId);
        break;
    case PendingSession::Pending::kOffer:
        (void) self->SendOfferCommand(exchangeMgr, sessionHandle, sessionId);
        break;
    case PendingSession::Pending::kIce:
        (void) self->SendICECandidatesCommand(exchangeMgr, sessionHandle, sessionId);
        break;
    default:
        ChipLogError(NotSpecified, "OnDeviceConnected: no pending command for session %u", sessionId);
        break;
    }
}

void WebRTCProviderDelegate::OnDeviceConnectionFailure(void * context, const ScopedNodeId & peerId, CHIP_ERROR error)
{
    ChipLogError(NotSpecified,
                 "WebRTC: CASE session establish failed for peer fab=%u node=" ChipLogFormatX64 ": %" CHIP_ERROR_FORMAT,
                 peerId.GetFabricIndex(), ChipLogValueX64(peerId.GetNodeId()), error.Format());
}

CHIP_ERROR WebRTCProviderDelegate::SendAnswerCommand(Messaging::ExchangeManager & exchangeMgr,
                                                     const SessionHandle & sessionHandle, uint16_t sessionId)
{
    chip::EndpointId endpointId = chip::kRootEndpointId;
    std::string      sdp;
    {
        std::lock_guard<std::mutex> lk(mSessionsMutex);
        auto * s = GetSession(sessionId);
        if (!s) return CHIP_ERROR_INTERNAL;
        endpointId = s->originatingEndpointId;
        sdp        = s->sdpAnswer;
        s->pendingCommand = PendingSession::Pending::kNone;
    }

    WebRTCTransportRequestor::Commands::Answer::Type cmd;
    cmd.webRTCSessionID = sessionId;
    cmd.sdp             = chip::CharSpan(sdp.data(), sdp.size());

    auto onSuccess = [sessionId](const ConcreteCommandPath &, const StatusIB &, const auto &) {
        ChipLogProgress(NotSpecified, "WebRTC: Answer command succeeded for session %u", sessionId);
    };
    auto onFailure = [sessionId](CHIP_ERROR err) {
        ChipLogError(NotSpecified, "WebRTC: Answer command failed for session %u: %" CHIP_ERROR_FORMAT,
                     sessionId, err.Format());
    };

    return Controller::InvokeCommandRequest(&exchangeMgr, sessionHandle, endpointId, cmd,
                                             onSuccess, onFailure,
                                             /*timedInvokeTimeoutMs=*/NullOptional,
                                             /*responseTimeout=*/NullOptional,
                                             /*outCancelFn=*/nullptr,
                                             /*allowLargePayload=*/true);
}

CHIP_ERROR WebRTCProviderDelegate::SendOfferCommand(Messaging::ExchangeManager & exchangeMgr,
                                                    const SessionHandle & sessionHandle, uint16_t sessionId)
{
    chip::EndpointId endpointId = chip::kRootEndpointId;
    std::string      sdp;
    {
        std::lock_guard<std::mutex> lk(mSessionsMutex);
        auto * s = GetSession(sessionId);
        if (!s) return CHIP_ERROR_INTERNAL;
        endpointId = s->originatingEndpointId;
        sdp        = s->sdpAnswer;
        s->pendingCommand = PendingSession::Pending::kNone;
    }

    WebRTCTransportRequestor::Commands::Offer::Type cmd;
    cmd.webRTCSessionID = sessionId;
    cmd.sdp             = chip::CharSpan(sdp.data(), sdp.size());

    auto onSuccess = [sessionId](const ConcreteCommandPath &, const StatusIB &, const auto &) {
        ChipLogProgress(NotSpecified, "WebRTC: Offer command succeeded for session %u", sessionId);
    };
    auto onFailure = [sessionId](CHIP_ERROR err) {
        ChipLogError(NotSpecified, "WebRTC: Offer command failed for session %u: %" CHIP_ERROR_FORMAT,
                     sessionId, err.Format());
    };

    return Controller::InvokeCommandRequest(&exchangeMgr, sessionHandle, endpointId, cmd,
                                             onSuccess, onFailure,
                                             /*timedInvokeTimeoutMs=*/NullOptional,
                                             /*responseTimeout=*/NullOptional,
                                             /*outCancelFn=*/nullptr,
                                             /*allowLargePayload=*/true);
}

CHIP_ERROR WebRTCProviderDelegate::SendICECandidatesCommand(Messaging::ExchangeManager & exchangeMgr,
                                                            const SessionHandle & sessionHandle, uint16_t sessionId)
{
    chip::EndpointId endpointId = chip::kRootEndpointId;
    std::vector<Globals::Structs::ICECandidateStruct::Type> ice;
    {
        std::lock_guard<std::mutex> lk(mSessionsMutex);
        auto * s = GetSession(sessionId);
        if (!s || s->pendingIce.empty()) return CHIP_ERROR_INCORRECT_STATE;
        endpointId = s->originatingEndpointId;
        ice        = s->pendingIce;            // copy: spans still reference the strings the session owns
        s->pendingCommand = PendingSession::Pending::kNone;
        // Note: we keep iceCandidateStrings/iceMidStrings around because the
        // CharSpans inside `ice` reference them. They get cleared on session End.
        s->pendingIce.clear();
    }

    WebRTCTransportRequestor::Commands::ICECandidates::Type cmd;
    cmd.webRTCSessionID = sessionId;
    cmd.ICECandidates   = chip::app::DataModel::List<const Globals::Structs::ICECandidateStruct::Type>(
        ice.data(), ice.size());

    auto onSuccess = [sessionId](const ConcreteCommandPath &, const StatusIB &, const auto &) {
        ChipLogProgress(NotSpecified, "WebRTC: ICECandidates command succeeded for session %u", sessionId);
    };
    auto onFailure = [sessionId](CHIP_ERROR err) {
        ChipLogError(NotSpecified, "WebRTC: ICECandidates command failed for session %u: %" CHIP_ERROR_FORMAT,
                     sessionId, err.Format());
    };

    return Controller::InvokeCommandRequest(&exchangeMgr, sessionHandle, endpointId, cmd,
                                             onSuccess, onFailure,
                                             /*timedInvokeTimeoutMs=*/NullOptional,
                                             /*responseTimeout=*/NullOptional,
                                             /*outCancelFn=*/nullptr,
                                             /*allowLargePayload=*/true);
}

} // namespace MatterBridge
