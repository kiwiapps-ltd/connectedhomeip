#include "MatterBridgeCameraSlots.h"

#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/reporting/reporting.h>
#include <app/util/attribute-table.h>
#include <app/util/endpoint-config-api.h>
#include <clusters/BridgedDeviceBasicInformation/Attributes.h>
#include <clusters/OccupancySensing/Attributes.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>

#include <array>
#include <cstring>

namespace MatterBridge {

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;
using Status = Protocols::InteractionModel::Status;

namespace {
std::array<CameraSlot, kCameraSlotCount> gSlots;
bool                                     gSlotsInitialized = false;

// emberAfWriteAttribute for CHAR_STRING wants a Pascal-prefixed buffer
// (`[len][chars...]`), NOT a raw c_str(). Passing c_str() makes ember read
// whatever byte follows the null terminator as the length — corrupted display
// names in SmartThings. Build the prefixed buffer here.
void WriteCharStringAttribute(EndpointId ep, ClusterId cluster, AttributeId attr, const std::string & value)
{
    // Short string: 1-byte length prefix. NodeLabel/VendorName/ProductName all
    // fit (max 32 bytes per spec).
    if (value.size() > 254)
    {
        ChipLogError(NotSpecified, "WriteCharString: value too long (%zu) for ep=%u cluster=0x%04x attr=0x%04x",
                     value.size(), ep, cluster, attr);
        return;
    }
    uint8_t buffer[256];
    buffer[0] = static_cast<uint8_t>(value.size());
    std::memcpy(buffer + 1, value.data(), value.size());
    auto status = emberAfWriteAttribute(ep, cluster, attr, buffer, ZCL_CHAR_STRING_ATTRIBUTE_TYPE);
    if (status != Status::Success)
    {
        ChipLogError(NotSpecified, "WriteCharString endpoint=%u cluster=0x%04x attr=0x%04x failed: %u",
                     ep, cluster, attr, chip::to_underlying(status));
    }
}

template <typename T>
void WriteScalarAttribute(EndpointId ep, ClusterId cluster, AttributeId attr, T value, EmberAfAttributeType type)
{
    auto status = emberAfWriteAttribute(ep, cluster, attr, reinterpret_cast<uint8_t *>(&value), type);
    if (status != Status::Success)
    {
        ChipLogError(NotSpecified, "WriteScalar endpoint=%u cluster=0x%04x attr=0x%04x failed: %u",
                     ep, cluster, attr, chip::to_underlying(status));
    }
}

void PopulateBdbiDefaults(EndpointId ep, const std::string & nodeLabel)
{
    using namespace BridgedDeviceBasicInformation::Attributes;
    WriteCharStringAttribute(ep, BridgedDeviceBasicInformation::Id, NodeLabel::Id,         nodeLabel);
    WriteCharStringAttribute(ep, BridgedDeviceBasicInformation::Id, VendorName::Id,        "MatterBridge");
    WriteCharStringAttribute(ep, BridgedDeviceBasicInformation::Id, ProductName::Id,       "Bridged Camera");
    WriteCharStringAttribute(ep, BridgedDeviceBasicInformation::Id, HardwareVersionString::Id, "1.0");
    WriteCharStringAttribute(ep, BridgedDeviceBasicInformation::Id, SoftwareVersionString::Id, "0.1.0");

    uint16_t vendorId       = 0xFFF1;
    uint16_t hardwareVer    = 1;
    uint32_t softwareVer    = 1;
    bool     reachable      = true;
    WriteScalarAttribute(ep, BridgedDeviceBasicInformation::Id, VendorID::Id,        vendorId,    ZCL_VENDOR_ID_ATTRIBUTE_TYPE);
    WriteScalarAttribute(ep, BridgedDeviceBasicInformation::Id, HardwareVersion::Id, hardwareVer, ZCL_INT16U_ATTRIBUTE_TYPE);
    WriteScalarAttribute(ep, BridgedDeviceBasicInformation::Id, SoftwareVersion::Id, softwareVer, ZCL_INT32U_ATTRIBUTE_TYPE);
    WriteScalarAttribute(ep, BridgedDeviceBasicInformation::Id, Reachable::Id,       reachable,   ZCL_BOOLEAN_ATTRIBUTE_TYPE);
}

void PopulateOccupancyDefaults(EndpointId ep)
{
    using namespace OccupancySensing::Attributes;
    uint8_t occ      = 0;
    uint8_t sensorTy = 3; // PIR + ultrasonic — matches the spec stand-in
    uint8_t bitmap   = 0x01;
    uint32_t fmap    = 0;
    WriteScalarAttribute(ep, OccupancySensing::Id, Occupancy::Id,                 occ,      ZCL_BITMAP8_ATTRIBUTE_TYPE);
    WriteScalarAttribute(ep, OccupancySensing::Id, OccupancySensorType::Id,       sensorTy, ZCL_ENUM8_ATTRIBUTE_TYPE);
    WriteScalarAttribute(ep, OccupancySensing::Id, OccupancySensorTypeBitmap::Id, bitmap,   ZCL_BITMAP8_ATTRIBUTE_TYPE);
    WriteScalarAttribute(ep, OccupancySensing::Id, FeatureMap::Id,                fmap,     ZCL_BITMAP32_ATTRIBUTE_TYPE);
}

void ConstructSlot(CameraSlot & slot, EndpointId ep)
{
    slot.endpointId    = ep;
    slot.avDelegate    = std::make_unique<AVStreamDelegate>();
    slot.avDelegate->SetEndpointId(ep);
    slot.zoneDelegate  = std::make_unique<ZoneMgmtDelegate>();
    slot.userDelegate  = std::make_unique<UserLevelMgmtDelegate>();
    slot.webrtcDelegate = std::make_unique<WebRTCProviderDelegate>();
    slot.webrtcDelegate->SetEndpointId(ep);
    slot.webrtcDelegate->SetAVStreamDelegate(slot.avDelegate.get());

    // CameraAVStreamMgmt — Init() requires at least one of Video/Audio/Snapshot.
    // Snapshot is the smallest delta-feature: ZAP advertises it but the
    // delegate's CaptureSnapshot calls into Swift via the back-channel.
    BitFlags<CameraAvStreamManagement::Feature>           avFeatures;
    avFeatures.Set(CameraAvStreamManagement::Feature::kSnapshot);
    avFeatures.Set(CameraAvStreamManagement::Feature::kVideo);
    BitFlags<CameraAvStreamManagement::OptionalAttribute> avOptional;
    CameraAvStreamManagement::VideoSensorParamsStruct     sensor;
    sensor.sensorWidth  = 1920;
    sensor.sensorHeight = 1080;
    sensor.maxFPS       = 30;
    CameraAvStreamManagement::VideoResolutionStruct minViewport;
    minViewport.width  = 320;
    minViewport.height = 240;
    std::vector<CameraAvStreamManagement::RateDistortionTradeOffStruct> rdPoints;
    {
        // One trade-off point at 1080p H.264 ~4 Mbps so VideoStreamAllocate
        // has something to clamp into.
        CameraAvStreamManagement::RateDistortionTradeOffStruct rd;
        rd.codec             = CameraAvStreamManagement::VideoCodecEnum::kH264;
        rd.resolution.width  = 1920;
        rd.resolution.height = 1080;
        rd.minBitRate        = 1; // bits/s lower bound; cluster Init() requires >= 1
        rdPoints.push_back(rd);
    }

    CameraAvStreamManagement::AudioCapabilitiesStruct micCaps; // empty supportedCodecs/sampleRates/bitDepths
    CameraAvStreamManagement::AudioCapabilitiesStruct spkrCaps;

    std::vector<CameraAvStreamManagement::SnapshotCapabilitiesStruct> snapCaps;
    {
        CameraAvStreamManagement::SnapshotCapabilitiesStruct cap;
        cap.resolution.width  = 1920;
        cap.resolution.height = 1080;
        cap.maxFrameRate      = 1;
        cap.imageCodec        = CameraAvStreamManagement::ImageCodecEnum::kJpeg;
        cap.requiresEncodedPixels = false;
        snapCaps.push_back(cap);
    }

    std::vector<Globals::StreamUsageEnum> streamUsages;
    streamUsages.push_back(Globals::StreamUsageEnum::kLiveView);
    streamUsages.push_back(Globals::StreamUsageEnum::kRecording);
    std::vector<Globals::StreamUsageEnum> priorities;
    priorities.push_back(Globals::StreamUsageEnum::kLiveView);
    priorities.push_back(Globals::StreamUsageEnum::kRecording);

    slot.avServer = std::make_unique<CameraAvStreamManagement::CameraAVStreamMgmtServer>(
        *slot.avDelegate, ep, avFeatures, avOptional,
        /*maxConcurrentEncoders=*/1, /*maxEncodedPixelRate=*/0, sensor, /*nightVisionUsesInfrared=*/false,
        minViewport, rdPoints, /*maxContentBufferSize=*/0, micCaps, spkrCaps,
        CameraAvStreamManagement::TwoWayTalkSupportTypeEnum::kNotSupported, snapCaps,
        /*maxNetworkBandwidth=*/0, streamUsages, priorities);
    {
        CHIP_ERROR err = slot.avServer->Init();
        if (err != CHIP_NO_ERROR)
        {
            ChipLogError(NotSpecified, "AVStream Init failed for slot ep=%u: %" CHIP_ERROR_FORMAT, ep, err.Format());
        }
    }

    // ZoneMgmt — no features, but Init() requires MaxZones >= 1 when UserDefined
    // is off, and SensitivityMax must be in [2, 10].
    BitFlags<ZoneManagement::Feature> zoneFeatures;
    ZoneManagement::TwoDCartesianVertexStruct twoDMax;
    twoDMax.x = 1919;
    twoDMax.y = 1079;
    slot.zoneServer = std::make_unique<ZoneManagement::ZoneMgmtServer>(
        *slot.zoneDelegate, ep, zoneFeatures,
        /*maxUserDefinedZones=*/0, /*maxZones=*/1, /*sensitivityMax=*/3, twoDMax);
    {
        CHIP_ERROR err = slot.zoneServer->Init();
        if (err != CHIP_NO_ERROR)
        {
            ChipLogError(NotSpecified, "ZoneMgmt Init failed for slot ep=%u: %" CHIP_ERROR_FORMAT, ep, err.Format());
        }
    }

    // CameraAvSettingsUserLevelMgmt — Init() requires at least one PTZ feature.
    // DigitalPTZ is the cheapest to advertise; the delegate just rejects all
    // movements with UnsupportedAccess until the back-channel is wired.
    BitFlags<CameraAvSettingsUserLevelManagement::Feature> userFeatures;
    userFeatures.Set(CameraAvSettingsUserLevelManagement::Feature::kDigitalPTZ);
    BitFlags<CameraAvSettingsUserLevelManagement::OptionalAttributes> userOptional;
    // Init() pairs DPTZStreams optional attribute with DigitalPTZ feature; either both
    // present or neither.
    userOptional.Set(CameraAvSettingsUserLevelManagement::OptionalAttributes::kDptzStreams);
    slot.userServer = std::make_unique<CameraAvSettingsUserLevelManagement::CameraAvSettingsUserLevelMgmtServer>(
        ep, *slot.userDelegate, userFeatures, userOptional, /*maxPresets=*/0);
    {
        CHIP_ERROR err = slot.userServer->Init();
        if (err != CHIP_NO_ERROR)
        {
            ChipLogError(NotSpecified, "UserLevelMgmt Init failed for slot ep=%u: %" CHIP_ERROR_FORMAT, ep, err.Format());
        }
    }

    // WebRTCTransportProvider — no allocated streams, all sessions rejected.
    slot.webrtcServer = std::make_unique<WebRTCTransportProvider::WebRTCTransportProviderServer>(*slot.webrtcDelegate, ep);
    {
        CHIP_ERROR err = slot.webrtcServer->Init();
        if (err != CHIP_NO_ERROR)
        {
            ChipLogError(NotSpecified, "WebRTCProvider Init failed for slot ep=%u: %" CHIP_ERROR_FORMAT, ep, err.Format());
        }
    }
}
} // anonymous namespace

void InitCameraSlots()
{
    if (gSlotsInitialized) return;
    gSlotsInitialized = true;

    for (size_t i = 0; i < kCameraSlotCount; ++i)
    {
        EndpointId ep = static_cast<EndpointId>(kFirstCameraSlotEndpoint + i);
        ConstructSlot(gSlots[i], ep);

        // Seed an unoccupied label. We leave the endpoint enabled — disabling
        // here breaks the cluster servers' AttributeAccessInterface dispatch
        // (reads on the re-enabled endpoint return FAILURE because ember's
        // per-endpoint indexing was finalized during the disabled window).
        WriteCharStringAttribute(ep, BridgedDeviceBasicInformation::Id,
                                 BridgedDeviceBasicInformation::Attributes::NodeLabel::Id, "Camera (unused)");
        ChipLogProgress(NotSpecified, "Camera slot %zu (endpoint %u) constructed", i, ep);
    }
}

CameraSlot * FindSlotByExtId(const std::string & extId)
{
    for (auto & s : gSlots)
    {
        if (s.occupied && s.extId == extId) return &s;
    }
    return nullptr;
}

CameraSlot * FindFreeSlot()
{
    for (auto & s : gSlots)
    {
        if (!s.occupied) return &s;
    }
    return nullptr;
}

void HandleAddCamera(const Json::Value & cmd)
{
    if (!cmd.isMember("Id") || !cmd["Id"].isString())
    {
        ChipLogError(NotSpecified, "AddCamera: missing 'Id'");
        return;
    }
    std::string id   = cmd["Id"].asString();
    std::string name = cmd.get("DisplayName", "Camera").asString();

    if (auto * existing = FindSlotByExtId(id))
    {
        // Wording matches MatterCameraIPCTests' "already present" substring.
        ChipLogProgress(NotSpecified, "AddCamera: id '%s' already present (endpoint %u)", id.c_str(), existing->endpointId);
        return;
    }

    auto * slot = FindFreeSlot();
    if (!slot)
    {
        ChipLogError(NotSpecified, "AddCamera: out of slots (max %zu)", kCameraSlotCount);
        return;
    }

    slot->occupied    = true;
    slot->extId       = id;
    slot->displayName = name;

    PopulateBdbiDefaults(slot->endpointId, name);
    PopulateOccupancyDefaults(slot->endpointId);

    ChipLogProgress(NotSpecified, "AddCamera: id='%s' name='%s' endpoint=%u (slot index %ld)",
                    id.c_str(), name.c_str(), slot->endpointId, slot - gSlots.data());
    fflush(stdout);
}

void HandleRemoveCamera(const Json::Value & cmd)
{
    if (!cmd.isMember("Id") || !cmd["Id"].isString())
    {
        ChipLogError(NotSpecified, "RemoveCamera: missing 'Id'");
        return;
    }
    std::string id   = cmd["Id"].asString();
    auto *      slot = FindSlotByExtId(id);
    if (!slot)
    {
        ChipLogProgress(NotSpecified, "RemoveCamera: id '%s' not found", id.c_str());
        return;
    }

    WriteCharStringAttribute(slot->endpointId, BridgedDeviceBasicInformation::Id,
                             BridgedDeviceBasicInformation::Attributes::NodeLabel::Id, "Camera (unused)");
    ChipLogProgress(NotSpecified, "RemoveCamera: id='%s' endpoint=%u released", id.c_str(), slot->endpointId);

    slot->occupied     = false;
    slot->extId.clear();
    slot->displayName.clear();
    slot->motionActive = false;
    fflush(stdout);
}

void HandleTriggerMotion(const Json::Value & cmd)
{
    if (!cmd.isMember("Id") || !cmd["Id"].isString()) return;
    auto * slot = FindSlotByExtId(cmd["Id"].asString());
    if (!slot || slot->motionActive) return;

    slot->motionActive = true;
    uint8_t value = 0x01;
    emberAfWriteAttribute(slot->endpointId, OccupancySensing::Id, OccupancySensing::Attributes::Occupancy::Id,
                          &value, ZCL_BITMAP8_ATTRIBUTE_TYPE);
    MatterReportingAttributeChangeCallback(slot->endpointId, OccupancySensing::Id,
                                           OccupancySensing::Attributes::Occupancy::Id);
    ChipLogProgress(NotSpecified, "TriggerMotion: id='%s' endpoint=%u",
                    cmd["Id"].asString().c_str(), slot->endpointId);
    fflush(stdout);
}

void HandleClearMotion(const Json::Value & cmd)
{
    if (!cmd.isMember("Id") || !cmd["Id"].isString()) return;
    auto * slot = FindSlotByExtId(cmd["Id"].asString());
    if (!slot || !slot->motionActive) return;

    slot->motionActive = false;
    uint8_t value = 0x00;
    emberAfWriteAttribute(slot->endpointId, OccupancySensing::Id, OccupancySensing::Attributes::Occupancy::Id,
                          &value, ZCL_BITMAP8_ATTRIBUTE_TYPE);
    MatterReportingAttributeChangeCallback(slot->endpointId, OccupancySensing::Id,
                                           OccupancySensing::Attributes::Occupancy::Id);
    ChipLogProgress(NotSpecified, "ClearMotion: id='%s' endpoint=%u",
                    cmd["Id"].asString().c_str(), slot->endpointId);
    fflush(stdout);
}

} // namespace MatterBridge
