local prizes = { { 4010000, 3 }, { 4010001, 3 }, { 4010002, 3 }, { 4010003, 3 }, { 4010004, 3 }, { 4010005, 3 } };

function talk(c)
    if c:isQuestStarted(2052) then
        if not c:gainItems({ { id = 4031025, amount = 10 } }) then
            c:sendNext("Check for an available slot in your ETC inventory.");
            return
        end
    else
        local prize = prizes[math.random(#prizes)]
        if not c:gainItems({ {  id = prize[0], amount = prize[1] } }) then
            c:sendNext("Check for an available slot in your ETC inventory.");
            return
        end
    end

    c:warp(105040300, 0)
end
