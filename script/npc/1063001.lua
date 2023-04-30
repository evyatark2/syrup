local prizes = { 4020000, 4020002, 4020006 };

local quest, reward, prizes, count = ...
function talk(c)
    loadfile("script/npc/common/flowers.lua")(c, 2053, 4031026, prizes, 4)
end
