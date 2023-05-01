-- [[ 2124 - A Supply from the Sand Crew ]]

function end_(c)
    if c:hasItem(4031619) then
        c:sendOk("Oh, you brought #p2012019#'s box! Thank you.")
        c:gainItems({ { id = 4031619, amount = -1 } })
        c:endQuestNow()
    else
        c:sendOk("Please bring me the box with the supplies that lies with #b#p2012019##k...")
    end
end
