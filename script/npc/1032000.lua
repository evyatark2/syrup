--[[ 1012100 - Ellinia's Regular Cab ]]

maps = { 104000000, 102000000, 100000000, 103000000, 120000000 };
costs = { 1000, 1000, 1000, 1000, 800 };

function talk(c)
    local i = 0
    if i == 0 then
        i = i + c:sendNext("Hello, I drive the Regular Cab. If you want to go from town to town safely and fast, then ride our cab. We'll glady take you to your destination with an affordable price.")
    end

    if i == 1 then
        local str
        if c:job() == Job.BEGINNER then
            str = "We have a special 90% discount for beginners."
            str = str .. "Choose your destination, for fees will change from place to place.#b"
            for i = 1, #maps do
                str = str .. "\r\n#L" .. (i-1) .. "##m" .. maps[i] .. "# (" .. (costs[i] / 10) .. " mesos)#l"
            end
        else
            str = "Choose your destination, for fees will change from place to place.#b"
            for i = 1, #maps do
                str = str .. "\r\n#L" .. (i-1) .. "##m" .. maps[i] .. "# (" .. costs[i] .. " mesos)#l"
            end
        end
        i = i + c:sendSimple(str) + 1;
    end

    if i > 1 then
        local cost
        if c:job() == Job.BEGINNER then
            cost = costs[i-1] / 10
        else
            cost = costs[i-1]
        end

        local y = c:sendYesNo("You don't have anything else to do here, huh? Do you really want to go to #b#m" .. maps[i-1] .. "##k? It'll cost you #b" .. cost .. " mesos#k.")
        if y == 1 then
            if c:meso() < cost then
                c:sendNext("You don't have enough mesos. Sorry to say this, but without them, you won't be able to ride the cab.")
                return
            else
                c:gainMeso(-cost)
                c:warp(maps[i-1], 0)
                return
            end
        else
            c:sendNext("There's a lot to see in this town, too. Come back and find us when you need to go to a different town.")
            return
        end
    end
end
