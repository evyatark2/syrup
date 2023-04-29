function talk(c)
    if c:isQuestStarted(3900) and c:getQuestInfo(3900) ~= "5" then
        c:sendNext("#b(You drink the water from the oasis and feel refreshed.)", 2)
        c:setQuestInfo(3900, "5")
    end
end
