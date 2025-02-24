CC	= g++ -g3
CFLAGS  = -g3
TARGET1 = user
TARGET2 = worker 

OBJS1	= user.o
OBJS2	= worker.o

all:	$(TARGET1) $(TARGET2)

$(TARGET1):	$(OBJS1)
	$(CC) -o $(TARGET1) $(OBJS1)

$(TARGET2):	$(OBJS2)
	$(CC) -o $(TARGET2) $(OBJS2)

user.o:	user.cpp
	$(CC) $(CFLAGS) -c user.cpp 

worker.o:	worker.cpp
	$(CC) $(CFLAGS) -c worker.cpp

clean:
	/bin/rm -f *.o $(TARGET1) $(TARGET2)
