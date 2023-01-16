function start(c)
    c:startQuestNow();
end

function end_(c)
    local i = c:sendNext("Congratulations on earning your honorable #b<Beginner Adventurer>#k title. I wish you the best of luck in your future endeavors! Keep up the good work.\r\n\r\n#fUI/UIWindow.img/QuestIcon/4/0#\r\n #v1142107:# #t1142107# 1")
    if i == 1 then
        if (c:gainItems({ { id = 1142107, amount = 1 } })) then
            c:endQuestNow()
        else
            c:sendNext("Please make room in your inventory")
        end
    end
end
