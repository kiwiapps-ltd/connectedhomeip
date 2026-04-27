#include "MatterBridgeCameraSlots.h"
#include "MatterBridgeBackChannel.h"

#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/AttributeAccessInterfaceRegistry.h>
#include <app/CommandHandlerInterfaceRegistry.h>
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

void PopulateBdbiDefaults(EndpointId ep, const std::string & nodeLabel, bool reachable)
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
    bool     reachableValue = reachable;
    WriteScalarAttribute(ep, BridgedDeviceBasicInformation::Id, VendorID::Id,        vendorId,       ZCL_VENDOR_ID_ATTRIBUTE_TYPE);
    WriteScalarAttribute(ep, BridgedDeviceBasicInformation::Id, HardwareVersion::Id, hardwareVer,    ZCL_INT16U_ATTRIBUTE_TYPE);
    WriteScalarAttribute(ep, BridgedDeviceBasicInformation::Id, SoftwareVersion::Id, softwareVer,    ZCL_INT32U_ATTRIBUTE_TYPE);
    WriteScalarAttribute(ep, BridgedDeviceBasicInformation::Id, Reachable::Id,       reachableValue, ZCL_BOOLEAN_ATTRIBUTE_TYPE);
}

void PopulateOccupancyDefaults(EndpointId ep)
{
    using namespace OccupancySensing::Attributes;
    uint8_t occ      = 0;
    uint8_t sensorTy = 3; // PIR + ultrasonic — matches the spec stand-in
    uint8_t bitmap   = 0x01;
    WriteScalarAttribute(ep, OccupancySensing::Id, Occupancy::Id,                 occ,      ZCL_BITMAP8_ATTRIBUTE_TYPE);
    WriteScalarAttribute(ep, OccupancySensing::Id, OccupancySensorType::Id,       sensorTy, ZCL_ENUM8_ATTRIBUTE_TYPE);
    WriteScalarAttribute(ep, OccupancySensing::Id, OccupancySensorTypeBitmap::Id, bitmap,   ZCL_BITMAP8_ATTRIBUTE_TYPE);
    // FeatureMap (0xFFFC) is read-only by spec — ember returns Failure on
    // writes. Don't try to seed it; the cluster reports 0 by default.
}

