CC = clang

CFLAGS = -Wall -Werror -Wextra -pedantic -pthread -O2 -flto

LDFLAGS = -lpthread

Target = httpserver

all: httpserver

$$(TARGET): $(TARGET).c
	$(clang) $(CFLAGS) -o $(TARGET) $(TARGET).c $(LDFLAGS)

clean: 
	$(RM) httpserver *.o *~
