function talk(c)
    local e = getEvent(3)
    local i
    if (e:getProperty(0) == 0) then
        i = c:sendYesNo("This will not be a short flight, so you need to take care of some things, I suggest you do that first before getting on board. Do you still wish to board the genie?")
    else
        c:sendOk("The genie already took off. I'm sorry, but you'll have to get on the next ride. The ride schedule is available through the guide at the ticketing booth.")
        return;
    end

    if i == 1 then
        if (e:getProperty(0) == 0) then
            c:warp(200000152, 0)
        else
            c:sendOk("This genie is getting ready for takeoff. I'm sorry, but you'll have to get on the next ride. The ride schedule is available through the guide at the ticketing booth.")
        end
    else
        c:sendOk("Okay, talk to me if you change your mind!");
    end
end
