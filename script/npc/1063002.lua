local prizes = { 4010006, 4010007, 4020007 };

local quest, reward, prizes, count = ...
function talk(c)
    loadfile("script/npc/common/flowers.lua")(c, 2053, 4031028, prizes, 4)
end
