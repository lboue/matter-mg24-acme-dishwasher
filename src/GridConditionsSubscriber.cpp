#include "GridConditionsSubscriber.h"

#include "DishwasherManager.h"

#include <app-common/zap-generated/cluster-objects.h>
#include <app/InteractionModelEngine.h>
#include <app/clusters/bindings/BindingManager.h>
#include <app/server/Server.h>
#include <lib/support/logging/CHIPLogging.h>

#include <inttypes.h>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;

GridConditionsSubscriber GridConditionsSubscriber::sInstance;

namespace {
constexpr uint16_t kMinIntervalFloorS   = 1;
constexpr uint16_t kMaxIntervalCeilingS = 300;

// ReadClient wants these to outlive the subscription; see PriceSubscriber.
AttributePathParams sAttributePaths[2];

void CopyConditions(const ElectricalGridConditions::Structs::ElectricalGridConditionsStruct::DecodableType & src,
                    GridConditionsSubscriber::Conditions & dst)
{
    dst.periodStart         = src.periodStart;
    dst.periodEnd           = src.periodEnd.IsNull() ? 0 : src.periodEnd.Value();
    dst.gridCarbonIntensity = src.gridCarbonIntensity;
}
} // namespace

void GridConditionsSubscriber::Init()
{
    LogErrorOnFailure(DeviceLayer::PlatformMgr().AddEventHandler(OnPlatformEvent, 0));

    Refresh();
}

void GridConditionsSubscriber::OnPlatformEvent(const DeviceLayer::ChipDeviceEvent * event, intptr_t)
{
    switch (event->Type)
    {
    case DeviceLayer::DeviceEventType::kBindingsChangedViaCluster:
    case DeviceLayer::DeviceEventType::kServerReady:
        sInstance.Refresh();
        break;
    default:
        break;
    }
}

void GridConditionsSubscriber::Refresh()
{
    mReadClient.reset();

    const Binding::TableEntry * target = nullptr;
    for (const auto & entry : Binding::Manager::GetInstance().GetBindingTable())
    {
        if (entry.type == Binding::MATTER_UNICAST_BINDING && entry.local == DishwasherManager::kDishwasherEndpoint &&
            entry.clusterId == std::make_optional(ElectricalGridConditions::Id))
        {
            target = &entry;
            break;
        }
    }

    if (target == nullptr)
    {
        ChipLogProgress(AppServer, "No Electrical Grid Conditions binding; not reading grid conditions");
        ClearConditions();
        return;
    }

    ChipLogProgress(AppServer, "Subscribing to Electrical Grid Conditions on node 0x" ChipLogFormatX64 " endpoint %u",
                    ChipLogValueX64(target->nodeId), target->remote);

    sAttributePaths[0] =
        AttributePathParams(target->remote, ElectricalGridConditions::Id, ElectricalGridConditions::Attributes::CurrentConditions::Id);
    sAttributePaths[1] =
        AttributePathParams(target->remote, ElectricalGridConditions::Id, ElectricalGridConditions::Attributes::ForecastConditions::Id);

    ReadPrepareParams params;
    params.mpAttributePathParamsList    = sAttributePaths;
    params.mAttributePathParamsListSize = MATTER_ARRAY_SIZE(sAttributePaths);
    params.mMinIntervalFloorSeconds     = kMinIntervalFloorS;
    params.mMaxIntervalCeilingSeconds   = kMaxIntervalCeilingS;

    ScopedNodeId peer(target->nodeId, target->fabricIndex);

    mReadClient = std::make_unique<ReadClient>(InteractionModelEngine::GetInstance(),
                                               &Server::GetInstance().GetExchangeManager(), *this,
                                               ReadClient::InteractionType::Subscribe);

    CHIP_ERROR err = mReadClient->SendAutoResubscribeRequest(peer, std::move(params));
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(AppServer, "Electrical Grid Conditions subscription failed: %" CHIP_ERROR_FORMAT, err.Format());
        mReadClient.reset();
    }
}

