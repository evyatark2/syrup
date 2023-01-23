--[[ 1012100 - Hensys' Regular Cab ]]

maps = { 104000000, 102000000, 101000000, 103000000, 120000000 };
costs = { 1000, 1000, 800, 1000, 800 };

function talk(c)
    loadfile("script/npc/common/cab.lua")(c, "Regular Cab", maps, costs)
end
