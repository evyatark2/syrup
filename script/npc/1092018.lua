function talk(c)
    if not c:isQuestComplete(2162) and not c:hasItem(4031839) then
        if c:gainItems({ { id = 4031839, amount = 1 } }) then
            c:sendNext("(You retrieved a Crumpled Paper standing out of the trash can. It's content seems important.)", 2)
        else
            c:sendNext("(You see a Crumpled Paper standing out of the trash can. It's content seems important, but you can't retrieve it since your inventory is full.)", 2)
        end
    end
end
