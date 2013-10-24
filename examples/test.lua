local pulsar = require 'pulsar'

local loop = pulsar.newLoop()

local longtask = loop:longTask()

local serv, err = loop:tcpServer("127.0.0.1", 2260, function(client)
	print("== Coroutine for client running", client:getpeername())
	client:startRead()

	local str = ""
	local stradd = string.rep("a", 1000)
	print("Start ...")
	for i = 1, 300 do
		str = str .. stradd
		longtask:split()
	end
	print("Started!")

	client:send("lolzor", true)
	client:send("lolzor", true)
	client:send("lolzor", true)
	client:send("lolzor\n")

	while client:connected() do
		local line = client:readUntil('\n', '\r')
		client:send("got '"..tostring(line).."'\n")
	end

	print("== Coroutine for client end", client)
end)
if not serv then return print(err) end
collectgarbage("collect")
serv:start()

local loltimer = loop:timer(0.3, 0.3, function(timer) while true do
	print("lol!")

	timer:next()
	collectgarbage("collect")
end end)
collectgarbage("collect")
loltimer:start()

local idle = loop:idle(function(idle) while true do
--	print("idling...")
	collectgarbage("collect")
	idle:next()
end end)
collectgarbage("collect")
idle:start()

loop:run()
