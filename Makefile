all: git websocket.o

websocket.o: websocket.c
	cc -g -Wall -Wextra -O -c -o websocket.o websocket.c -I. -IAXL -pthread -D_GNU_SOURCE

websocket: websocket.c AXL/axl.o 	# Test
	cc -g -Wall -Wextra -O -o websocket websocket.c -I. -IAXL -D_GNU_SOURCE AXL/axl.o -lcurl -lcrypto -pthread -lssl -DMAIN -lpopt

AXL/axl.o: AXL/axl.c
	make -C AXL

clean:
	rm -f *.o

git:
	git submodule update --init
