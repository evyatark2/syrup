local prizes = { 4010000, 4010001, 4010002, 4010003, 4010004, 4010005 };

function talk(c)
    loadfile("script/npc/common/flowers.lua")(c, 2052, 4031025, prizes, 3)
end
