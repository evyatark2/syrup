--[[ 1052001 - Dark Lord ]]

function talk(c)
    local i = 0
    if c:job() == Job.BEGINNER then
        while true do
            if i == 0 then
                i = i + c:sendNext("Want to be a #rthief#k? There are some standards to meet. because we can't just accept EVERYONE in... #bYour level should be at least 10, with at least 20 DEX#k. Let's see.")
            end

            if i == 1 then
                if c:level() >= 10 then
                    i = i + c:sendYesNo("Oh...! You look like someone that can definitely be a part of us... all you need is a little sinister mind, and... yeah... so, what do you think? Wanna be the Rogue?")
                    if i == 0 then
                        break
                    end
                else
                    c:sendOk("Train a bit more until you reach the base requirements and I can show you the way of the #rWarrior#k.")
                    break
                end
            end

            if i == 2 then
                if c:gainItems({ { id = 2070015, amount = 500 }, { id = 1472061, amount = 1 }, { id = 1332063, amount = 1 } }) then
                    c:changeJob(Job.ROGUE)
                    c:resetStats()
                    i = i + 1
                else
                    c:sendNext("Make some room in your inventory and talk back to me.")
                    break
                end
            end

            if i == 3 then
                i = i + c:sendNext("Alright, from here out, you are a part of us! You'll be living the life of a wanderer at ..., but just be patient as soon, you'll be living the high life. Alright, it ain't much, but I'll give you some of my abilities... HAAAHHH!!!");
            end

            if i == 4 then
                i = i + c:sendPrevNext("You've gotten much stronger now. Plus every single one of your inventories have added slots. A whole row, to be exact. Go see for it yourself. I just gave you a little bit of #bSP#k. When you open up the #bSkill#k menu on the lower left corner of the screen, there are skills you can learn by using SP's. One warning, though: You can't raise it all together all at once. There are also skills you can acquire only after having learned a couple of skills first.");
            end

            if i == 5 then
                i = i + c:sendPrevNext("Now a reminder. Once you have chosen, you cannot change up your mind and try to pick another path. Go now, and live as a proud Thief.");
            end

            if i == 6 then
                break
            end
        end
    elseif c:job() == Job.ROGUE and c:level() >= 30 then
        while true do
            if i == 0 then
                if c:hasItem(4031011) then
                    c:sendOk("Go and see the #b#p1072003##k.")
                    break
                elseif c:hasItem(4031012) then
                    i = i + c:sendNext("I see you have done well. I will allow you to take the next step on your long road.");
                else
                    i = i + c:sendNext("The progress you have made is astonishing.")
                end
            end

            if i == 1 then
                if c:hasItem(4031012) then
                    i = i + c:sendSimple("Alright, when you have made your decision, click on [I'll choose my occupation] at the bottom.#b\r\n#L1#Please explain to me what being the Hunter is all about.\r\n#L2#Please explain to me what being the Crossbowman is all about.\r\n#L3#I'll choose my occupation!", 3)
                else
                    i = i + c:sendNext("Good decision. You look strong, but I need to see if you really are strong enough to pass the test, it's not a difficult test, so you'll do just fine. Here, take my letter first... make sure you don't lose it!");
                end
            end

            if i == 2 then
                if c:hasItem(4031012) then
                else
                    if c:hasItem(4031011) or c:gainItem({ { id = 4031011, amount = 1 } }) then
                        i = i + c:sendPrevNext("Please get this letter to #b#p1072003##k who's around #b#m102040000##k near Kerning City. He is taking care of the job of an instructor in place of me. Give him the letter and he'll test you in place of me. Best of luck to you.");
                    else
                        c:sendNext("Please, make some space in your inventory.")
                        break
                    end
                end
            end

            if i == 3 then
                if c:hasItem(4031012) then
                else
                    break
                end
            end
        end
    else
        c:sendOk("You have chosen wisely.")
    end
end
