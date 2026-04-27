/*
 * MatterBridge: pre-allocated camera slots.
 *
 * The bridge ZAP defines 8 fixed slot endpoints (3..10), each with the Matter
 * 1.5 camera cluster set: ZoneManagement, CameraAVStreamMgmt,
 * UserLevelMgmt, WebRTCTransportProvider, plus BridgedDeviceBasicInformation
 * and OccupancySensing.
 *
 * This module owns the cluster server + delegate instances per slot and the
 * mapping from Swift Camera UUIDs to slot endpoints. Slots are disabled at
 * startup; AddCamera enables one and writes its NodeLabel; RemoveCamera
 * disables it.
 */
#pragma once

#include <app/clusters/camera-av-settings-user-level-management-server/camera-av-settings-user-level-management-server.h>
#include <app/clusters/camera-av-stream-management-server/camera-av-stream-management-server.h>
#include <app/clusters/webrtc-transport-provider-server/webrtc-transport-provider-server.h>
#include <app/clusters/zone-management-server/zone-management-server.h>
#include <app/util/attribute-storage.h>

#include "MatterBridgeAVStreamDelegate.h"
#include "MatterBridgeUserLevelMgmtDelegate.h"
#include "MatterBridgeWebRTCProviderDelegate.h"
#include "MatterBridgeZoneMgmtDelegate.h"

#include <json/json.h>
#include <memory>
#include <string>

namespace MatterBridge {

constexpr chip::EndpointId kFirstCameraSlotEndpoint = 3;
constexpr size_t           kCameraSlotCount         = 8;

struct CameraSlot
{
    chip::EndpointId endpointId = chip::kInvalidEndpointId;
    bool             occupied   = false;
    std::string      extId;       // Swift Camera UUID
    std::string      displayName;
    bool             motionActive = false;

    std::unique_ptr<AVStreamDelegate>          avDelegate;
    std::unique_ptr<chip::app::Clusters::CameraAvStreamManagement::CameraAVStreamMgmtServer> avServer;

    std::unique_ptr<ZoneMgmtDelegate>          zoneDelegate;
    std::unique_ptr<chip::app::Clusters::ZoneManagement::ZoneMgmtServer> zoneServer;

    std::unique_ptr<UserLevelMgmtDelegate>     userDelegate;
    std::unique_ptr<chip::app::Clusters::CameraAvSettingsUserLevelManagement::CameraAvSettingsUserLevelMgmtServer> userServer;

    std::unique_ptr<WebRTCProviderDelegate>    webrtcDelegate;
    std::unique_ptr<chip::app::Clusters::WebRTCTransportProvider::WebRTCTransportProviderServer> webrtcServer;
};

/// Construct cluster server + delegate per slot, register them with the
/// interaction model, and disable the slot endpoint until AddCamera enables
/// it. Idempotent — safe to call once at ApplicationInit.
void InitCameraSlots();

/// Look up a slot by Swift external id. Returns nullptr when not found.
CameraSlot * FindSlotByExtId(const std::string & extId);

/// Resolve a slot endpoint id back to the Swift camera UUID (extId). Empty
/// string when no occupied slot exists at that endpoint. Used by the WebRTC
/// back-channel to tell Swift which camera a session belongs to.
std::string FindExtIdByEndpoint(chip::EndpointId ep);

/// First slot with occupied=false, or nullptr if all 8 are taken.
CameraSlot * FindFreeSlot();

/// IPC handlers — invoked from the BridgeAppCommandHandler dispatch.
void HandleAddCamera(const Json::Value & cmd);
void HandleRemoveCamera(const Json::Value & cmd);
void HandleTriggerMotion(const Json::Value & cmd);
void HandleClearMotion(const Json::Value & cmd);

} // namespace MatterBridge
