function talk(c)
    local i = 0
    while true do
        if i == 0 then
            i = i + c:sendNext("This is the town called #bAmherst#k, located at the northeast part of the Maple Island. You know that Maple Island is for beginners, right? I'm glad there are only weak monsters around this place.")
        end

        if i == 1 then
            i = i + c:sendPrevNext("If you want to get stronger, then go to #bSouthperry#k where there's a harbor. Ride on the gigantic ship and head to the place called #bVictoria Island#k. It's incomparable in size compared to this tiny island.")
        end

        if i == 2 then
            i = i + c:sendPrev("At the Victoria Island, you can choose your job. Is it called #bPerion#k...? I heard there's a bare, desolate town where warriors live. A highland...what kind of a place would that be?")
        end

        if i == 3 then
            break
        end
    end
end
