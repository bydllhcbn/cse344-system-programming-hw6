CFLAGS=-pedantic -errors -Wall

all: Banka Client

Banka : Banka.o
	gcc -o Banka Banka.o -lrt

cat : Client.o
	gcc -o Client Client.o -lrt

clean : 
	rm *.o Banka Client
