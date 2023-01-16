--[[
    9201123 - Warrior Statue
 --]]

function talk(c)
    local i = 0
    while true do
        if i == -1 then
            c:sendOk("Come back to me if you decided to be a #bWarrior#k.")
            break
        end

        if i == 0 then
            if c:job() == Job.BEGINNER then
                if c:level() >= 10 or c:isQuestStarted(2077) then
                    i = i + c:sendYesNo("Hey #h #, I can send you to #b#m102000003##k if you want to be a #bWarrior#k. Do you want to go now?")
                else
                    c:sendOk("If you want to be a #bWarrior#k, train yourself further until you reach #blevel 10#k.")
                    break
                end
            else
                c:sendOk("You're much stronger now. Keep training!")
                break
            end
        end

        if i == 1 then
            c:warp(102000003, 0)
            break
        end
    end
end
