local destinations = { "Ellinia", "Ludibrium", "Leafre", "Mu Lung", "Ariant", "Ereve" };
local boatType = { "the ship", "the train", "the bird", "Hak", "Genie", "the ship" };

function talk(c)
	local message = "Orbis Station has lots of platforms available to choose from. You need to choose the one that'll take you to the destination of your choice. Which platform will you take?\r\n"
    local i = 1
    for i = 1, #destinations do
		message = message .. "\r\n#L" .. i-1 .. "##bThe platform to " .. boatType[i] .. " that heads to " .. destinations[i] .. ".#l"
    end
	local sel = c:sendSimple(message)
    c:sendNext("Ok #h #, I will send you to the platform for #b#m" .. (200000110 + (sel * 10)) .. "##k.")
    c:warp(200000110 + (sel * 10), 0)
end