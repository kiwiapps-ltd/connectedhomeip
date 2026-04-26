/*
 * MatterBridge: minimal CameraAvSettingsUserLevelManagement delegate.
 *
 * Returns UnsupportedAccess for all PTZ commands; loads no presets / streams.
 * Just enough surface to let the cluster server register and answer reads.
 */
#pragma once

#include <app/clusters/camera-av-settings-user-level-management-server/camera-av-settings-user-level-management-server.h>

namespace MatterBridge {

class UserLevelMgmtDelegate : public chip::app::Clusters::CameraAvSettingsUserLevelManagement::Delegate
{
public:
    using Status               = chip::Protocols::InteractionModel::Status;
    using MPTZStructType       = chip::app::Clusters::CameraAvSettingsUserLevelManagement::MPTZStructType;
    using MPTZPresetHelper     = chip::app::Clusters::CameraAvSettingsUserLevelManagement::MPTZPresetHelper;
    using DPTZStruct           = chip::app::Clusters::CameraAvSettingsUserLevelManagement::DPTZStruct;
    using ViewportStructType   = chip::app::Clusters::Globals::Structs::ViewportStruct::Type;
    using PhysicalPTZCallback  = chip::app::Clusters::CameraAvSettingsUserLevelManagement::PhysicalPTZCallback;

    void ShutdownApp() override {}
    bool CanChangeMPTZ() override { return false; }
    void VideoStreamAllocated(uint16_t) override {}
    void VideoStreamDeallocated(uint16_t) override {}
    void DefaultViewportUpdated(ViewportStructType) override {}

    Status MPTZSetPosition(chip::Optional<int16_t>, chip::Optional<int16_t>, chip::Optional<uint8_t>, PhysicalPTZCallback *) override
    {
        return Status::UnsupportedAccess;
    }
    Status MPTZRelativeMove(chip::Optional<int16_t>, chip::Optional<int16_t>, chip::Optional<uint8_t>, PhysicalPTZCallback *) override
    {
        return Status::UnsupportedAccess;
    }
    Status MPTZMoveToPreset(uint8_t, chip::Optional<int16_t>, chip::Optional<int16_t>, chip::Optional<uint8_t>,
                            PhysicalPTZCallback *) override
    {
        return Status::UnsupportedAccess;
    }
    Status MPTZSavePreset(uint8_t) override { return Status::UnsupportedAccess; }
    Status MPTZRemovePreset(uint8_t) override { return Status::UnsupportedAccess; }
    Status DPTZSetViewport(uint16_t, ViewportStructType) override { return Status::UnsupportedAccess; }
    Status DPTZRelativeMove(uint16_t, chip::Optional<int16_t>, chip::Optional<int16_t>, chip::Optional<int8_t>,
                            ViewportStructType &) override
    {
        return Status::UnsupportedAccess;
    }

    CHIP_ERROR PersistentAttributesLoadedCallback() override { return CHIP_NO_ERROR; }
    CHIP_ERROR LoadMPTZPresets(std::vector<MPTZPresetHelper> &) override { return CHIP_NO_ERROR; }
    CHIP_ERROR LoadDPTZStreams(std::vector<DPTZStruct> &) override { return CHIP_NO_ERROR; }
};

} // namespace MatterBridge
