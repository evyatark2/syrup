function talk(c)
    local e = getEvent(Events.TRAIN)
    local i
    if c:hasItem(4031045) then
        if e:getProperty(Events.SAILING) == 0 then
            i = c:sendYesNo("Do you want to go to Orbis?")
        else
            train_not_here(c)
            return
        end
    else
        c:sendOk("Make sure you got a Orbis ticket to travel in this train. Check your inventory.")
        return
    end


    if i == 1 then
        if e:getProperty(Events.SAILING) == 0 then
            c:gainItems({ id = 4031045, amount = -1 })
            c:warp(220000111, 0)
        else
            train_not_here(c)
        end
    else
        c:sendOk("Okay, talk to me if you change your mind!");
    end
end

function train_not_here(c)
    c:sendOk("The train to Orbis is ready to take off, please be patient for the next one.")
end
