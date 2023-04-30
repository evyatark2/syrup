function talk(c)
    if c:sendYesNo("Would you like to leave?") == 1 then
        c:warp(105040300, 0);
    end
end
