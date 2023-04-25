function talk(c)
    local e = getEvent(1)
    local i
    if (e:getProperty(0) == 0) then
        i = c:sendYesNo("Do you want to go to Orbis?")
    else
        c:sendOk("The train to Orbis is already travelling, please be patient for the next one.")
        return;
    end


    if i == 1 then
        if (e:getProperty(0) == 0) then
            c:warp(220000111, 0)
        else
            c:sendOk("The train to Orbis is ready to take off, please be patient for the next one.")
        end
    else
        c:sendOk("Okay, talk to me if you change your mind!");
    end
end
