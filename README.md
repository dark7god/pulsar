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

local serv = loop:tcpServer("127.0.0.1", 3000, function(client)
	client:startRead()
	client:send("Welcome what is your name?\n")
	local line = client:readUntil('\n')
	client:send("Hi "..line.."\n")
end)
serv:start()
loop:run()
```

Compilation
===========
Very basic for now, just alter src/Makefile if needed and run make

API
===
Pulsar provides multiple kind of helpers:
* **TCP server**: create a listening socket and spawn each connecting client into its own coroutine
* **TCP client**: connect a socket to a remote host/port
* **Timer**: wakeup a coroutine at the given intervals
* **Idle**: wakeup a coroutine when nothing else to do
* **Long Task**: split up a long time consuming task to prevent blocking the application

Loop
====
All functions are bound to a loop, generally you will only need one. (See libev loops for more info)
```lua
local loop = pulsar.newLoop()
```

TCP Server
==========
```lua
local server = loop:tcpServer("127.0.0.1", 3000, function(client) end)
server:start()
```

***server = loop:tcpServer(host, port, handler_function)***

Create a tcp server on given host (use 0.0.0.0 to bind on all IPs) and port.
When a new connection arrives a coroutine is spawned running the handler function which is passed a ***TCP Client***.

***server:start()***

Let the server start accepting connections.

***server:stop()***

Stop the server from accepting any more connections.

TCP Client
==========
```lua
client:startRead()
client:read(10)
client:readUntil('\n'
client:send("Hellow world")
```

***client = loop:tcpClient(host, port)***

Create a tcp client to the given host and port.
It must be used in a coroutine for most methods to work.

***client:startRead()***

Let incomming data be read into the client's buffer.
You must call this before any read() calls.

***data, err = client:read(nb)***

Read nb bytes.
This will block the coroutine until exactly nb bytes are available.

***data, err = client:readUntil(until_string, ignore_string)***

Read bytes until until_string is found.
This will block the coroutine until data is found.
Returns the data without the until_string and if given without ignore_string at the end.