void GridConditionsSubscriber::OnAttributeData(const ConcreteDataAttributePath & aPath, TLV::TLVReader * apData,
                                               const StatusIB & aStatus)
{
    if (aPath.mClusterId != ElectricalGridConditions::Id)
    {
        return;
    }

    if (!aStatus.IsSuccess() || apData == nullptr)
    {
        if (aPath.mAttributeId == ElectricalGridConditions::Attributes::ForecastConditions::Id)
        {
            // Servers without the Forecasting (FORE) feature don't have this attribute.
            ChipLogProgress(AppServer, "Electrical Grid Conditions server has no forecast");
            mForecastSupported = false;
            mForecastCount     = 0;
        }
        else
        {
            ChipLogError(AppServer, "Electrical Grid Conditions attribute 0x%" PRIx32 " error: %" CHIP_ERROR_FORMAT,
                         aPath.mAttributeId, aStatus.ToChipError().Format());
        }
        return;
    }

    CHIP_ERROR err = CHIP_NO_ERROR;

    switch (aPath.mAttributeId)
    {
    case ElectricalGridConditions::Attributes::CurrentConditions::Id: {
        ElectricalGridConditions::Attributes::CurrentConditions::TypeInfo::DecodableType current;
        SuccessOrExit(err = DataModel::Decode(*apData, current));
        mHasCurrentConditions = !current.IsNull();
        if (mHasCurrentConditions)
        {
            CopyConditions(current.Value(), mCurrentConditions);
        }
        break;
    }
    case ElectricalGridConditions::Attributes::ForecastConditions::Id: {
        if (aPath.IsListItemOperation())
        {
            // A list too big for one report arrives as an empty list, then one append per entry.
            ElectricalGridConditions::Structs::ElectricalGridConditionsStruct::DecodableType entry;
            SuccessOrExit(err = DataModel::Decode(*apData, entry));
            if (mForecastCount < kMaxForecastEntries)
            {
                CopyConditions(entry, mForecast[mForecastCount++]);
            }
            break;
        }

        ElectricalGridConditions::Attributes::ForecastConditions::TypeInfo::DecodableType forecast;
        SuccessOrExit(err = DataModel::Decode(*apData, forecast));
        mForecastSupported = true;
        mForecastCount     = 0;
        auto iter          = forecast.begin();
        while (iter.Next() && mForecastCount < kMaxForecastEntries)
        {
            CopyConditions(iter.GetValue(), mForecast[mForecastCount++]);
        }
        SuccessOrExit(err = iter.GetStatus());
        break;
    }
    default:
        return;
    }

    ConditionsChanged();

exit:
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(AppServer, "Failed to decode Electrical Grid Conditions attribute 0x%" PRIx32 ": %" CHIP_ERROR_FORMAT,
                     aPath.mAttributeId, err.Format());
    }
}

void GridConditionsSubscriber::OnSubscriptionEstablished(SubscriptionId aSubscriptionId)
{
    ChipLogProgress(AppServer, "Electrical Grid Conditions subscription established (id 0x%" PRIx32 ")", aSubscriptionId);
}

void GridConditionsSubscriber::OnError(CHIP_ERROR aError)
{
    // ReadClient keeps trying to resubscribe on its own.
    ChipLogError(AppServer, "Electrical Grid Conditions subscription error: %" CHIP_ERROR_FORMAT, aError.Format());
}

void GridConditionsSubscriber::OnDone(ReadClient * apReadClient)
{
    ChipLogProgress(AppServer, "Electrical Grid Conditions subscription ended");
    if (mReadClient.get() == apReadClient)
    {
        mReadClient.reset();
    }
}

void GridConditionsSubscriber::OnDeallocatePaths(ReadPrepareParams && aReadPrepareParams)
{
    // The paths are in sAttributePaths, nothing to free.
}

void GridConditionsSubscriber::ClearConditions()
{
    mHasCurrentConditions = false;
    mForecastSupported    = false;
    mForecastCount        = 0;
    ConditionsChanged();
}

void GridConditionsSubscriber::ConditionsChanged()
{
    DishwasherMgr().UpdateDishwasherDisplay();
}

bool GridConditionsSubscriber::GetCurrentConditions(Conditions & conditions) const
{
    VerifyOrReturnValue(mHasCurrentConditions, false);
    conditions = mCurrentConditions;
    return true;
}
