#include"MetadataManager.h"
#include"Cache.h"
#include<unordered_set>
#include<unordered_map>
#include<vector>
#include<queue>

#define SECTOR_SIZE 512

class ContainerCache : public Cache{
    public:
        ContainerCache(const char* containersPath, int cache_max_size){
            this->containers_path = containersPath;
            this->cache_max_size = cache_max_size; // 单位：容器数量
            int ret = posix_memalign((void**)&this->container_buf, SECTOR_SIZE, CONTAINER_SIZE);
            if (ret != 0) {
                fprintf(stderr, "posix_memalign failed: %s\n", strerror(ret));
                exit(EXIT_FAILURE); // 或者抛出异常
            }

            container_io_time = 0;
        }

        ~ContainerCache(){
            free(this->container_buf);
        }
        
        virtual std::string getChunkData(ENTRY_VALUE ev);
        uint64_t getStorageIOTime(){return this->container_io_time;}

    private:
        std::unordered_set<int> container_index_set;
        std::queue<int> container_index_queue;
        std::string containers_path;
        int cache_max_size;
        std::unordered_map<int, std::string> cache;
        char* container_buf;
        uint64_t container_io_time;

        void loadContainer(int container_number);
        void evictContainerFIFO();
};