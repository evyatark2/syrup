function talk(c)
    local e = getEvent(0)
    local i
    if (e:getProperty(0) == 0) then
        i = c:sendYesNo("Do you want to go to Ellinia?")
    else
        c:sendOk("The boat to Ellinia is already travelling, please be patient for the next one.")
        return;
    end

    if i == 1 then
        if (e:getProperty(0) == 0) then
            c:warp(200000112, 0)
        else
            c:sendOk("The boat to Ellinia is ready to take off, please be patient for the next one.")
        end
    else
        c:sendOk("Okay, talk to me if you change your mind!");
    end
end
