function talk(c)
    local e = getEvent(2)
    local i
    if (e:getProperty(0) == 0) then
        i = c:sendYesNo("It looks like there's plenty of room for this ride. Please have your ticket ready so I can let you in. The ride will be long, but you'll get to your destination just fine. What do you think? Do you want to get on this ride?")
    else
        c:sendOk("We will begin boarding 1 minute before the takeoff. Please be patient and wait for a few minutes. Be aware that the subway will take off right on time, and we stop receiving tickets 1 minute before that, so please make sure to be here on time.")
        return;
    end

    if i == 1 then
        if (e:getProperty(0) == 0) then
            c:warp(600010004, 0)
        else
            c:sendOk("We will begin boarding 1 minute before the takeoff. Please be patient and wait for a few minutes. Be aware that the subway will take off right on time, and we stop receiving tickets 1 minute before that, so please make sure to be here on time.")
        end
    end
end
