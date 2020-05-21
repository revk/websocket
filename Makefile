all: git websocket.o websocket

websocket.o: websocket.c
	cc -g -Wall -Wextra -O -c -o websocket.o websocket.c -I. -IAJL -pthread -D_GNU_SOURCE

websocket: websocket.c AJL/ajl.o 	# Test
	cc -g -Wall -Wextra -O -o websocket websocket.c -I. -IAJL -D_GNU_SOURCE AJL/ajl.o -lcurl -lcrypto -pthread -lssl -DMAIN -lpopt

AJL/ajl.o: AJL/ajl.c
	make -C AJL

clean:
	rm -f *.o

git:
	git submodule update --init
