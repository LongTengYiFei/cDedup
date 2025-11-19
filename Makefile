CC = gcc
LIB = -lcrypto -lz -lstdc++ -llz4 -lpthread -lstdc++fs
SRC = main.cpp ./src/fastcdc.cpp 
SRC += ./src/MetadataManager.cpp 
SRC += ./src/ContainerCache.cpp 
SRC += ./utils/cJSON.c 
SRC += ./src/jcr.cpp 

EXE_NAME = cDedup

amazing:
	$(CC) -std=c++17 $(SRC) $(LIB) -o $(EXE_NAME) \
	-g -O0 -I./include -I./utils -I./utils/lz4-1.9.1/lib -L./utils/lz4-1.9.1/lib

clean:
	rm $(EXE_NAME)

