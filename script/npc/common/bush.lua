local c = ...

if c:isQuestStarted(2186) then
    c:sendNext("Do you want to obtain a glasses?");
    if c:hasItem(4031853) or c:hasItem(4031854) or c:hasItem(4031855) then
        c:sendOk("You #balready have#k the glasses that was here!")
    else
        local rolled = math.random(3)
        c:gainItems({ id = 4031852 + rolled })
    end
else
    c:sendOk("Just a pile of boxes, nothing special...")
end
