CC = g++
LIB = -lcrypto -lz -lstdc++ -llz4 -lpthread -lstdc++fs
SRC = main.cpp ./src/fastcdc.cpp ./src/ramcdc.cpp
SRC += ./src/MetadataManager.cpp 
SRC += ./src/ContainerCache.cpp 
SRC += ./utils/cJSON.c 
SRC += ./src/jcr.cpp 
OPTION = -g -O3 -mavx512bw -std=c++17
EXE_NAME = cDedup

amazing:
	$(CC) $(SRC) $(LIB) -o $(EXE_NAME) \
	$(OPTION) \
	-I./include -I./utils -I./utils/lz4-1.9.1/lib -L./utils/lz4-1.9.1/lib

clean:
	rm $(EXE_NAME)

