--[[ 1012100 - Nautilus' Mid-Sized Taxi ]]

maps = { 104000000, 102000000, 100000000, 101000000, 103000000 };
costs = { 1000, 1000, 1000, 800, 1000 };

function talk(c)
    loadfile("script/npc/common/cab.lua")(c, "Nautilus' Mid-Sized Taxi", maps, costs)
end
