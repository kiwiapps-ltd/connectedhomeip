/*
 * MatterBridge: WebRTCTransportProvider delegate that forwards every SDP /
 * ICE message to Swift over the back-channel socket. The actual
 * RTCPeerConnection lives in Swift (using stasel/WebRTC), with frames
 * sourced from the existing RTSP ingest.
 *
 * The delegate does *not* maintain any state itself — Swift owns the session
 * lifecycle. This stub answers the data-model surface (HasAllocatedVideoStreams,
 * ValidateVideoStreamID, etc.) by consulting the paired AVStream delegate.
 */
#pragma once

#include <app/clusters/webrtc-transport-provider-server/webrtc-transport-provider-server.h>

namespace MatterBridge {

class AVStreamDelegate;

class WebRTCProviderDelegate : public chip::app::Clusters::WebRTCTransportProvider::Delegate
{
public:
    using StreamUsageEnum     = chip::app::Clusters::Globals::StreamUsageEnum;
    using WebRTCSessionStruct = chip::app::Clusters::Globals::Structs::WebRTCSessionStruct::Type;
    using WebRTCEndReasonEnum = chip::app::Clusters::Globals::WebRTCEndReasonEnum;
    using ICECandidateStruct  = chip::app::Clusters::Globals::Structs::ICECandidateStruct::Type;
    using ICEServerDecodableStruct = chip::app::Clusters::Globals::Structs::ICEServerStruct::DecodableType;

    void SetAVStreamDelegate(AVStreamDelegate * av) { mAvStream = av; }
    void SetEndpointId(chip::EndpointId ep)         { mEndpointId = ep; }

    CHIP_ERROR HandleSolicitOffer(const OfferRequestArgs & args, WebRTCSessionStruct & outSession,
                                  bool & outDeferredOffer) override;
    CHIP_ERROR HandleProvideOffer(const ProvideOfferRequestArgs & args, WebRTCSessionStruct & outSession) override;
    CHIP_ERROR HandleProvideAnswer(uint16_t sessionId, const std::string & sdpAnswer) override;
    CHIP_ERROR HandleProvideICECandidates(uint16_t sessionId, const std::vector<ICECandidateStruct> & candidates) override;
    CHIP_ERROR HandleEndSession(uint16_t sessionId, WebRTCEndReasonEnum reasonCode,
                                chip::app::DataModel::Nullable<uint16_t> videoStreamID,
                                chip::app::DataModel::Nullable<uint16_t> audioStreamID) override;

    CHIP_ERROR ValidateStreamUsage(StreamUsageEnum streamUsage,
                                   chip::Optional<chip::app::DataModel::Nullable<uint16_t>> & videoStreamId,
                                   chip::Optional<chip::app::DataModel::Nullable<uint16_t>> & audioStreamId) override;
    CHIP_ERROR ValidateVideoStreamID(uint16_t videoStreamId) override;
    CHIP_ERROR ValidateAudioStreamID(uint16_t /*audioStreamId*/) override { return CHIP_ERROR_NOT_FOUND; }
    CHIP_ERROR IsStreamUsageSupported(StreamUsageEnum streamUsage) override;
    CHIP_ERROR IsHardPrivacyModeActive(bool & isActive) override
    {
        isActive = false;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR IsSoftRecordingPrivacyModeActive(bool & isActive) override
    {
        isActive = false;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR IsSoftLivestreamPrivacyModeActive(bool & isActive) override
    {
        isActive = false;
        return CHIP_NO_ERROR;
    }
    bool HasAllocatedVideoStreams() override;
    bool HasAllocatedAudioStreams() override { return false; }
    CHIP_ERROR ValidateSFrameConfig(uint16_t /*cipherSuite*/, size_t /*baseKeyLength*/) override { return CHIP_NO_ERROR; }
    CHIP_ERROR IsUTCTimeNull(bool & isNull) override
    {
        isNull = false;
        return CHIP_NO_ERROR;
    }

private:
    AVStreamDelegate * mAvStream  = nullptr;
    chip::EndpointId   mEndpointId = chip::kInvalidEndpointId;
};

} // namespace MatterBridge
