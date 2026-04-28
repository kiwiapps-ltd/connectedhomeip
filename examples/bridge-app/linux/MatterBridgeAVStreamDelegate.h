/*
 * MatterBridge: minimal CameraAVStreamMgmtDelegate.
 *
 * Honest scope: this stub satisfies the cluster server's pure-virtual
 * interface so we can construct + register the server without UB. Most
 * methods return Success with empty allocations; CaptureSnapshot is the
 * one method that actually does work — it serializes the request and asks
 * Swift over the back-channel for real JPEG bytes.
 *
 * Real stream allocation / modify / dealloc come in a follow-up — they
 * need persistence + state synchronization with Swift's libwebrtc transcoder
 * pipeline and aren't worth half-implementing.
 */
#pragma once

#include <app/clusters/camera-av-stream-management-server/CameraAVStreamManagementCluster.h>

namespace MatterBridge {

class AVStreamDelegate : public chip::app::Clusters::CameraAvStreamManagement::CameraAVStreamManagementDelegate
{
public:
    AVStreamDelegate() = default;
    ~AVStreamDelegate() override = default;

    using Status = chip::Protocols::InteractionModel::Status;
    using ImageSnapshot = chip::app::Clusters::CameraAvStreamManagement::ImageSnapshot;
    using StreamAllocationAction = chip::app::Clusters::CameraAvStreamManagement::StreamAllocationAction;
    using VideoStreamStruct = chip::app::Clusters::CameraAvStreamManagement::Structs::VideoStreamStruct::Type;
    using AudioStreamStruct = chip::app::Clusters::CameraAvStreamManagement::Structs::AudioStreamStruct::Type;
    using SnapshotStreamStruct = chip::app::Clusters::CameraAvStreamManagement::Structs::SnapshotStreamStruct::Type;
    using VideoResolutionStruct = chip::app::Clusters::CameraAvStreamManagement::Structs::VideoResolutionStruct::Type;

    /// Allocate a video stream. Stores the request locally so WebRTCProvider
    /// can find the stream by ID. The actual stream production lives in
    /// Swift — this method just bookkeeps the (id, args) pair.
    Status VideoStreamAllocate(const VideoStreamStruct & allocateArgs, uint16_t & outStreamID) override;
    void OnVideoStreamAllocated(const VideoStreamStruct &, StreamAllocationAction) override {}
    Status VideoStreamModify(uint16_t, chip::Optional<bool>, chip::Optional<bool>) override { return Status::Success; }
    Status VideoStreamDeallocate(uint16_t streamID) override;
    Status AudioStreamAllocate(const AudioStreamStruct &, uint16_t & outID) override
    {
        outID = 0;
        return Status::UnsupportedAccess; // audio path is post-v1
    }
    Status AudioStreamDeallocate(uint16_t) override { return Status::UnsupportedAccess; }
    Status SnapshotStreamAllocate(const SnapshotStreamAllocateArgs & args, uint16_t & outID) override;
    Status SnapshotStreamModify(uint16_t, chip::Optional<bool>, chip::Optional<bool>) override { return Status::Success; }
    Status SnapshotStreamDeallocate(uint16_t streamID) override;
    void OnStreamUsagePrioritiesChanged() override {}
    void OnAttributeChanged(chip::AttributeId) override {}

    /// CaptureSnapshot — calls back-channel for real JPEG bytes (Swift owns
    /// the VideoToolbox decode path).
    Status CaptureSnapshot(const chip::app::DataModel::Nullable<uint16_t> streamID,
                           const VideoResolutionStruct & resolution,
                           ImageSnapshot & outImageSnapshot) override;

    CHIP_ERROR PersistentAttributesLoadedCallback() override { return CHIP_NO_ERROR; }
    const std::vector<VideoStreamStruct> & GetAllocatedVideoStreams() const override { return mAllocatedVideoStreams; }
    const std::vector<AudioStreamStruct> & GetAllocatedAudioStreams() const override { return mEmptyAudioStreams; }

    /// Used by WebRTCProviderDelegate to validate stream IDs.
    ///
    /// SmartThings's `WebRTC.ProvideOffer` arrives with a `VideoStreamID`
    /// field whose value comes from a previous read of the
    /// `AllocatedVideoStreams` attribute (which the cluster server
    /// persists across reboots). Our in-memory `mAllocatedVideoStreams`
    /// starts empty on each boot, so the strict `HasVideoStream` check
    /// would reject every ProvideOffer with `DYNAMIC_CONSTRAINT_ERROR`.
    ///
    /// A bridged camera always exposes exactly one logical live stream
    /// per slot. We don't multiplex stream IDs — every ProvideOffer
    /// targets the same camera regardless of the requested ID. Always
    /// answering "yes, that ID is valid" lets the negotiation proceed
    /// to the SDP/ICE phase where the real frame pump takes over.
    bool HasVideoStream(uint16_t /*streamID*/) const { return true; }
    bool HasAnyVideoStream() const { return true; }

    /// Tag this delegate with its endpoint id at construction so the snapshot
    /// back-channel request can carry it (Swift needs to know which camera).
    void SetEndpointId(chip::EndpointId ep) { mEndpointId = ep; }

private:
    std::vector<VideoStreamStruct>    mAllocatedVideoStreams;
    std::vector<AudioStreamStruct>    mEmptyAudioStreams;
    std::vector<SnapshotStreamStruct> mAllocatedSnapshotStreams;
    uint16_t                          mNextVideoStreamID    = 1;
    uint16_t                          mNextSnapshotStreamID = 1;
    chip::EndpointId                  mEndpointId           = chip::kInvalidEndpointId;
};

} // namespace MatterBridge
