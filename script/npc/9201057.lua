function talk(c)
    local i
    local map = c:map()
    if map == 103000100 or map == 600010001 then
        i = c:sendYesNo("The ride to " .. (map == 103000100 and "New Leaf City of Masteria" or "Kerning City of Victoria Island") .. " takes off every minute, beginning on the hour, and it'll cost you #b5000 mesos#k. Are you sure you want to purchase #b#t" .. (4031711 + map // 300000000) .. "##k?")
    else
        i = c:sendYesNo("Do you want to leave before the train start? There will be no refund.")
    end

    if i == 1 then
        if map == 103000100 or map == 600010001 then
            if c:meso() >= 5000 and c:gainItems({ id = 4031711 + map // 300000000 }) then
                c:gainMeso(-5000);
                c:sendNext("There you go.")
            elseif c:meso() >= 5000 then 
                c:sendNext("You don't have a etc. slot available.")
            else
                c:sendNext("You don't have enough mesos.")
            end
        else
            c:warp(map == 600010002 and 600010001 or 103000100, 0)
        end
    else
        c:sendNext("You must have some business to take care of here, right?")
    end
end
