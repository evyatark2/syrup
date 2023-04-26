function talk(c)
    c:sendNext("Magicians are armed with flashy element-based spells and secondary magic that aids party as a whole. After the 2nd job adv., the elemental-based magic will provide ample amount of damage to enemies of opposite element.")
    if c:sendYesNo("Would you like to experience what it's like to be a Magician?") == 1 then
        c:warp(1020200, 0)
    else
        c:sendNext("If you wish to experience what it's like to be a Magician, come see me again.")
    end
end
