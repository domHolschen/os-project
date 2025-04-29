CC	= g++ -g3
CFLAGS  = -g3
TARGET1 = oss
TARGET2 = worker

OBJS1	= oss.o clockUtils.o resourceUtils.o
OBJS2	= worker.o clockUtils.o resourceUtils.o
OBJS3	= clockUtils.o resourceUtils.o

all:	$(TARGET1) $(TARGET2)

$(TARGET1):	$(OBJS1)
	$(CC) -o $(TARGET1) $(OBJS1)

$(TARGET2):	$(OBJS2)
	$(CC) -o $(TARGET2) $(OBJS2)

oss.o:	oss.cpp clockUtils.h resourceUtils.h
	$(CC) $(CFLAGS) -c oss.cpp 

worker.o:	worker.cpp clockUtils.h resourceUtils.h
	$(CC) $(CFLAGS) -c worker.cpp

clockUtils.o:	clockUtils.cpp clockUtils.h
	$(CC) $(CFLAGS) -c clockUtils.cpp

resourceUtils.o: resourceUtils.cpp resourceUtils.h
	$(CC) $(CFLAGS) -c resourceUtils.cpp

clean:
	/bin/rm -f *.o $(TARGET1) $(TARGET2)
