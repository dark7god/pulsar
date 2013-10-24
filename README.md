Pulsar
=====
libev + coroutines + lua = profit

Description
===========
Pulsar is born from the wish to use the awesome libev to handle many connections from a single thread and the wish to see callback based code die a painful death.

All code in pulsar runs into coroutines and the user writes it "old school" aka in blocking calls style. Except it doesnt block.
Since an example is worth a thousands words:
```lua
local pulsar = require 'pulsar'
local loop = pulsar.newLoop()

local serv, err = loop:tcpServer("127.0.0.1", 3000, function(client)
	client:startRead()
	client:send("Welcome what is your name?\n")
	local line = client:readUntil('\n')
	client:send("Hi "..line.."\n")
end)
serv:start()
loop:run()
```

Compilation
=========
Very basic for now, just alter src/Makefile if needed and run make

Usage
=====
