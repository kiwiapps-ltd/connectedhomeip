#include "MatterBridgeAVStreamDelegate.h"
#include "MatterBridgeBackChannel.h"

#include <algorithm>
#include <cstring>
#include <lib/support/Base64.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>

namespace MatterBridge {

using namespace chip;
using namespace chip::app::Clusters::CameraAvStreamManagement;

AVStreamDelegate::Status AVStreamDelegate::VideoStreamAllocate(const VideoStreamStruct & allocateArgs, uint16_t & outStreamID)
{
    outStreamID = mNextVideoStreamID++;
    VideoStreamStruct stored = allocateArgs;
    stored.videoStreamID     = outStreamID;
    stored.referenceCount    = 0;
    mAllocatedVideoStreams.push_back(stored);
    ChipLogProgress(NotSpecified, "AVStream[ep=%u]: VideoStream %u allocated (codec=%u %ux%u..%ux%u)", mEndpointId,
                    outStreamID, static_cast<unsigned>(stored.videoCodec),
                    stored.minResolution.width, stored.minResolution.height,
                    stored.maxResolution.width, stored.maxResolution.height);
    return Status::Success;
}

AVStreamDelegate::Status AVStreamDelegate::VideoStreamDeallocate(uint16_t streamID)
{
    auto it = std::find_if(mAllocatedVideoStreams.begin(), mAllocatedVideoStreams.end(),
                           [streamID](const VideoStreamStruct & s) { return s.videoStreamID == streamID; });
    if (it == mAllocatedVideoStreams.end()) return Status::NotFound;
    mAllocatedVideoStreams.erase(it);
    ChipLogProgress(NotSpecified, "AVStream[ep=%u]: VideoStream %u deallocated", mEndpointId, streamID);
    return Status::Success;
}

AVStreamDelegate::Status AVStreamDelegate::SnapshotStreamAllocate(const SnapshotStreamAllocateArgs & args, uint16_t & outID)
{
    outID = mNextSnapshotStreamID++;
    SnapshotStreamStruct s;
    s.snapshotStreamID = outID;
    s.imageCodec       = args.imageCodec;
    s.frameRate        = args.maxFrameRate;
    s.minResolution    = args.minResolution;
    s.maxResolution    = args.maxResolution;
    s.quality          = args.quality;
    s.referenceCount   = 0;
    mAllocatedSnapshotStreams.push_back(s);
    return Status::Success;
}

AVStreamDelegate::Status AVStreamDelegate::SnapshotStreamDeallocate(uint16_t streamID)
{
    auto it = std::find_if(mAllocatedSnapshotStreams.begin(), mAllocatedSnapshotStreams.end(),
                           [streamID](const SnapshotStreamStruct & s) { return s.snapshotStreamID == streamID; });
    if (it == mAllocatedSnapshotStreams.end()) return Status::NotFound;
    mAllocatedSnapshotStreams.erase(it);
    return Status::Success;
}

bool AVStreamDelegate::HasVideoStream(uint16_t streamID) const
{
    return std::any_of(mAllocatedVideoStreams.begin(), mAllocatedVideoStreams.end(),
                       [streamID](const VideoStreamStruct & s) { return s.videoStreamID == streamID; });
}

AVStreamDelegate::Status AVStreamDelegate::CaptureSnapshot(const chip::app::DataModel::Nullable<uint16_t> streamID,
                                                            const VideoResolutionStruct & resolution,
                                                            ImageSnapshot & outImageSnapshot)
{
    Json::Value req(Json::objectValue);
    req["Name"]     = "Snapshot";
    req["Endpoint"] = mEndpointId;
    req["Width"]    = resolution.width;
    req["Height"]   = resolution.height;
    if (!streamID.IsNull()) req["StreamId"] = streamID.Value();

    Json::Value resp;
    if (!GetBackChannel().Request(std::move(req), resp, /*timeoutMs=*/15000))
    {
        ChipLogError(NotSpecified, "Snapshot back-channel timeout/disconnect");
        return Status::Failure;
    }

    if (resp.get("Status", "error").asString() != "ok")
    {
        ChipLogError(NotSpecified, "Snapshot rejected by Swift: %s",
                     resp.get("Reason", "?").asString().c_str());
        return Status::Failure;
    }

    // Bytes come back base64-encoded. Decode into outImageSnapshot.data.
    std::string b64 = resp.get("Bytes", "").asString();
    if (b64.empty())
    {
        ChipLogError(NotSpecified, "Snapshot response missing bytes");
        return Status::Failure;
    }

    // Conservative upper bound on decoded size (3 output bytes per 4 input).
    size_t expected = (b64.size() / 4) * 3 + 4;
    outImageSnapshot.data.assign(expected, 0);
    uint16_t actual = chip::Base64Decode(b64.data(), static_cast<uint16_t>(b64.size()),
                                         outImageSnapshot.data.data());
    if (actual == 0 && !b64.empty())
    {
        ChipLogError(NotSpecified, "Snapshot bytes failed to decode");
        return Status::Failure;
    }
    outImageSnapshot.data.resize(actual);
    outImageSnapshot.imageRes  = resolution;
    outImageSnapshot.imageCodec = ImageCodecEnum::kJpeg;
    ChipLogProgress(NotSpecified, "Snapshot returned %u bytes (%ux%u)", actual,
                    resolution.width, resolution.height);
    return Status::Success;
}

} // namespace MatterBridge
