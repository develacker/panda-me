CC = gcc
CFLAGS = -m32
TARGET= echo fauxware hof 

all: echo fauxware hof 

hof: hof.c
	$(CC) $(CFLAGS) -o hof hof.c

echo: echo.c
	$(CC) $(CFLAGS) -o echo echo.c

fauxware: fauxware.c
	$(CC) $(CFLAGS) -o fauxware fauxware.c

clean:
	rm -rf *.o
	rm -rf $(TARGET)