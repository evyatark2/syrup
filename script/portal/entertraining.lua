function enter(c)
    if c:isQuestStarted(1041) then
        --c:playPortalSound()
        c:warp(1010100, 4)
    elseif c:isQuestStarted(1042) then
        --c:playPortalSound()
        c:warp(1010200, 4)
    elseif c:isQuestStarted(1043) then
        --c:playPortalSound()
        c:warp(1010300, 4)
    elseif c:isQuestStarted(1044) then
        --c:playPortalSound()
        c:warp(1010400, 4)
    else
        c:message("Only the adventurers that have been trained by Mai may enter.");
        c:enableActions()
    end
end
