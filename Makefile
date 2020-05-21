all: git websocket.o

websocket.o: websocket.c
	cc -g -Wall -Wextra -O -c -o websocket.o websocket.c -I. -IAXL -IAJL -pthread -D_GNU_SOURCE

websocket: websocket.c AXL/axl.o AJL/ajl.o 	# Test
	cc -g -Wall -Wextra -O -o websocket websocket.c -I. -IAXL -IAJL -D_GNU_SOURCE AXL/axl.o AJL/ajl.o -lcurl -lcrypto -pthread -lssl -DMAIN -lpopt

AXL/axl.o: AXL/axl.c
	make -C AXL

AJL/ajl.o: AJL/ajl.c
	make -C AJL

clean:
	rm -f *.o

git:
	git submodule update --init
