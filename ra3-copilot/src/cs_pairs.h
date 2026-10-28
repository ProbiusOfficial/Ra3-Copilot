// cs_pairs.h - CommandSet key 映射表（自动生成，请勿手改）
// 生成器 : ra3-copilot/tools/gen_cs_pairs.py
// 数据源 : mod.imp 的 LogicCommandSet 目录项（base_key -> enh_key）
// 用途   : 0x1F「扩展模式列表」在 0x6CB344 把建造页的 cs key 换成同名 Enhanced 版
#pragma once
#include <cstdint>

namespace ra3 {

struct CsPair { uint32_t base; uint32_t enh; };

static const CsPair kCsPairs[] = {
    { 0x003019D1u, 0xFEADEF32u },   // JapanWarFactoryCommandSetBase  15->15
    { 0x19CD70D0u, 0x4E50F80Au },   // CelestialWarFactoryUpgradeCommandSetBase  15->10
    { 0x22A63EB3u, 0xEAB0A8EAu },   // SovietPowerPlantAdvancedCommandSet  None->None
    { 0x26AC879Fu, 0x0C37FFD2u },   // CelestialOutPostCommandSet  1->16
    { 0x330BA476u, 0x7E945880u },   // SovietConstructionYardCommandSet  17->19
    { 0x49209FBAu, 0x10BF5D9Fu },   // CelestialWarFactoryCommandSet  9->16
    { 0x51A38416u, 0x1ADD879Cu },   // JapanWarFactoryCommandSet  16->16
    { 0x5ECA3034u, 0xA73AB9CBu },   // SovietCraneCommandSet  15->17
    { 0x7BD00B9Eu, 0xB74C6077u },   // CelestialConstructionYardCommandSet  15->17
    { 0x7C1AE533u, 0xFD92743Cu },   // JapanConstructionYardCommandSet  16->20
    { 0x9B69CFCCu, 0xB481A48Au },   // AlliedConstructionYardCommandSetLevel3  16->18
    { 0x9CEBD1E5u, 0x00C8EB1Fu },   // CelestialWarFactoryTransitionCommandSet  10->16
    { 0x9EDC7B82u, 0x68950BAFu },   // JapanWarFactoryCommandSetTech3  16->16
    { 0xA492AAD3u, 0x1540ADCFu },   // SovietAirfieldCommandSet  6->7
    { 0xB67E88C7u, 0x66421C40u },   // SovietBarracksCommandSet  6->7
    { 0xBC17A605u, 0xD746F3A6u },   // AlliedConstructionYardCommandSetLevel2  16->18
    { 0xBC73BC2Du, 0x12CE7483u },   // CelestialWarFactoryUpgradeCommandSet  9->16
    { 0xC24ACBEAu, 0xC62913ECu },   // JapanWarFactoryCommandSetTech2  16->16
    { 0xC49E3569u, 0xFEC760AEu },   // AlliedConstructionYardCommandSetBase  15->17
    { 0xE1259A4Au, 0x57B775C7u },   // AlliedNavalYardCommandSet  8->9
    { 0xE36CF8D9u, 0xC60E6D9Fu },   // CelestialWarFactoryCommandSetBase  16->11
    { 0xEC7B195Eu, 0xAFD21A84u },   // AlliedWarFactoryCommandSet  16->11
    { 0xF27DA9BCu, 0xBB408578u },   // AlliedConstructionYardCommandSet  16->18
    { 0xFE64EB5Eu, 0x3929E633u },   // AlliedAirfieldCommandSet  8->9
};
static const int kCsPairCount = 24;

}  // namespace ra3

// 同 base_key 的其它分支（未采用，映射只能选一条）：
//   0x0x9CEBD1E5 -> 0x0xCA728F8C  CelestialWarFactoryTransitionCommandSet  10->11
