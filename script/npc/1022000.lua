--[[ 1022000 - Dances with Balrog ]]

function talk(c)
    local i = 0
    if c:job() == Job.BEGINNER then
        while true do
            if i == 0 then
                i = i + c:sendNext("Do you want to become a #rwarrior#k? You need to meet some criteria in order to do so.#b You should be at least in level 10, and at least 35 STR#k. Let's see...")
            end

            if i == 1 then
                if c:level() >= 10 then
                    i = i + c:sendPrevNext("It is an important and final choice. You will not be able to turn back.");
                else
                    c:sendOk("Train a bit more until you reach the base requirements and I can show you the way of the #rWarrior#k.")
                    break
                end
            end

            if i == 2 then
                if c:gainItems({ { id = 1302077, amount = 1 } }) then
                    c:changeJob(Job.SWORDSMAN)
                    c:resetStats()
                    i = i + 1
                else
                    c:sendNext("Make some room in your inventory and talk back to me.")
                    break
                end
            end

            if i == 3 then
                i = i + c:sendNext("From here on out, you are going to the Warrior path. This is not an easy job, but if you have discipline and confidence in your own body and skills, you will overcome any difficulties in your path. Go, young Warrior!")
            end

            if i == 4 then
                i = i + c:sendPrevNext("You've gotten much stronger now. Plus every single one of your inventories have added slots. A whole row, to be exact. Go see for it yourself. I just gave you a little bit of #bSP#k. When you open up the #bSkill#k menu on the lower left corner of the screen, there are skills you can learn by using SP's. One warning, though: You can't raise it all together all at once. There are also skills you can acquire only after having learned a couple of skills first.")
            end

            if i == 5 then
                i = i + c:sendPrevNext("Now a reminder. Once you have chosen, you cannot change up your mind and try to pick another path. Go now, and live as a proud Warrior.")
            end

            if i == 6 then
                break
            end
        end
    elseif c:job() == Job.SWORDSMAN and c:level() >= 30 then
        while true do
            if i == -1 then
            end

            if i == 0 then
                if c:hasItem(4031008) then
                    c:sendOk("Go and see the #b#p1072000##k.")
                    break
                elseif c:hasItem(4031012) then
                    i = i + c:sendNext("Haha...I knew you'd breeze through that test. I'll admit, you are a great bowman. I'll make you much stronger than you're right now. before that, however... you;ll need to choose one of two paths given to you. It'll be a difficult decision for you to make, but... if there's any question to ask, please do so.")
                else
                    i = i + c:sendNext("The progress you have made is astonishing.")
                end
            end

            if i == 1 then
                if c:hasItem(4031012) then
                    i = i + c:sendSimple("Alright, when you have made your decision, click on [I'll choose my occupation] at the bottom.#b\r\n#L0#Please explain to me what being the Hunter is all about.\r\n#L1#Please explain to me what being the Crossbowman is all about.\r\n#L3#I'll choose my occupation!")
                else
                    i = i + c:sendNext("Good decision. You look strong, but I need to see if you really are strong enough to pass the test, it's not a difficult test, so you'll do just fine. Here, take my letter first... make sure you don't lose it!")
                end
            end

            if i == 2 then
                if c:hasItem(4031012) then
                else
                    if c:hasItem(4031008) or c:gainItem({ { id = 4031008, amount = 1 } }) then
                        i = i + c:sendPrevNext("Please get this letter to #b#p1072000##k who's around #b#m102020300##k near Perion. He is taking care of the job of an instructor in place of me. Give him the letter and he'll test you in place of me. Best of luck to you.")
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
