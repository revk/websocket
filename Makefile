all: git websocketxml.o websocketjson.o websocketxml websocketjson

websocketxml.o: websocket.c websocket.h
	cc -g -Wall -Wextra -O -c -o websocketxml.o websocket.c -I. -IAXL -pthread -D_GNU_SOURCE -DUSEAXL

websocketjson.o: websocket.c websocket.h
	cc -g -Wall -Wextra -O -c -o websocketjson.o websocket.c -I. -IAJL -pthread -D_GNU_SOURCE -DUSEAJL

websocketxml: websocket.c websocket.h AXL/axl.o 	# Test
	cc -g -Wall -Wextra -O -o websocketxml websocket.c -I. -IAXL -D_GNU_SOURCE AXL/axl.o -lcurl -lcrypto -pthread -lssl -DMAIN -lpopt -DUSEAXL

websocketjson: websocket.c websocket.h AJL/ajl.o 	# Test
	cc -g -Wall -Wextra -O -o websocketjson websocket.c -I. -IAJL -D_GNU_SOURCE AJL/ajl.o -lcurl -lcrypto -pthread -lssl -DMAIN -lpopt -DUSEAJL

AXL/axl.o: AXL/axl.c
	make -C AXL

AJL/ajl.o: AJL/ajl.c
	make -C AJL

clean:
	rm -f *.o

git:
	git submodule update --init
