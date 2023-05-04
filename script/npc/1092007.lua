-- [[ 1092007 - Muirhat ]]

function talk(c)
    if c:isQuestStarted(2175) then
        c:sendOk("Please take this #b#t2030019##k, it will make your life a lot easier.  #i2030019#")
        if c:gainItems({ id = 2030019 }) then
            c:warp(100000006, 0)
        else
            c:sendOk("No free inventory spot available. Please make room in your USE inventory first.")
        end
    else
        c:sendOk("The Black Magician and his followers. Kyrin and the Crew of Nautilus. \n They'll be chasing one another until one of them doesn't exist, that's for sure.")
    end
end
