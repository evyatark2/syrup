function talk(c)
    local e = getEvent(Events.GENIE)
    local i
    if c:hasItem(4031576) then
        if e:getProperty(Events.SAILING) == 0 then
            i = c:sendYesNo("This will not be a short flight, so you need to take care of some things, I suggest you do that first before getting on board. Do you still wish to board the genie?")
        else
            genie_not_here(c)
            return
        end
    else
        c:sendOk("Make sure you got an Ariant ticket to travel in this genie. Check your inventory.")
        return
    end

    if i == 1 then
        if e:getProperty(Events.SAILING) == 0 then
            c:gainItems({ id = 4031576, amount = -1 })
            c:warp(200000152, 0)
        else
            genie_not_here(c)
        end
    else
        c:sendOk("Okay, talk to me if you change your mind!");
    end
end

function genie_not_here(c)
    c:sendOk("This genie is getting ready for takeoff. I'm sorry, but you'll have to get on the next ride. The ride schedule is available through the guide at the ticketing booth.")
end
