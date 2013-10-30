local print = function(...) print(...) return ... end
local pulsar = require 'pulsar'

local loop = pulsar.defaultLoop()

local worker = loop:worker()

worker:register(function() print("lol worker") end)

local serv, err = loop:tcpServer("127.0.0.1", 3000, function(client)
	print("== Coroutine for client running", client:getpeername())

	worker:register(function()
		print("=== google register")
		local google = print(loop:tcpClient("127.0.0.1", 3001))
		if google then
			google:startRead()
			print("==google== start read")
			google:send("GET / HTTP/1.1\r\nHost: google.com\r\n\r\n")
			print("==google== sent")
			print("==google==", google:readUntil('\n'))
		end
	end)

	local str = ""
	local cat = string.rep("a", 1000)
	for i = 1, 300 do
		str=str..cat
		worker:split()
	end

	client:startRead()
	client:send("READY!\n")
	while client:connected() do
		print("===", client:readUntil('\n'))
		client:send("lolzor???\n")
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
