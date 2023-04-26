function talk(c)
    c:sendNext("Bowmen are blessed with dexterity and power, taking charge of long-distance attacks, providing support for those at the front line of the battle. Very adept at using landscape as part of the arsenal.")
    if c:sendYesNo("Would you like to experience what it's like to be a Bowman?") == 1 then
        c:warp(1020300, 0)
    else
        c:sendNext("If you wish to experience what it's like to be a Bowman, come see me again.")
    end
end
