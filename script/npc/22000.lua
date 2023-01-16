function talk(c)
    local i = 0
    while true do
        if i == -1 then
            c:sendOk("Hmm... I guess you still have things to do here?")
            break
        end

        if i == 0 then
            i = i + c:sendYesNo("Take this ship and you'll head off to a bigger continent. For #e150 mesos#n, I'll take you to #bVictoria Island#k. The thing is, once you leave this place, you can't ever come back. What do you think? Do you want to go to Victoria Island?")
        end

        if i == 1 then
            if c:hasItem(4031801) then
                i = i + c:sendNext("Okay, now give me 150 mesos... Hey, what's that? Is that the recommendation letter from Lucas, the chief of Amherst? Hey, you should have told me you had this. I, Shanks, recognize greatness when I see one, and since you have been recommended by Lucas, I see that you have a great, great potential as an adventurer. No way would I charge you for this trip!")
            else
                c:sendNext("Bored of this place? Here... Give me #e150 mesos#n first...")
                i = 20
            end
        end

        if i == 2 then
            i = i + c:sendPrevNext("Since you have the recommendation letter, I won't charge you for this. Alright, buckle up, because we're going to head to Victoria Island right now, and it might get a bit turbulent!!")
        end

        if i == 3 then
            c:gainItems({ { id = 4031801, amount = -1 } })
            c:warp(104000000, 0)
            break
        end

        if i == 20 then
            if c:level() > 6 then
                if (c:meso() < 150) then
                    c:sendOk("What? You're telling me you wanted to go without any money? You're one weirdo...")
                    break
                else
                    i = i + c:sendNext("Awesome! #e150#n mesos accepted! Alright, off to Victoria Island!")
                end
            else
                c:sendOk("Let's see... I don't think you are strong enough. You'll have to be at least Level 7 to go to Victoria Island.")
                break
            end
        end

        if i == 21 then
            c:gainMeso(-150)
            c:warp(104000000, 0)
            break
        end
    end
end
