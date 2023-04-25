function talk(c)
    local e = getEvent(0)
    if (e:getProperty(0) == 0) then
        c:warp(101000301, 0)
    else
        c:sendOk("The boat is currently sailing");
    end
end
