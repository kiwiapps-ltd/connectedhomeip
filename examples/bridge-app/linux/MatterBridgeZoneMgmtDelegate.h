/*
 * MatterBridge: minimal ZoneManagement delegate.
 *
 * Honest scope: returns empty zone/trigger lists and rejects mutations with
 * UnsupportedAccess. The point is to satisfy ZoneMgmtServer's pure-virtual
 * interface so it can register its AttributeAccessInterface and answer
 * FeatureMap reads (which is what SmartThings retries on after commissioning).
 */
#pragma once

#include <app/clusters/zone-management-server/zone-management-server.h>

namespace MatterBridge {

class ZoneMgmtDelegate : public chip::app::Clusters::ZoneManagement::Delegate
{
public:
    using Status                       = chip::Protocols::InteractionModel::Status;
    using TwoDCartesianZoneStorage     = chip::app::Clusters::ZoneManagement::TwoDCartesianZoneStorage;
    using ZoneInformationStorage       = chip::app::Clusters::ZoneManagement::ZoneInformationStorage;
    using ZoneTriggerControlStruct     = chip::app::Clusters::ZoneManagement::ZoneTriggerControlStruct;

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
    CHIP_ERROR LoadZones(std::vector<ZoneInformationStorage> &) override { return CHIP_NO_ERROR; }
    CHIP_ERROR LoadTriggers(std::vector<ZoneTriggerControlStruct> &) override { return CHIP_NO_ERROR; }
};

} // namespace MatterBridge
