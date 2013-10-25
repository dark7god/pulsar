local pulsar = require 'pulsar'

local loop = pulsar.defaultLoop()

local longtask = loop:longTask()

local c
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
	c = client

	while client:connected() do
		local line = client:readUntil('\n', '\r')
		client:send("got '"..tostring(line).."'\n")
	end

	print("== Coroutine for client end", client)
end)
if not serv then return print(err) end
collectgarbage("collect")
serv:start()

local loltimer = loop:timer(4, 4, function(timer) while true do
	if c then
		c:close()
	end
	timer:next()
end end)
loltimer:start()

loop:run()
