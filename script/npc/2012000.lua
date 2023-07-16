local destinations = {
    { ticket = 4031047, cost = 5000, mapName1 = "Ellinia of Victoria Island", mapName2 = "Ellinia of Victoria Island" },
    { ticket = 4031074, cost = 6000, mapName1 = "Ludibrium", mapName2 = "Ludibrium" },
    { ticket = 4031331, cost = 30000, mapName1 = "Leafre", mapName2 = "Leafre of Minar Forest" },
    { ticket = 4031576, cost = 6000, mapName1 = "Ariant", mapName2 = "Nihal Desert" },
}

function talk(c)
    local str = "Hello, I'm in charge of selling tickets for the ship ride for every destination. Which ticket would you like to purchase?"
    for i = 1, #destinations do
        str = str .. "\r\n#L" .. i .. "##b" .. destinations[i].mapName1 .. "#k#l"
    end

    local sel = c:sendSimple(str, #destinations)
    local destination = destinations[sel]

    local i = c:sendYesNo("The ride to " .. destination.mapName2 .. " takes off every " .. (sel == 1 and 15 or 10) .. " minutes, beginning on the hour, and it'll cost you #b" .. destination.cost .. " mesos#k. Are you sure you want to purchase #b#t" .. destination.ticket .. "##k?")
    if i == 1 then
        if c:meso() >= destination.cost and c:gainItems({ id = destination.ticket }) then
            c:gainMeso(-destination.cost)
        else
            c:sendOk("Are you sure you have #b" .. destination.cost .. " mesos#k? If so, then I urge you to check you etc. inventory, and see if it's full or not.")
        end
    end
end
