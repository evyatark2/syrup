local c, quest, item, map = ...

if c:isQuestStarted(quest) then
    if c:hasItem(item) then
        if c:sendYesNo("Would you like to move to #b#m" .. map .. "##k?") == 1 then
            c:warp(map, 0)
        end
    else
        c:sendOk("The entrance is blocked by a force that can only be lifted by those holding an emblem.")
    end
else
    c:sendOk("The entrance is blocked by a strange force.")
end
