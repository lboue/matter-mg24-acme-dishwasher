#include "PriceSubscriber.h"

#include "DishwasherManager.h"

#include <app-common/zap-generated/cluster-objects.h>
#include <app/InteractionModelEngine.h>
#include <app/clusters/bindings/BindingManager.h>
#include <app/server/Server.h>
#include <lib/support/logging/CHIPLogging.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;

PriceSubscriber PriceSubscriber::sInstance;

namespace {
constexpr uint16_t kMinIntervalFloorS   = 1;
constexpr uint16_t kMaxIntervalCeilingS = 300;

// ReadClient wants these to outlive the subscription. They're the same for every subscription, so
// rather than allocating them per subscription (and freeing in OnDeallocatePaths) they live here.
AttributePathParams sAttributePaths[4];

void CopyPrice(const CommodityPrice::Structs::CommodityPriceStruct::DecodableType & src, PriceSubscriber::PriceInfo & dst)
{
    dst.periodStart   = src.periodStart;
    dst.periodEnd     = src.periodEnd.IsNull() ? 0 : src.periodEnd.Value();
    dst.hasPrice      = src.price.HasValue();
    dst.price         = src.price.ValueOr(0);
    dst.hasPriceLevel = src.priceLevel.HasValue();
    dst.priceLevel    = src.priceLevel.ValueOr(0);
}
} // namespace

void PriceSubscriber::Init()
{
    auto & server = Server::GetInstance();
    LogErrorOnFailure(Binding::Manager::GetInstance().Init(
        { &server.GetFabricTable(), server.GetCASESessionManager(), &server.GetPersistentStorage() }));

    LogErrorOnFailure(DeviceLayer::PlatformMgr().AddEventHandler(OnPlatformEvent, 0));

    Refresh();
}

void PriceSubscriber::OnPlatformEvent(const DeviceLayer::ChipDeviceEvent * event, intptr_t)
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

void PriceSubscriber::Refresh()
{
    mReadClient.reset();

    const Binding::TableEntry * target = nullptr;
    for (const auto & entry : Binding::Manager::GetInstance().GetBindingTable())
    {
        if (entry.type == Binding::MATTER_UNICAST_BINDING && entry.local == DishwasherManager::kDishwasherEndpoint &&
            entry.clusterId == std::make_optional(CommodityPrice::Id))
        {
            target = &entry;
            break;
        }
    }

    if (target == nullptr)
    {
        ChipLogProgress(AppServer, "No Commodity Price binding; not reading prices");
        ClearPrices();
        return;
    }

    ChipLogProgress(AppServer, "Subscribing to Commodity Price on node 0x" ChipLogFormatX64 " endpoint %u",
                    ChipLogValueX64(target->nodeId), target->remote);

    const AttributeId attributes[] = { CommodityPrice::Attributes::TariffUnit::Id, CommodityPrice::Attributes::Currency::Id,
                                       CommodityPrice::Attributes::CurrentPrice::Id,
                                       CommodityPrice::Attributes::PriceForecast::Id };
    static_assert(MATTER_ARRAY_SIZE(attributes) == MATTER_ARRAY_SIZE(sAttributePaths));
    for (size_t i = 0; i < MATTER_ARRAY_SIZE(attributes); i++)
    {
        sAttributePaths[i] = AttributePathParams(target->remote, CommodityPrice::Id, attributes[i]);
    }

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
        ChipLogError(AppServer, "Commodity Price subscription failed: %" CHIP_ERROR_FORMAT, err.Format());
        mReadClient.reset();
    }
}

