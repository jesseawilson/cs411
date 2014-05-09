CC = gcc 
CXX = gcpc
CFLAGS = -Wall -std=c99 -O3 -g -I. -pthread -lrt
CXXFLAGS = -Wall -O3 -g

LDFLAGS = -lrt -lpthread

TARGET = sim_fifo

SOURCE = sim_fifo.c

default:	compile

compile: ${SOURCE] ${INCLUDES}
	${CC} ${CFLAGS} ${SOURCE} -o ${TARGET} ${LDFLAGS}


