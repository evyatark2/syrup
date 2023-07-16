function talk(c)
    local sel = c:sendSimple("Pick your destination.\n\r\n#L1##bKerning Square Shopping Center#l\n\n\r\n#L2#Enter Contruction Site#l\r\n#L3#New Leaf City#l", 3)
    local funcs = {
        function (c) 
            c:sendOk("Unimplemented.")
        end,

        function (c)
        if c:hasItem(4031036) or c:hasItem(4031037) or c:hasItem(4031038) then
            local text = "Here's the ticket reader. You will be brought in immediately. Which ticket you would like to use?#b"
            local count = 0
            local tickets = {}
            for i = 1, 3 do
                if c:hasItem(4031035 + i) then
                    count = count + 1
                    text = text .. "\r\n#b#L" .. count .. "##t" .. (4031035 + i) .. "#"
                    tickets[count] = i
                end
            end
            local sel = c:sendSimple(text, count)

            if c:gainItems({ id = 4031035 + tickets[sel], amount = -1 }) then
                c:warp(103000897 + (tickets[sel] * 3), 0)
            end
        else
            c:sendOk("It seems as though you don't have a ticket!")
        end
        end,

        function (c)
            local e = getEvent(Events.SUBWAY)
            local i
            if c:hasItem(4031711) then
                if e:getProperty(Events.SAILING) == 0 then
                    i = c:sendYesNo("It looks like there's plenty of room for this ride. Please have your ticket ready so I can let you in. The ride will be long, but you'll get to your destination just fine. What do you think? Do you want to get on this ride?");
                else
                    subway_not_here(c)
                    return
                end
            else
                c:sendOk("It seems you don't have a ticket! You can buy one from Bell.")
                return
            end

            if i == 1 then
                if e:getProperty(Events.SAILING) == 0 then
                    c:gainItems({ id = 4031711, amount = -1 })
                    c:warp(600010004, 0)
                else
                    subway_not_here(c)
                end
            end
        end
    }

    funcs[sel](c)
end

function subway_not_here(c)
    c:sendOk("We will begin boarding 1 minute before the takeoff. Please be patient and wait for a few minutes. Be aware that the subway will take off right on time, and we stop receiving tickets 1 minute before that, so please make sure to be here on time.")
end

