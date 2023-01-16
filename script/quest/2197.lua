function end_(c)
    local i = c:sendNext("Oh, you already have monster book. Good luck on your journey~!")
    if i == 1 then
        c:endQuestNow()
    end
end
