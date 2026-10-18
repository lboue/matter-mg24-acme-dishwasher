#pragma once

#include <app-common/zap-generated/cluster-objects.h>
#include <app/clusters/mode-base-server/mode-base-server.h>
#include <app/clusters/operational-state-server/operational-state-server.h>
#include <protocols/interaction_model/StatusCode.h>

namespace chip {
namespace app {
namespace Clusters {

namespace OperationalState {

class OperationalStateDelegate : public Delegate
{
public:
    DataModel::Nullable<uint32_t> GetCountdownTime() override;

    CHIP_ERROR GetOperationalStateAtIndex(size_t index, GenericOperationalState & operationalState) override;
    CHIP_ERROR GetOperationalPhaseAtIndex(size_t index, MutableCharSpan & operationalPhase) override;

    void HandlePauseStateCallback(GenericOperationalError & err) override;
    void HandleResumeStateCallback(GenericOperationalError & err) override;
    void HandleStartStateCallback(GenericOperationalError & err) override;
    void HandleStopStateCallback(GenericOperationalError & err) override;

private:
    const GenericOperationalState opStateList[4] = {
        GenericOperationalState(to_underlying(OperationalStateEnum::kStopped)),
        GenericOperationalState(to_underlying(OperationalStateEnum::kRunning)),
        GenericOperationalState(to_underlying(OperationalStateEnum::kPaused)),
        GenericOperationalState(to_underlying(OperationalStateEnum::kError)),
    };
    DataModel::List<const GenericOperationalState> mOperationalStateList = Span<const GenericOperationalState>(opStateList);

    const CharSpan phaseList[5] = { "pre-soak"_span, "main-wash"_span, "rinse"_span, "final-rinse"_span, "drying"_span };
    Span<const CharSpan> mOperationalPhaseList                         = Span<const CharSpan>(phaseList);
};

Instance * GetInstance();
OperationalStateDelegate * GetDelegate();

} // namespace OperationalState

namespace DishwasherMode {

const uint8_t ModeEco   = 0;
const uint8_t ModeChef  = 1;
const uint8_t ModeQuick = 2;
const uint8_t ModeHeavy = 3;

class DishwasherModeDelegate : public ModeBase::Delegate
{
public:
    CHIP_ERROR GetModeLabelByIndex(uint8_t modeIndex, MutableCharSpan & label) override;

    static constexpr uint8_t kModeCount = 4;

private:
    using ModeTagStructType              = detail::Structs::ModeTagStruct::Type;
    ModeTagStructType modeTagsEco[1]   = { { .value = to_underlying(ModeTag::kNormal) } };
    ModeTagStructType modeTagsChef[2]  = { { .value = to_underlying(ModeBase::ModeTag::kMax) },
                                           { .value = to_underlying(ModeTag::kHeavy) } };
    ModeTagStructType modeTagsQuick[3] = { { .value = to_underlying(ModeTag::kLight) },
                                           { .value = to_underlying(ModeBase::ModeTag::kNight) },
                                           { .value = to_underlying(ModeBase::ModeTag::kQuiet) } };
    ModeTagStructType modeTagsHeavy[1] = { { .value = to_underlying(ModeTag::kHeavy) } };

    // These are the modes on my dishwasher.
    // TODO Implement these with the appropriate ModeTags.
    //
    // Eco 50°
    // Chef 70°
    // Auto 45° - 65°
    // Glass 40°
    // Silence 50°
    // Pre Rinse
    // Quick 45°
    // Short 60°
    // Machine Care
    const detail::Structs::ModeOptionStruct::Type kModeOptions[kModeCount] = {
        detail::Structs::ModeOptionStruct::Type{ .label    = CharSpan::fromCharString("Eco 50°"),
                                                 .mode     = ModeEco,
                                                 .modeTags = DataModel::List<const ModeTagStructType>(modeTagsEco) },
        detail::Structs::ModeOptionStruct::Type{ .label    = CharSpan::fromCharString("Chef 70°"),
                                                 .mode     = ModeChef,
                                                 .modeTags = DataModel::List<const ModeTagStructType>(modeTagsChef) },
        detail::Structs::ModeOptionStruct::Type{ .label    = CharSpan::fromCharString("Quick 45°"),
                                                 .mode     = ModeQuick,
                                                 .modeTags = DataModel::List<const ModeTagStructType>(modeTagsQuick) },
        detail::Structs::ModeOptionStruct::Type{ .label    = CharSpan::fromCharString("Heavy 65°"),
                                                 .mode     = ModeHeavy,
                                                 .modeTags = DataModel::List<const ModeTagStructType>(modeTagsHeavy) }
    };

    CHIP_ERROR Init() override;
    void HandleChangeToMode(uint8_t mode, ModeBase::Commands::ChangeToModeResponse::Type & response) override;
    CHIP_ERROR GetModeValueByIndex(uint8_t modeIndex, uint8_t & value) override;
    CHIP_ERROR GetModeTagsByIndex(uint8_t modeIndex, DataModel::List<ModeTagStructType> & tags) override;
};

ModeBase::Instance * GetInstance();
DishwasherModeDelegate * GetDelegate();

} // namespace DishwasherMode

} // namespace Clusters
} // namespace app
} // namespace chip
