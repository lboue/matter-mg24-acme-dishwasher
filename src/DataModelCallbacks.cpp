/**
 * @file
 *   This file implements the handler for data model messages.
 */
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/ConcreteAttributePath.h>
#include <lib/support/logging/CHIPLogging.h>

#include "DishwasherManager.h"

using namespace chip;
using namespace chip::app::Clusters;

void MatterPostAttributeChangeCallback(const chip::app::ConcreteAttributePath & attributePath, uint8_t type, uint16_t size,
                                       uint8_t * value)
{
    ClusterId clusterId     = attributePath.mClusterId;
    AttributeId attributeId = attributePath.mAttributeId;
    ChipLogDetail(Zcl, "Cluster callback: " ChipLogFormatMEI, ChipLogValueMEI(clusterId));

    if (attributePath.mEndpointId == DishwasherManager::kDishwasherEndpoint && clusterId == OnOff::Id &&
        attributeId == OnOff::Attributes::OnOff::Id)
    {
        bool on = *value != 0;
        ChipLogProgress(Zcl, "OnOff attribute updated to: %s!", on ? "on" : "off");

        if (on)
        {
            DishwasherMgr().TurnOnPower();
        }
        else
        {
            DishwasherMgr().TurnOffPower();
        }
    }
    else if (clusterId == Identify::Id)
    {
        ChipLogProgress(Zcl, "Identify attribute ID: " ChipLogFormatMEI " Type: %u Value: %u, length %u",
                        ChipLogValueMEI(attributeId), type, *value, size);
    }
}
