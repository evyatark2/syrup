function talk(c)
    c:sendNext("Warriors possess an enormous power with stamina to back it up, and they shine the brightest in melee combat situation. Regular attacks are powerful to begin with, and armed with complex skills, the job is perfect for explosive attacks.")
    if c:sendYesNo("Would you like to experience what it's like to be a Warrior?") == 1 then
        c:warp(1020100, 0)
    else
        c:sendNext("If you wish to experience what it's like to be a Warrior, come see me again.")
    end
end
