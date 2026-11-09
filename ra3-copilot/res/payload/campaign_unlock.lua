-- 战役扩展与全科技模式（v4）：只打通「技能能正常用」的底座。
-- 协议技能本体交还玩家：局内 F12「协议技能」面板里自由勾选授予。
-- 开局默认只给扩展模式的抽卡（单抽 / 十连），不再一股脑授予全部技能。
-- 侧边栏 toggleUI("_cor") 的等长替换由启动器 C++ 侧完成，不在这里改 Lua 语法。
local CU_TAG = "RA3COPILOT_CU_V4_OK"
for i = 1, getn(PlayerRefList2), 1 do
local pn = PlayerRefList2[i]
if ObjectPlayerScriptName(pn) == pn then
if exPlayerTechUnlockBaseTechForPlayer ~= nil then exPlayerTechUnlockBaseTechForPlayer(pn) end
if exPlayerTechChangeTechFaction ~= nil then exPlayerTechChangeTechFaction(pn, "PlayerTechStore_Neutral") end
if exUnlockPlayerGenericRankTech ~= nil then for lv = 2, 10, 1 do exUnlockPlayerGenericRankTech(pn, lv) end end
ExecuteAction("PLAYER_SET_MAX_PLAYER_POWER_POINTS", pn, 99)
ExecuteAction("PLAYER_SET_PLAYER_POWER_POINTS", pn, 99)
end
end
ExecuteAction("PLAYER_GRANT_SPECIAL_POWER", "SpecialPower_LuckyUnitCrate", "<All Human Players>")
ExecuteAction("PLAYER_GRANT_SPECIAL_POWER", "SpecialPower_LuckyUnitCrateX10", "<All Human Players>")
ExecuteAction("SHOW_MILITARY_CAPTION", CU_TAG, 8)
