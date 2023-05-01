-- [[ Zombie Mushroom Signal 3 ]]

function end_(c)
    if c:hasItem(4032399, 20) then
        c:sendOk("Oh, you brought 20 #b#t4032399##k! Thank you.")
        -- Gained from Act.img
        -- c:gainItems({ { id = 4032399, amount = -20 } })
        -- c:gainExp(8000)
        c:endQuestNow()
    else
        c:sendOk("Please bring me 20 #b#t4032399##k... #i4032399#")
    end
end
