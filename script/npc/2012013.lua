function talk(c)
    local e = getEvent(1)
    local i
    if (e:getProperty(0) == 0) then
        i = c:sendYesNo("Do you want to go to Ludibrium?")
    else
        c:sendOk("The train to Ludibrium is already travelling, please be patient for the next one.")
        return;
    end


    if i == 1 then
        if (e:getProperty(0) == 0) then
            c:warp(200000122, 0)
        else
            c:sendOk("The train to Ludibrium is ready to take off, please be patient for the next one.")
        end
    else
        c:sendOk("Okay, talk to me if you change your mind!");
    end
end
