#pragma once

#include <app/ReadClient.h>
#include <lib/support/Span.h>
#include <platform/CHIPDeviceLayer.h>

#include <memory>

/**
 * Reads electricity prices from a Commodity Price server (e.g. an Electrical Energy Tariff device).
 *
 * Matter devices can't discover each other, so the server is found through a Binding on the
 * dishwasher endpoint:
 *
 *   chip-tool binding write binding '[{"node":<tariff>,"endpoint":<ep>,"cluster":149}]' <dishwasher> 1
 *
 * (and the tariff device needs an ACL entry giving the dishwasher View). We then hold one
 * subscription to TariffUnit, Currency, CurrentPrice and PriceForecast. ReadClient re-establishes
 * CASE and resubscribes by itself if the tariff device goes away.
 *
 * Everything here runs on the Matter thread.
 */
class PriceSubscriber : public chip::app::ReadClient::Callback
{
public:
    struct PriceInfo
    {
        uint32_t periodStart = 0; // Matter epoch seconds
        uint32_t periodEnd   = 0; // 0 when open-ended
        bool hasPrice        = false;
        int64_t price        = 0; // in Currency.decimalPoints units, per TariffUnit
        bool hasPriceLevel   = false;
        int16_t priceLevel   = 0;
    };

    static constexpr size_t kMaxForecastEntries = 48; // 24 h of half-hour slots

    static PriceSubscriber & Instance() { return sInstance; }

    /** Call with the stack locked, after Server init. */
    void Init();

    /** Re-read the binding table and (re)subscribe. */
    void Refresh();

    bool IsBound() const { return mReadClient != nullptr; }
    bool GetCurrentPrice(PriceInfo & info) const;
    const char * GetCurrentPriceDescription() const { return mCurrentDescription; }
    bool IsForecastSupported() const { return mForecastSupported; }
    chip::Span<const PriceInfo> GetForecast() const { return chip::Span<const PriceInfo>(mForecast, mForecastCount); }

    /** e.g. "24.567 GBP/kWh". */
    void FormatPrice(int64_t price, char * buffer, size_t size) const;

    /** The current price for the status line, or "no price". */
    void FormatCurrentPrice(char * buffer, size_t size) const;

    // ReadClient::Callback
    void OnAttributeData(const chip::app::ConcreteDataAttributePath & aPath, chip::TLV::TLVReader * apData,
                         const chip::app::StatusIB & aStatus) override;
    void OnSubscriptionEstablished(chip::SubscriptionId aSubscriptionId) override;
    void OnError(CHIP_ERROR aError) override;
    void OnDone(chip::app::ReadClient * apReadClient) override;
    void OnDeallocatePaths(chip::app::ReadPrepareParams && aReadPrepareParams) override;

private:
    static PriceSubscriber sInstance;

    static void OnPlatformEvent(const chip::DeviceLayer::ChipDeviceEvent * event, intptr_t arg);

    void ClearPrices();
    void PricesChanged();

    std::unique_ptr<chip::app::ReadClient> mReadClient;

    bool mHasCurrency      = false;
    uint16_t mCurrency     = 0; // ISO 4217 numeric code
    uint8_t mDecimalPoints = 0;
    bool mTariffUnitKVAh   = false;

    bool mHasCurrentPrice = false;
    PriceInfo mCurrentPrice;
    char mCurrentDescription[33] = "";

    bool mForecastSupported = false;
    PriceInfo mForecast[kMaxForecastEntries];
    size_t mForecastCount = 0;
};
