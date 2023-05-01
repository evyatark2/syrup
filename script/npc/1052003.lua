function talk(c)
    local selStr = "Yes, I do own this forge. If you're willing to pay, I can offer you some of my services.#b"
    local options = { "Refine a mineral ore", "Refine a jewel ore", "I have Iron Hog's Metal Hoof...", "Upgrade a claw" }
    for i = 1, #options do
        selStr = selStr .. "\r\n#L" .. i .. "# " .. options[i] .. "#l"
    end

    local i = c:sendSimple(selStr, #options);

    local items
    local materials
    local quantities
    local costs
    if i == 1 then
        local selStr = "So, what kind of mineral ore would you like to refine?#b"
        local minerals = { "Bronze", "Steel", "Mithril", "Adamantium", "Silver", "Orihalcon", "Gold" }
        for i = 1, #minerals do
            selStr = selStr .. "\r\n#L" .. i .. "# " .. minerals[i] .. "#l";
        end

        i = c:sendSimple(selStr, #minerals);

        items = { 4011000, 4011001, 4011002, 4011003, 4011004, 4011005, 4011006 }
        materials = { 4010000, 4010001, 4010002, 4010003, 4010004, 4010005, 4010006 }
        quantities = { 10, 10, 10, 10, 10, 10, 10 }
        costs = { 300, 300, 300, 500, 500, 500, 800 }
    elseif i == 2 then
        local selStr = "So, what kind of jewel ore would you like to refine?#b"
        local jewels = { "Garnet", "Amethyst", "Aquamarine", "Emerald", "Opal", "Sapphire", "Topaz", "Diamond", "Black Crystal" }
        for i = 1, #jewels do
            selStr = selStr .. "\r\n#L" .. i .. "# " .. jewels[i] .. "#l";
        end

        i = c:sendSimple(selStr, #jewels);

        items = { 4021000, 4021001, 4021002, 4021003, 4021004, 4021005, 4021006, 4021007, 4021008 }
        materials = { 4020000, 4020001, 4020002, 4020003, 4020004, 4020005, 4020006, 4020007, 4020008 }
        quantities = { 10, 10, 10, 10, 10, 10, 10, 10, 10 }
        costs = { 500, 500, 500, 500, 500, 500, 500, 1000, 3000 }
    else
        return
    end

    local qty = c:sendGetNumber("So, you want me to make some #t" .. items[i] .. "#s? In that case, how many do you want me to make?", 1, 1, 100)
    local prompt = "You want me to make ";
    if (qty == 1) then
        prompt = prompt .. "a";
    else
        prompt = prompt ..  qty;
    end

    prompt = prompt .. " #t" .. items[i] .. "#? In that case, I'm going to need specific items from you in order to make it. Make sure you have room in your inventory, though!#b\r\n"

    prompt = prompt .. "#i" .. materials[i] .. "# " .. quantities[i] * qty .. " #t" .. materials[i] .. "#\r\n";

    prompt = prompt .. "#i4031138# " .. costs[i] * qty .. " meso";

    if c:sendYesNo(prompt) == 1 then
        if c:meso() < costs[i] * qty then
            c:sendOk("Cach only, no credit")
        elseif not c:hasItem(materials[i], quantities[i] * qty) then
            c:sendOk("I cannot accept substitutes. If you don't have what I need, then I won't be able to help you.")
        elseif not c:gainItems({ { id = items[i], amount = 1 } }) then
            c:sendOk("Check your inventory for a free slot first.")
        else
            c:gainMeso(-costs[i] * qty)
            c:gainItems({ { id = materials[i], amount = -quantities[i] } })
            c:sendNext("Phew... I almost didn't think that would work for a second... Well, I hope you enjoy it, anyway.");
        end
    else
    end
end
