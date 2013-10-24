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
client:readUntil('\n')
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


Timer
=====
```lua
local timer = loop:timer(10, 1, function(timer) while true do
	print("Hello")
	timer:next()
end end)
timer:start()
```

Timers resume their coroutine after a first time and then repeatedly, this means the function passed to the timer must not exit (if it does the timer is deleted).

***timer = loop:timer(first, repeat, handler)***

Creates a timer that, once started, will fire in _first_ seconds and then every _repeat_ seconds. Reapeat time can be 0 to not repeat and decimal seconds can be given.

***timer:start()***

Starts the timer.

***timer:stop()***

Stops the timer.

***timer:next()***

Pause the coroutine, indicating we are finished with this iteration.


Idler
=====
```lua
local idle = loop:idle(function(idle) while true do
	print("I'm so idle...")
	idle:next()
end end)
idle:start()
```

Idlers resume their coroutine each time the system has nothing beter to do, this means the function passed to the idler must not exit (if it does the idler is deleted).

***idle = loop:idle(handler)***

Creates a idle that, once started, will fire when the system is not busy.

***idle:start()***

Starts the idle.

***idle:stop()***

Stops the idle.

***idle:next()***

Pause the coroutine, indicating we are finished with this iteration.


Long Task
=========
```lua
local longtask = loop:longTask()

...inside a coroutine...
	local str = ""
	local stradd = string.rep("a", 1000)
	print("Start ...")
	for i = 1, 300 do
		str = str .. stradd
		longtask:split()
	end
```

Long tasks allow you to split time consuming tasks into many small fragments, so that they do not block the application.
Split code must be running in a coroutine.

You probably only need one longtask object in your application since it can split as many simultaneous tasks as you want.

***longtask = loop:longTask()***

Creates a long task.

***longtask:split()***

Splits the current coroutine, allowing others to run. It will be waken up at the next possible occasion (by an internal idle handler).