void PriceSubscriber::OnAttributeData(const ConcreteDataAttributePath & aPath, TLV::TLVReader * apData, const StatusIB & aStatus)
{
    if (aPath.mClusterId != CommodityPrice::Id)
    {
        return;
    }

    if (!aStatus.IsSuccess() || apData == nullptr)
    {
        if (aPath.mAttributeId == CommodityPrice::Attributes::PriceForecast::Id)
        {
            // Servers without the Forecasting (FORE) feature don't have this attribute.
            ChipLogProgress(AppServer, "Commodity Price server has no forecast");
            mForecastSupported = false;
            mForecastCount     = 0;
        }
        else
        {
            ChipLogError(AppServer, "Commodity Price attribute 0x%" PRIx32 " error: %" CHIP_ERROR_FORMAT, aPath.mAttributeId,
                         aStatus.ToChipError().Format());
        }
        return;
    }

    CHIP_ERROR err = CHIP_NO_ERROR;

    switch (aPath.mAttributeId)
    {
    case CommodityPrice::Attributes::TariffUnit::Id: {
        CommodityPrice::Attributes::TariffUnit::TypeInfo::DecodableType unit;
        SuccessOrExit(err = DataModel::Decode(*apData, unit));
        mTariffUnitKVAh = unit == Globals::TariffUnitEnum::kKVAh;
        break;
    }
    case CommodityPrice::Attributes::Currency::Id: {
        CommodityPrice::Attributes::Currency::TypeInfo::DecodableType currency;
        SuccessOrExit(err = DataModel::Decode(*apData, currency));
        mHasCurrency = !currency.IsNull();
        if (mHasCurrency)
        {
            mCurrency      = currency.Value().currency;
            mDecimalPoints = currency.Value().decimalPoints;
        }
        break;
    }
    case CommodityPrice::Attributes::CurrentPrice::Id: {
        CommodityPrice::Attributes::CurrentPrice::TypeInfo::DecodableType current;
        SuccessOrExit(err = DataModel::Decode(*apData, current));
        mHasCurrentPrice       = !current.IsNull();
        mCurrentDescription[0] = '\0';
        if (mHasCurrentPrice)
        {
            CopyPrice(current.Value(), mCurrentPrice);
            if (current.Value().description.HasValue())
            {
                const CharSpan & d = current.Value().description.Value();
                size_t len         = std::min(d.size(), sizeof(mCurrentDescription) - 1);
                memcpy(mCurrentDescription, d.data(), len);
                mCurrentDescription[len] = '\0';
            }
        }
        break;
    }
    case CommodityPrice::Attributes::PriceForecast::Id: {
        if (aPath.IsListItemOperation())
        {
            // A list too big for one report arrives as an empty list, then one append per entry.
            CommodityPrice::Structs::CommodityPriceStruct::DecodableType entry;
            SuccessOrExit(err = DataModel::Decode(*apData, entry));
            if (mForecastCount < kMaxForecastEntries)
            {
                CopyPrice(entry, mForecast[mForecastCount++]);
            }
            break;
        }

        CommodityPrice::Attributes::PriceForecast::TypeInfo::DecodableType forecast;
        SuccessOrExit(err = DataModel::Decode(*apData, forecast));
        mForecastSupported = true;
        mForecastCount     = 0;
        auto iter          = forecast.begin();
        while (iter.Next() && mForecastCount < kMaxForecastEntries)
        {
            CopyPrice(iter.GetValue(), mForecast[mForecastCount++]);
        }
        SuccessOrExit(err = iter.GetStatus());
        break;
    }
    default:
        return;
    }

    PricesChanged();

exit:
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(AppServer, "Failed to decode Commodity Price attribute 0x%" PRIx32 ": %" CHIP_ERROR_FORMAT,
                     aPath.mAttributeId, err.Format());
    }
}

void PriceSubscriber::OnSubscriptionEstablished(SubscriptionId aSubscriptionId)
{
    ChipLogProgress(AppServer, "Commodity Price subscription established (id 0x%" PRIx32 ")", aSubscriptionId);
}

void PriceSubscriber::OnError(CHIP_ERROR aError)
{
    // ReadClient keeps trying to resubscribe on its own.
    ChipLogError(AppServer, "Commodity Price subscription error: %" CHIP_ERROR_FORMAT, aError.Format());
}

void PriceSubscriber::OnDone(ReadClient * apReadClient)
{
    ChipLogProgress(AppServer, "Commodity Price subscription ended");
    if (mReadClient.get() == apReadClient)
    {
        mReadClient.reset();
    }
}

void PriceSubscriber::OnDeallocatePaths(ReadPrepareParams && aReadPrepareParams)
{
    // The paths are in sAttributePaths, nothing to free.
}

void PriceSubscriber::ClearPrices()
{
    mHasCurrency           = false;
    mHasCurrentPrice       = false;
    mCurrentDescription[0] = '\0';
    mForecastSupported     = false;
    mForecastCount         = 0;
    PricesChanged();
}

