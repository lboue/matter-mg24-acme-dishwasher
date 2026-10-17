#pragma once

#include <app/ReadClient.h>
#include <lib/support/Span.h>
#include <platform/CHIPDeviceLayer.h>

#include <memory>

/**
 * Reads the grid's carbon intensity from an Electrical Grid Conditions server, the same way
 * PriceSubscriber reads prices. The server is found through a Binding on the dishwasher endpoint:
 *
 *   chip-tool binding write binding '[{"node":<grid>,"endpoint":<ep>,"cluster":160}]' <dishwasher> 1
 *
 * A binding write replaces the whole table, so list the Commodity Price binding in the same write.
 * We hold one subscription to CurrentConditions and ForecastConditions.
 *
 * Everything here runs on the Matter thread.
 */
class GridConditionsSubscriber : public chip::app::ReadClient::Callback
{
public:
    struct Conditions
    {
        uint32_t periodStart        = 0; // Matter epoch seconds
        uint32_t periodEnd          = 0; // 0 when open-ended
        int16_t gridCarbonIntensity = 0; // gCO2/kWh
    };

    static constexpr size_t kMaxForecastEntries = 48; // 24 h of half-hour slots

    static GridConditionsSubscriber & Instance() { return sInstance; }

    /** Call with the stack locked, after PriceSubscriber::Init() (which sets up the binding manager). */
    void Init();

    /** Re-read the binding table and (re)subscribe. */
    void Refresh();

    bool IsBound() const { return mReadClient != nullptr; }
    bool GetCurrentConditions(Conditions & conditions) const;
    bool IsForecastSupported() const { return mForecastSupported; }
    chip::Span<const Conditions> GetForecast() const { return chip::Span<const Conditions>(mForecast, mForecastCount); }

    // ReadClient::Callback
    void OnAttributeData(const chip::app::ConcreteDataAttributePath & aPath, chip::TLV::TLVReader * apData,
                         const chip::app::StatusIB & aStatus) override;
    void OnSubscriptionEstablished(chip::SubscriptionId aSubscriptionId) override;
    void OnError(CHIP_ERROR aError) override;
    void OnDone(chip::app::ReadClient * apReadClient) override;
    void OnDeallocatePaths(chip::app::ReadPrepareParams && aReadPrepareParams) override;

private:
    static GridConditionsSubscriber sInstance;

    static void OnPlatformEvent(const chip::DeviceLayer::ChipDeviceEvent * event, intptr_t arg);

    void ClearConditions();
    void ConditionsChanged();

    std::unique_ptr<chip::app::ReadClient> mReadClient;

    bool mHasCurrentConditions = false;
    Conditions mCurrentConditions;

    bool mForecastSupported = false;
    Conditions mForecast[kMaxForecastEntries];
    size_t mForecastCount = 0;
};
