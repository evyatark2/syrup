function talk(c)
    local e = getEvent(Events.BOAT)
    local i
    if c:hasItem(4031045) then
        if (e:getProperty(Events.SAILING) == 0) then
            i = c:sendYesNo("Do you want to go to Orbis?")
        else
            boat_not_here(c)
            return
        end
    else
        c:sendOk("Make sure you got a Orbis ticket to travel in this boat. Check your inventory.")
        return
    end

    if i == 1 then
        if (e:getProperty(Events.SAILING) == 0) then
            c:gainItems({ id = 4031045, amount = -1 })
            c:warp(101000301, 0)
        else
            boat_not_here(c)
        end
    else
        c:sendOk("Okay, talk to me if you change your mind!");
    end
end

function boat_not_here(c)
    c:sendOk("The boat to Orbis is already travelling, please be patient for the next one.")
end