// emberAfEndpointEnableDisable(false) calls shutdownEndpoint() which calls
// AttributeAccessInterfaceRegistry::UnregisterAllForEndpoint() AND
// CommandHandlerInterfaceRegistry::UnregisterAllCommandHandlersForEndpoint()
// — both registrations made by the cluster server's Init() get stripped.
// Re-enabling the endpoint runs cluster init *callbacks* (codegen plugin
// init), but does NOT call our C++ cluster server's Init() again, so AAI
// stays missing and every camera-cluster attribute read returns FAILURE
// (CodegenDataModelProvider_Read.cpp:137 falls through to ember's RAM
// storage, which has nothing because all camera attributes are External).
//
// This helper re-registers what shutdownEndpoint stripped. Call after each
// emberAfEndpointEnableDisable(slot, true).
void ReregisterSlotClusterServers(CameraSlot & slot)
{
    auto & aai = chip::app::AttributeAccessInterfaceRegistry::Instance();
    auto & chr = chip::app::CommandHandlerInterfaceRegistry::Instance();

    auto reg = [&](chip::app::AttributeAccessInterface * a, chip::app::CommandHandlerInterface * h, const char * name) {
        if (a && !aai.Register(a))
        {
            ChipLogError(NotSpecified, "Re-register AAI failed for %s ep=%u", name, slot.endpointId);
        }
        if (h)
        {
            CHIP_ERROR err = chr.RegisterCommandHandler(h);
            if (err != CHIP_NO_ERROR)
            {
                ChipLogError(NotSpecified, "Re-register CommandHandler failed for %s ep=%u: %" CHIP_ERROR_FORMAT,
                             name, slot.endpointId, err.Format());
            }
        }
    };
    reg(slot.avServer.get(),     slot.avServer.get(),     "AVStreamMgmt");
    reg(slot.zoneServer.get(),   slot.zoneServer.get(),   "ZoneMgmt");
    reg(slot.userServer.get(),   slot.userServer.get(),   "UserLevelMgmt");
    reg(slot.webrtcServer.get(), slot.webrtcServer.get(), "WebRTCProvider");
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
    // Watermark + OSD features must mirror the field-presence in incoming
    // VideoStreamAllocate / SnapshotStreamAllocate commands. SmartThings
    // unconditionally serializes watermarkEnabled + OSDEnabled, so we have to
    // advertise both or every allocate request comes back as INVALID_COMMAND
    // (camera-av-stream-management-server.cpp:2248). The features are stubs
    // — we never actually draw a watermark / OSD overlay.
    BitFlags<CameraAvStreamManagement::Feature>           avFeatures;
    avFeatures.Set(CameraAvStreamManagement::Feature::kSnapshot);
    avFeatures.Set(CameraAvStreamManagement::Feature::kVideo);
    avFeatures.Set(CameraAvStreamManagement::Feature::kWatermark);
    avFeatures.Set(CameraAvStreamManagement::Feature::kOnScreenDisplay);
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

    // Route inbound back-channel events from Swift (e.g. local ICE
    // candidates from libwebrtc) to the right slot's WebRTCProviderDelegate
    // so they get forwarded to the controller as a Matter cluster command.
    GetBackChannel().SetEventHandler([](const Json::Value & msg) {
        const std::string name = msg.isMember("Name") ? msg["Name"].asString() : "";
        if (name == "WebRTC.LocalCandidate")
        {
            const auto ep = static_cast<chip::EndpointId>(msg.get("Endpoint", 0).asUInt());
            const auto sid = static_cast<uint16_t>(msg.get("SessionId", 0).asUInt());
            const std::string cand = msg.get("Candidate", "").asString();
            const std::string mid  = msg.get("SdpMid", "").asString();
            const int mlIdx        = msg.isMember("SdpMLineIndex") ? msg["SdpMLineIndex"].asInt() : -1;
            for (auto & s : gSlots)
            {
                if (s.endpointId == ep && s.webrtcDelegate)
                {
                    s.webrtcDelegate->EnqueueLocalIceCandidate(sid, cand, mid, mlIdx);
                    return;
                }
            }
            ChipLogError(NotSpecified, "WebRTC.LocalCandidate: no slot for endpoint %u", ep);
        }
    });

    for (size_t i = 0; i < kCameraSlotCount; ++i)
    {
        EndpointId ep = static_cast<EndpointId>(kFirstCameraSlotEndpoint + i);
        ConstructSlot(gSlots[i], ep);

        // Disable the slot endpoint until AddCamera occupies it. Disabled
        // slot endpoints are automatically excluded from the aggregator's
        // Descriptor::PartsList — the spec-correct way to hide bridged
        // children.
        //
        // shutdownEndpoint() (triggered by the disable below) calls
        // UnregisterAllForEndpoint on both the AAI and command-handler
        // registries — so the cluster server registrations we just made in
        // ConstructSlot() get stripped here. Re-enable in HandleAddCamera
        // calls ReregisterSlotClusterServers() to restore them; without that
        // re-attach, every camera-cluster attribute read on the slot
        // endpoint returns FAILURE.
        //
        // We deliberately do NOT batch-write BDBI defaults here — that path
        // triggered helper crashes during SmartThings teardown. BDBI fields
        // are populated only when AddCamera enables the slot.
        emberAfEndpointEnableDisable(ep, false);
        ChipLogProgress(NotSpecified, "Camera slot %zu (endpoint %u) constructed (disabled)", i, ep);
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

std::string FindExtIdByEndpoint(EndpointId ep)
{
    for (auto & s : gSlots)
    {
        if (s.occupied && s.endpointId == ep) return s.extId;
    }
    return {};
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

    // Enable the slot endpoint FIRST so subsequent attribute writes succeed
    // (writes to a disabled endpoint return 0x7F UNSUPPORTED_WRITE).
    emberAfEndpointEnableDisable(slot->endpointId, true);
    // ember's shutdownEndpoint stripped the cluster server AAI/command-handler
    // registrations when we disabled the slot at startup. Re-enable does NOT
    // call our C++ Init() again, so the AAI is still missing — every read on
    // this endpoint's camera clusters would return FAILURE. Re-attach.
    ReregisterSlotClusterServers(*slot);
    PopulateBdbiDefaults(slot->endpointId, name, /*reachable=*/true);
    PopulateOccupancyDefaults(slot->endpointId);
    // Tell subscribed controllers (SmartThings, Apple Home, …) the
    // aggregator's child set just changed. Without this, controllers
    // continue to see the cached PartsList from before the slot was
    // enabled and never enumerate the new camera endpoint.
    constexpr chip::EndpointId kAggregatorEndpoint = 1;
    MatterReportingAttributeChangeCallback(kAggregatorEndpoint, Descriptor::Id,
                                           Descriptor::Attributes::PartsList::Id);
    MatterReportingAttributeChangeCallback(static_cast<chip::EndpointId>(0), Descriptor::Id,
                                           Descriptor::Attributes::PartsList::Id);

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

    // Disable the slot endpoint so it's hidden from PartsList. ember keeps
    // the cluster server AAI registrations intact across the disable.
    emberAfEndpointEnableDisable(slot->endpointId, false);
    constexpr chip::EndpointId kAggregatorEndpoint = 1;
    MatterReportingAttributeChangeCallback(kAggregatorEndpoint, Descriptor::Id,
                                           Descriptor::Attributes::PartsList::Id);
    MatterReportingAttributeChangeCallback(static_cast<chip::EndpointId>(0), Descriptor::Id,
                                           Descriptor::Attributes::PartsList::Id);
    ChipLogProgress(NotSpecified, "RemoveCamera: id='%s' endpoint=%u released (disabled)", id.c_str(), slot->endpointId);

    slot->occupied     = false;
    slot->extId.clear();
    slot->displayName.clear();
    slot->motionActive = false;
    fflush(stdout);
}

// Default zone the bridge auto-publishes per camera. ID is reserved by the
// Matter spec for built-in motion zones; SmartThings (and other
// controllers) can subscribe to ZoneTriggered/ZoneStopped events keyed by
// this ID without the bridge needing to expose a zone editor UI.
static constexpr uint16_t kDefaultMotionZoneId = 1;

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
    if (slot->zoneServer)
    {
        // Emit a ZoneManagement.ZoneTriggered event so SmartThings (and
        // any other controller) can build automations on per-zone motion
        // edges instead of polling Occupancy. This pairs with the
        // Occupancy attribute write above; consumers that care about
        // event-style timestamps + per-zone routing get the event,
        // consumers that only sample state get the attribute.
        (void) slot->zoneServer->GenerateZoneTriggeredEvent(
            kDefaultMotionZoneId,
            chip::app::Clusters::ZoneManagement::ZoneEventTriggeredReasonEnum::kMotion);
    }
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
    if (slot->zoneServer)
    {
        (void) slot->zoneServer->GenerateZoneStoppedEvent(
            kDefaultMotionZoneId,
            chip::app::Clusters::ZoneManagement::ZoneEventStoppedReasonEnum::kActionStopped);
    }
    ChipLogProgress(NotSpecified, "ClearMotion: id='%s' endpoint=%u",
                    cmd["Id"].asString().c_str(), slot->endpointId);
    fflush(stdout);
}

} // namespace MatterBridge
