function talk(c)
    c:sendNext("Pirates are blessed with outstanding dexterity and power, utilizing their guns for long-range attacks while using their power on melee combat situations. Gunslingers use elemental-based bullets for added damage, while Infighters transform to a different being for maximum effect.")
    if c:sendYesNo("Would you like to experience what it's like to be a Pirate?") == 1 then
        c:warp(1020500, 0)
    else
        c:sendNext("If you wish to experience what it's like to be a Pirate, come see me again.")
    end
end
