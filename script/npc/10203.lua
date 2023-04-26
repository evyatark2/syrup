function talk(c)
    c:sendNext("Thieves are a perfect blend of luck, dexterity, and power that are adept at the surprise attacks against helpless enemies. A high level of avoidability and speed allows Thieves to attack enemies from various angles.")
    if c:sendYesNo("Would you like to experience what it's like to be a Thief?") == 1 then
        c:warp(1020400, 0)
    else
        c:sendNext("If you wish to experience what it's like to be a Thief, come see me again.")
    end
end
