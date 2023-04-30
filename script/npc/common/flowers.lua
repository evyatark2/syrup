local c, quest, reward, prizes, count = ...

if c:isQuestStarted(quest) then
    if not c:gainItems({ { id = reward, amount = (quest - 2051) * 10 } }) then
        c:sendNext("Check for an available slot in your ETC inventory.");
        return
    end
else
    local prize = prizes[math.random(#prizes)]
    if not c:gainItems({ {  id = prize[0], amount = count } }) then
        c:sendNext("Check for an available slot in your ETC inventory.");
        return
    end
end

c:warp(105040300, 0)
