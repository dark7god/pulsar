package = "pulsar"
version = "scm-1"

source = {
	url = "git@github.com:dark7god/pulsar.git"
}

description = {
	summary = "lib-ev + coroutine",
	detailed = [[
		Pulsar: Lua's coroutine integration with libev (http://dist.schmorp.de/libev) and tcp sockets
	]],
	homepage = "https://github.com/dark7god/pulsar",
	license = "MIT/X11"
}

dependencies = {
	"lua >= 5.1"
}

external_dependencies = {
	LIBEV = {
		header = "ev.h",
		library = "libev.so"
	}
}

build = {
	type = "builtin",
	modules = {
		ev = {
			sources = {
				"src/pulsar.c"
			},
			libraries = {
				"pulsar"
			}
		}
	}
}
