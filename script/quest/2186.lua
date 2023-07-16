function end_(c)
    if (c:hasItem(4031853)) then
        c:sendOk("Geez, you found my glasses! Thank you, thank you so much. Now I'm able to see everything again!")
        if c:gainItems({ { id = 2030019, amount = 10 }, { id = 4031853, amount = -1 } }) then
            c:endQuestNow()
            c:gainExp(1700)
        else
            c:sendOk("I need you to have an USE slot available to reward you properly!")
        end
    elseif c:hasItem(4031854) or c:hasItem(4031855) then
        c:sendOk("Hm, those aren't my glasses... But alas, I'll take it anyway. Thanks.")
        if c:gainItems({ { id = 2030019, amount = 5 }, { id = 4031854, amount = -1 } }) or
            c:gainItems({ { id = 2030019, amount = 5 }, { id = 4031855, amount = -1 } }) then
            c:endQuestNow()
            c:gainExp(1000)
        else
            c:sendOk("I need you to have an USE slot available to reward you properly!")
        end
    end
end
