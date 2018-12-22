websocket.o: websocket.c
	cc -g -Wall -Wextra -O -c -o websocket.o websocket.c -I. -IAXL -pthread -D_GNU_SOURCE

clean:
	rm -f *.o
