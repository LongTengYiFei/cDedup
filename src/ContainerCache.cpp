#include"ContainerCache.h"
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

std::string ContainerCache::getChunkData(ENTRY_VALUE ev){
    auto numberIter = this->container_index_set.find(ev.container_number);
    if(numberIter != this->container_index_set.end()){
        //cache hit
        return std::string(cache[ev.container_number], ev.offset, ev.chunk_length);
    }else{
        //cache miss
        if(container_index_queue.size() >= this->cache_max_size){
            evictContainerFIFO();
        }
        this->loadContainer(ev.container_number);
        return std::string(cache[ev.container_number], ev.offset, ev.chunk_length);
    }
}

void ContainerCache::loadContainer(int container_index){
    this->container_index_queue.push(container_index);
    this->container_index_set.insert(container_index);
    
    std::string container_name(this->containers_path);
    container_name.append("/container");
    container_name.append(std::to_string(container_index));
    int fd = open(container_name.data(), O_RDONLY | O_DIRECT);

    memset(this->container_buf, 0, CONTAINER_SIZE);

    struct timeval cIO_time_start, cIO_time_end;
    gettimeofday(&cIO_time_start, NULL);
    int n = read(fd, this->container_buf, CONTAINER_SIZE); // 可能塞不满
    gettimeofday(&cIO_time_end, NULL);
    this->container_io_time += (cIO_time_end.tv_sec - cIO_time_start.tv_sec) * 1000000 + (cIO_time_end.tv_usec - cIO_time_start.tv_usec);

    std::string content(this->container_buf , n);

    this->cache[container_index] = content;
    close(fd);
}

void ContainerCache::evictContainerFIFO(){
    int container_index = this->container_index_queue.front();
    this->container_index_set.erase(container_index);
    this->container_index_queue.pop();

    cache.erase(container_index);
}