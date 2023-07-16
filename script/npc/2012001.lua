function talk(c)
    local e = getEvent(Events.BOAT)
    local i
    if c:hasItem(4031047) then
        if (e:getProperty(Events.SAILING) == 0) then
            i = c:sendYesNo("Do you want to go to Ellinia?")
        else
            boat_not_here(c)
            return
        end
    else
        c:sendOk("Make sure you got a Ellinia ticket to travel in this boat. Check your inventory.")
        return
    end

    if i == 1 then
        if (e:getProperty(Events.SAILING) == 0) then
            c:gainItems({ id = 4031047, amount = -1 })
            c:warp(200000112, 0)
        else
            boat_not_here(c)
        end
    else
        c:sendOk("Okay, talk to me if you change your mind!");
    end
end

function boat_not_here(c)
    c:sendOk("The boat to Ellinia is ready to take off, please be patient for the next one.")
end
