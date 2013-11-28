local print = function(...) print(...) return ... end
local pulsar = require 'pulsar'

local loop = pulsar.defaultLoop()

local worker = loop:worker()
worker:register(function() print("lol worker") end)

local serv, err = loop:tcpServer("127.0.0.1", 2525, function(client)
	print("== Coroutine for client running", client:getpeername())
	client:startRead()
	client:send("READY!\n")
	for i = 1, 10 do worker:split() print("plop") end
	while client:connected() do
		local line = client:readUntil('\n')
		print("<<", line)
		client:send("lolzor\n")
--		print("===", line)
		if line == "quit" then break end
	end
	print("== Coroutine for client end", client)
end)
serv:start()

local timer = loop:timer(0.5, 2, function(timer) while true do
	print("timer")
	timer:next()
end end)
timer:start()

local idle = loop:idle(function(idle) while true do
	collectgarbage("collect")
	idle:next()
end end)
idle:start()

loop:run()
