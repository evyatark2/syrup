--[[ 1012100 - Nautilus' Mid-Sized Taxi ]]

local c, name, maps, costs = ...

c:sendNext("Hello, I drive the " .. name .. ". If you want to go from town to town safely and fast, then ride our cab. We'll glady take you to your destination with an affordable price.")

local multiplier = 1
local str = ""
if c:job() == Job.BEGINNER then
    multiplier = 10
    str = "We have a special 90% discount for beginners."
end

str = str .. "Choose your destination, for fees will change from place to place.#b"

for i = 1, #maps do
    str = str .. "\r\n#L" .. i .. "##m" .. maps[i] .. "# (" .. (costs[i] // multiplier) .. " mesos)#l"
end

local sel = c:sendSimple(str, #maps);

local cost
if c:job() == Job.BEGINNER then
    cost = costs[sel] // 10
else
    cost = costs[sel]
end

local y = c:sendYesNo("You don't have anything else to do here, huh? Do you really want to go to #b#m" .. maps[sel] .. "##k? It'll cost you #b" .. cost .. " mesos#k.")
if y == 1 then
    if c:meso() < cost then
        c:sendNext("You don't have enough mesos. Sorry to say this, but without them, you won't be able to ride the cab.")
        return
    else
        c:gainMeso(-cost)
        c:warp(maps[sel], 0)
        return
    end
else
    c:sendNext("There's a lot to see in this town, too. Come back and find us when you need to go to a different town.")
    return
end
