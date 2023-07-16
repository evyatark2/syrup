function talk(c)
    local e = getEvent(Events.SUBWAY)
    local i
    if c:hasItem(4031713) then
        c:sendSimple("Here's the ticket reader.\r\n#b#L1##t4031713#", 1)
        if e:getProperty(Events.SAILING) == 0 then
            i = c:sendYesNo("It looks like there's plenty of room for this ride. Please have your ticket ready so I can let you in. The ride will be long, but you'll get to your destination just fine. What do you think? Do you want to get on this ride?")
        else
            subway_not_here(c)
            return
        end
    else
        c:sendOk("It seems you don't have a ticket! You can buy one from Bell.")
        return
    end

    if i == 1 then
        if (e:getProperty(Events.SAILING) == 0) then
            c:gainItems({ id = 4031713, amount = -1 })
            c:warp(600010002, 0)
        else
            subway_not_here(c)
        end
    else
        c:sendNext("You must have some business to take care of here, right?")
    end
end

function subway_not_here(c)
    c:sendOk("We will begin boarding 1 minute before the takeoff. Please be patient and wait for a few minutes. Be aware that the subway will take off right on time, and we stop receiving tickets 1 minute before that, so please make sure to be here on time.")
end
