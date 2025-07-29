CC=gcc
SOURCES= server.c ControlDB.c
OBJECTS = ${SOURCES:.c=.o}
CFLAGS = -I/usr/include/mysql -Wall -g
LDFLAGS= -lmysqlclient -lwiringPi -pthread -g
RM = rm -f
OUT = server
$(OUT): $(OBJECTS)
	$(CC) -o $(OUT) $(OBJECTS) $(LIBS) $(CFLAGS) $(LDFLAGS)
clean:
	$(RM) $(OUT)*.o