local names = { "Deep Forest of Patience 1", "Deep Forest of Patience 2", "Deep Forest of Patience 3" };
local maps = { 105040310, 105040312, 105040314 };

function talk(c)
    local zones = 0
    if c:isQuestStarted(2054) or c:isQuestComplete(2054) then
        zones = 3
    elseif c:isQuestStarted(2053) or c:isQuestComplete(2053) then
        zones = 2
    elseif c:isQuestStarted(2052) or c:isQuestComplete(2052) then
        zones = 1
    end

    c:sendNext("You feel a mysterious force surrounding this statue.")
    
    if zones ~= 0 then
        local selStr = "Its power allows you to will yourself deep inside the forest.#b"
        for i = 1, zones do
            selStr = selStr .. "\r\n#L" .. i .. "#" .. names[i] .. "#l"
        end

        local i = c:sendSimple(selStr, zones)

        c:warp(maps[i], 0)
    end
end
