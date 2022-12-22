# make rule primaria
all: peer ds

# make rule per il peer
peer: peer.o
			gcc -Wall peer.o -o peer

# make rule per il discovery server
ds: ds.o
			gcc -Wall ds.o -o ds

# pulizia dei file della compilazione
clean:
			rm *.o peer ds
