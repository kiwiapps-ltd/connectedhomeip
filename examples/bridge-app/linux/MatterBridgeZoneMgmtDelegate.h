/*
 * MatterBridge: minimal ZoneManagement delegate.
 *
 * Honest scope: returns ONE factory motion zone (id=1) covering the full
 * 1920×1080 frame, and rejects all mutations with UnsupportedAccess. The
 * factory zone is what `ZoneTriggered` / `ZoneStopped` events reference
 * when a Scrypted motion event arrives — without it, the events refer
 * to a zone the cluster doesn't know about, and SmartThings's matter-
 * camera Edge driver drops them. With the zone declared, motion events
 * land in SmartThings's history view with proper timestamps.
 */
#pragma once

#include <app/clusters/zone-management-server/zone-management-server.h>
#include <clusters/ZoneManagement/Enums.h>

namespace MatterBridge {

class ZoneMgmtDelegate : public chip::app::Clusters::ZoneManagement::Delegate
{
public:
    using Status                       = chip::Protocols::InteractionModel::Status;
    using TwoDCartesianZoneStorage     = chip::app::Clusters::ZoneManagement::TwoDCartesianZoneStorage;
    using ZoneInformationStorage       = chip::app::Clusters::ZoneManagement::ZoneInformationStorage;
    using ZoneTriggerControlStruct     = chip::app::Clusters::ZoneManagement::ZoneTriggerControlStruct;
    using TwoDCartesianVertexStruct    = chip::app::Clusters::ZoneManagement::Structs::TwoDCartesianVertexStruct::Type;
    using ZoneUseEnum                  = chip::app::Clusters::ZoneManagement::ZoneUseEnum;
    using ZoneTypeEnum                 = chip::app::Clusters::ZoneManagement::ZoneTypeEnum;
    using ZoneSourceEnum               = chip::app::Clusters::ZoneManagement::ZoneSourceEnum;

    Status CreateTwoDCartesianZone(const TwoDCartesianZoneStorage &, uint16_t & outZoneID) override
    {
        outZoneID = 0;
        return Status::UnsupportedAccess;
    }
    Status UpdateTwoDCartesianZone(uint16_t, const TwoDCartesianZoneStorage &) override { return Status::UnsupportedAccess; }
    Status RemoveZone(uint16_t) override { return Status::UnsupportedAccess; }
    Status CreateTrigger(const ZoneTriggerControlStruct &) override { return Status::UnsupportedAccess; }
    Status UpdateTrigger(const ZoneTriggerControlStruct &) override { return Status::UnsupportedAccess; }
    Status RemoveTrigger(uint16_t) override { return Status::UnsupportedAccess; }
    void OnAttributeChanged(chip::AttributeId) override {}
    CHIP_ERROR PersistentAttributesLoadedCallback() override { return CHIP_NO_ERROR; }

    CHIP_ERROR LoadZones(std::vector<ZoneInformationStorage> & aZones) override
    {
        // One factory motion zone covering the full 1920×1080 frame.
        // ZoneSource = MFG (manufacturer-defined; user can't edit).
        // ZoneType   = TwoDCartesian (matches the kTwoDimensionalCartesianZone feature).
        // ZoneUse    = Motion.
        std::vector<TwoDCartesianVertexStruct> vertices;
        TwoDCartesianVertexStruct v;
        v.x = 0;       v.y = 0;       vertices.push_back(v);
        v.x = 1919;    v.y = 0;       vertices.push_back(v);
        v.x = 1919;    v.y = 1079;    vertices.push_back(v);
        v.x = 0;       v.y = 1079;    vertices.push_back(v);

        TwoDCartesianZoneStorage twoD(
            chip::CharSpan::fromCharString("Bridge"),
            ZoneUseEnum::kMotion,
            vertices,
            chip::Optional<chip::CharSpan>());

        ZoneInformationStorage info(
            /*zoneID=*/1,
            ZoneTypeEnum::kTwoDCARTZone,
            ZoneSourceEnum::kMfg,
            chip::MakeOptional(twoD));
        aZones.push_back(info);
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR LoadTriggers(std::vector<ZoneTriggerControlStruct> &) override { return CHIP_NO_ERROR; }
};

} // namespace MatterBridge
