function enter(c)
    if c:isQuestStarted(2073) then
        c:warp(900000000, 0)
    else
        c:message("Private property. This place can only be entered when running an errand from Camila.")
    end
end
