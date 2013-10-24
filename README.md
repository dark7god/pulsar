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
The client will be properly closed when the function ends.

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

***client:stopRead()***

Stop receiving any more data.

***data, err = client:read(nb)***

Read nb bytes.
This will block the coroutine until exactly nb bytes are available.

***data, err = client:readUntil(until_string, ignore_string)***

Read bytes until until_string is found.
This will block the coroutine until data is found.
Returns the data without the until_string and if given without ignore_string at the end.

***ok = client:send(data, noblock)***

Send data and block until it finishes.
If noblock is set it will not block.
If multiple calls are made with noblock and then one with blocking it will wait until all is finished.

***is_connected = client:connected()***

Returns a boolean indicating if we are still connected to the other side.

***has_data = client:hasData()***

Return a boolean indicating there is data to be read in the buffer.

***ip = client:getpeername()***

Returns a string containing the IP of the other side.


***client:close()***

Closes the connection to the other side.