void PriceSubscriber::PricesChanged()
{
    DishwasherMgr().UpdateDishwasherDisplay();
}

bool PriceSubscriber::GetCurrentPrice(PriceInfo & info) const
{
    VerifyOrReturnValue(mHasCurrentPrice, false);
    info = mCurrentPrice;
    return true;
}

void PriceSubscriber::FormatPrice(int64_t price, char * buffer, size_t size) const
{
    FormatAmount(price, buffer, size);
    const size_t length = strlen(buffer);
    snprintf(buffer + length, size - length, "%s", mTariffUnitKVAh ? "/kVAh" : "/kWh");
}

void PriceSubscriber::FormatAmount(int64_t amount, char * buffer, size_t size) const
{
    FormatValue(amount, buffer, size);
    const size_t length = strlen(buffer);
    snprintf(buffer + length, size - length, "%s", CurrencySuffix());
}

const char * PriceSubscriber::CurrencySuffix() const
{
    static char numericCurrency[12];
    switch (mCurrency)
    {
    case 826:
        return " GBP";
    case 978:
        return " EUR";
    case 840:
        return " USD";
    default:
        if (!mHasCurrency)
        {
            return "";
        }
        snprintf(numericCurrency, sizeof(numericCurrency), " (%u)", mCurrency);
        return numericCurrency;
    }
}

void PriceSubscriber::FormatValue(int64_t amount, char * buffer, size_t size) const
{
    uint8_t decimals = mHasCurrency ? std::min<uint8_t>(mDecimalPoints, 9) : 0;
    uint64_t scale   = 1;
    for (uint8_t i = 0; i < decimals; i++)
    {
        scale *= 10;
    }

    uint64_t magnitude = amount < 0 ? static_cast<uint64_t>(-(amount + 1)) + 1 : static_cast<uint64_t>(amount);
    // Logging/printf on this platform doesn't do 64-bit integers reliably, so print the parts as 32-bit.
    unsigned long whole = static_cast<unsigned long>(magnitude / scale);
    unsigned long frac  = static_cast<unsigned long>(magnitude % scale);

    if (decimals == 0)
    {
        snprintf(buffer, size, "%s%lu", amount < 0 ? "-" : "", whole);
    }
    else
    {
        snprintf(buffer, size, "%s%lu.%0*lu", amount < 0 ? "-" : "", whole, static_cast<int>(decimals), frac);
    }
}

void PriceSubscriber::FormatUnit(char * buffer, size_t size) const
{
    // The currency without its leading space, e.g. "GBP/kWh".
    const char * currency = CurrencySuffix();
    snprintf(buffer, size, "%s%s", currency[0] == ' ' ? currency + 1 : currency, mTariffUnitKVAh ? "/kVAh" : "/kWh");
}

const char * PriceSubscriber::ShortLevelName(int16_t level)
{
    // The levels the price server uses, short enough to sit beside a price.
    static const char * const kShortLevels[] = { "V.Low", "Low", "Med", "High", "V.High" };
    return level >= 0 && level < static_cast<int16_t>(MATTER_ARRAY_SIZE(kShortLevels)) ? kShortLevels[level] : "?";
}

void PriceSubscriber::FormatCurrentPrice(char * buffer, size_t size) const
{
    if (!mHasCurrentPrice || !mCurrentPrice.hasPrice)
    {
        snprintf(buffer, size, "%s", IsBound() ? "Price unknown" : "No pricing available");
        return;
    }
    FormatPrice(mCurrentPrice.price, buffer, size);
}

void PriceSubscriber::FormatCurrentCost(char * buffer, size_t size) const
{
    if (!mHasCurrentPrice || !mCurrentPrice.hasPriceLevel)
    {
        FormatCurrentPrice(buffer, size);
        return;
    }

    // The levels the price server uses.
    static const char * const kLevels[] = { "Very Low", "Low", "Medium", "High", "Very High" };
    const int16_t level                 = mCurrentPrice.priceLevel;
    if (level >= 0 && level < static_cast<int16_t>(MATTER_ARRAY_SIZE(kLevels)))
    {
        snprintf(buffer, size, "Cost now: %s", kLevels[level]);
    }
    else
    {
        snprintf(buffer, size, "Cost now: level %d", level);
    }
}
