websocket.o: websocket.c
	cc -Wall -Wextra -O -c -o websocket.o websocket.c -I. -I../AXL -pthread -D_GNU_SOURCE

clean:
	rm -f *.o
