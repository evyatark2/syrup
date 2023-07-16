local COST = 5000

function talk(c)
    local i = c:sendYesNo("Hello, I'm in charge of selling tickets for the ship ride to Orbis Station of Ossyria. The ride to Orbis takes off every 15 minutes, beginning on the hour, and it'll cost you #b" .. COST .. " mesos#k. Are you sure you want to purchase #b#t4031045##k?")

    if i == 1 then
        if c:meso() >= COST and c:gainItems({ id = 4031045 }) then
            c:gainMeso(-COST)
        else
            c:sendOk("Are you sure you have #b" .. COST .. " mesos#k? If so, then I urge you to check your etc. inventory, and see if it's full or not.")
        end
    else
        c:sendNext("You must have some business to take care of here, right?")
    end
end
