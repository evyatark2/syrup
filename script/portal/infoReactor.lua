function enter(c)
    if c:isQuestStarted(1008) then
        c:showInfo("UI/tutorial.img/22")
    elseif c:isQuestStarted(1020) then
        c:showInfo("UI/tutorial.img/27")
    end
    c:enableActions()
end
