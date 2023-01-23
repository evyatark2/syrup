--[[ 1012100 - Ellinia's Regular Cab ]]

maps = { 104000000, 102000000, 100000000, 103000000, 120000000 };
costs = { 1000, 1000, 1000, 1000, 800 };

function talk(c)
    loadfile("script/npc/common/cab.lua")(c, "Regular Cab", maps, costs)
end
