function talk(c)
    if c:isQuestStarted(2166) then
        c:sendNext("It's a beautiful, shiny rock. I can feel the mysterious power surrounding it.")
        c:setQuestInfo(2166, 5)
    else
        c:sendNext("I touched the shiny rock with my hand, and I felt a mysterious power flowing into my body.")
    end
end
