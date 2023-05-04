function start(c)
    local i = 0
    while true do
        if i == 0 then
            local gender
            if c:gender() == 0 then gender = "Man" else gender = "Miss" end
            i = i + c:sendNext("Hey, " .. gender .. "~ What's up? Haha! I am Roger who can teach you adorable new Maplers lots of information.")
        end
        if i == 1 then
            i = i + c:sendPrevNext("You are asking who made me do this? Ahahahaha!\r\nMyself! I wanted to do this and just be kind to you new travellers.")
        end
        if i == 2 then
            i = i + c:sendAcceptDecline("So..... Let me just do this for fun! Abaracadabra~!")
            if (i == 1) then return end
        end
        if i == 3 then
            if c:hp() > 25 then
                c:setHp(25)
            end

            if not c:hasItem(2010007) then
                -- We are assuming that at this point the player must have empty inventory slots
                -- so c:gainItems cannot fail
                c:gainItems({ id = 2010007  })
            end

            c:startQuestNow()

            i = i + c:sendNext("Surprised? If HP becomes 0, then you are in trouble. Now, I will give you #rRoger's Apple#k. Please take it. You will feel stronger. Open the Item window and double click to consume. Hey, it's very simple to open the Item window. Just press #bI#k on your keyboard.")
        end
        if i == 4 then
            i = i + c:sendPrev("Please take all Roger's Apples that I gave you. You will be able to see the HP bar increasing. Please talk to me again when you recover your HP 100%.")
        end
        if i == 5 then
            c:showInfo("UI/tutorial.img/28")
            break
        end
    end
end

function end_(c)
    local i = 0
    while true do
        if i == 0 then
            if c:hp() < 50 then
                c:sendNext("Hey, your HP is not fully recovered yet. Did you take all the Roger's Apple that I gave you? Are you sure?")
                return
            else
                i = i + c:sendNext("How easy is it to consume the item? Simple, right? You can set a #bhotkey#k on the right bottom slot. Haha you didn't know that! right? Oh, and if you are a beginner, HP will automatically recover itself as time goes by. Well it takes time but this is one of the strategies for the beginners.")
            end
        end
        if i == 1 then
            i = i + c:sendPrevNext("Alright! Now that you have learned alot, I will give you a present. This is a must for your travel in Maple World, so thank me! Please use this under emergency cases!")
        end
        if i == 2 then
            i = i + c:sendPrev("Okay, this is all I can teach you. I know it's sad but it is time to say good bye. Well take care if yourself and Good luck my friend!\r\n\r\n#fUI/UIWindow.img/QuestIcon/4/0#\r\n#v2010000# 3 #t2010000#\r\n#v2010009# 3 #t2010009#\r\n\r\n#fUI/UIWindow.img/QuestIcon/8/0# 10 exp")
        end
        if i == 3 then
            if c:gainItems({ { id = 2010000, amount = 3 }, { id = 2010009, amount = 3 } }) then
                c:gainExp(10)
                c:endQuestNow()
                break
            else
                c:dropMessage(1, "Your inventory is full")
            end
        end
    end
end
