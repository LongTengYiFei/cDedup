#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <experimental/filesystem>
#include <string>
#include <iostream>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <vector>
#include <algorithm>
#include <random>
#include <numeric> // for std::iota
#include <iomanip> // 确保包含此头文件

#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/sendfile.h>
#include <openssl/sha.h>

#include "fastcdc.h"
#include "MetadataManager.h"
#include "ContainerCache.h"
#include "general.h"
#include "config.h"
#include "utils/cJSON.h"
#include "global_stat.h"
#include "jcr.h"
#include "ramcdc.h"

#define MB (1024*1024)
#define GB (1024*1024*1024)

struct backup_job{
    uint64_t hash_collision_sum;
    uint64_t sum_chunks;
    uint64_t sum_size;
    uint64_t dedup_chunks;
    uint64_t dedup_size;
    uint64_t file_num;
};

namespace fs = std::experimental::filesystem;

const char* global_stat_path = "/home/cyf/cDedup/global_stat.json";
string dedup_log_path;
std::ofstream log_file;
extern MetadataManager *GlobalMetadataManagerPtr;
int (*chunking) (unsigned char*p, int n);
struct backup_job bj;

// 写任务
uint32_t container_index = 0; // 控制当前写入的container index
uint32_t rev_container_cnt = 0;
unsigned char rev_container_buf[CONTAINER_SIZE]={0};
unsigned char tmp_buf[CONTAINER_SIZE]={0};
unsigned char* file_cache;
unsigned char* container_buffer;
unsigned char* sample_cache;

// interval observation
struct interval_task{
    int interval;
    double actual_dr;
    float avg_read_amplification;
    uint64_t sum_size;
    uint64_t dedup_size;
    int total_container_reference;
    int file_num;
};
vector<struct interval_task> interval_tasks;
vector<int> intervals;
vector<int> container_indice;


/*
    window observation
    观察不同window，达到actual dr峰值的window内偏移；
    这里就默认把100个备份划分为两个窗口，每个窗口50个备份
    并且这100个备份来自同一个source；
*/
const int window_size = 50;
const vector<int> windows_start = {0, 50};
struct window_result{
    // statistic
    int peak_actual_dr_offset;
    float peak_actual_dr;
    uint64_t dup_size;
    uint64_t sum_size;
    vector<string> files;
    vector<float> actual_drs;
    vector<float> read_amplifications;

    // control write job
    float current_actual_dr;
    int current_container_index;
};
map<int, window_result> window_results;

// /*
//     Estimation Observation
// */
// struct estimation_result{
//     uint64_t sum_size;
//     uint64_t dup_size;
//     float dr;
// };
// vector<struct estimation_result> estimation_results;

uint32_t getFilesNum(const char* dirPath) {
    if (!dirPath) {
        std::fprintf(stderr, "getFilesNum: dirPath is null\n");
        exit(-1);
    }

    DIR* dir = opendir(dirPath);
    if (!dir) {
        std::fprintf(stderr, "getFilesNum opendir error, errno %d: %s, dir: %s\n",
                     errno, strerror(errno), dirPath);
        exit(-1);  // 或 return 0; 视错误处理策略而定
        // ❌ 不要在这里调用 closedir(dir)!
    }

    int ans = 0;
    struct dirent* ptr;
    while ((ptr = readdir(dir)) != nullptr) {
        // 跳过 "." 和 ".."
        if (strcmp(ptr->d_name, ".") != 0 && strcmp(ptr->d_name, "..") != 0) {
            ans++;
        }
    }

    closedir(dir);  // 此时 dir 一定非空
    return static_cast<uint32_t>(ans);
}

void saveFileRecipe(std::vector<std::string> file_recipe, const char* fileRecipesPath){
    int n_version = getFilesNum(fileRecipesPath);
    std::string recipe_name(fileRecipesPath);
    recipe_name.append("/recipe");
    recipe_name.append(std::to_string(n_version));
    int fd = open(recipe_name.data(), O_RDWR | O_CREAT, 0777);
    if(fd < 0){ 
        std::printf("saveFileRecipe open error, id %d, %s\n", errno, strerror(errno)); 
        exit(-1);
    }
    for(auto x : file_recipe){
        if(write(fd, x.data(), SHA_DIGEST_LENGTH) < 0)
            std::printf("save recipe write error\n");
    }
    close(fd);
}

std::string getRecipeNameFromVersion(uint8_t restore_version, const char* file_recipe_path){
    std::string recipe_name(file_recipe_path);
    recipe_name.append("/recipe");
    recipe_name.append(std::to_string(restore_version));
    return recipe_name;
}

bool fileRecipeExist(uint8_t restore_version, const char* file_recipe_path){
    if((access(getRecipeNameFromVersion(restore_version, file_recipe_path).data(), F_OK)) != -1)    
        return true;    
 
    return false;
}

std::vector<std::string> getFileRecipe(uint8_t restore_version, const char* file_recipe_path){
    if(!fileRecipeExist(restore_version, file_recipe_path)){
        std::printf("Restore version %d not exist!!!\n", restore_version); 
        exit(-1);
    }

    std::string recipe_name = getRecipeNameFromVersion(restore_version, file_recipe_path);
    int fd = open(recipe_name.data(), O_RDONLY, 0777);
    if(fd < 0){
        std::printf("getFileRecipe open error, %s, %s\n", strerror(errno), recipe_name.data());
        exit(-1);
    }
    char fp_buf[SHA_DIGEST_LENGTH]={0};
    std::vector<std::string> ans;

    while(1){
        int n = read(fd, fp_buf, SHA_DIGEST_LENGTH);
        if(n == 0){
            break;
        }else if(n < 0){
            std::printf("getFileRecipe error, %s, %s\n", strerror(errno), recipe_name.data());
            exit(-1);
        }else{
            ans.push_back(std::string(fp_buf, SHA_DIGEST_LENGTH));
        }
    }
    close(fd);
    return ans;
}

void saveContainer(int container_index, unsigned char* container_buf, unsigned int len, const char* containersPath){
    std::string container_name(containersPath);
    container_name.append("/container");
    container_name.append(std::to_string(container_index));
    int fd = open(container_name.data(), O_RDWR | O_CREAT | O_DIRECT, 0777);
    if(write(fd, container_buf, len) != len){
        std::printf("saveContainer write error, id %d, %s\n", errno, strerror(errno));
        exit(-1);
    }
    close(fd);
}

void saveChunkToContainer(unsigned int& container_buf_pointer, unsigned char* container_buf, 
                          uint32_t& container_index, uint32_t& container_inner_offset, uint16_t& container_inner_index,
                          int chunk_length, int file_offset, unsigned char* file_cache, void* SHA_buf,
                          const char* containers_path){
    
    // flush
    if(container_buf_pointer + chunk_length >= CONTAINER_SIZE){
        saveContainer(container_index, container_buf, CONTAINER_SIZE, containers_path);

        std::memset(container_buf, 0, CONTAINER_SIZE);
        container_index++;
        container_inner_offset = 0;
        container_buf_pointer = 0;
        container_inner_index = 0;

        // rev_container_cnt = 0;
    }

    // container buffer
    memcpy(container_buf + container_buf_pointer, file_cache + file_offset, chunk_length);
    // memcpy(rev_container_buf + sizeof(SHA1FP)*rev_container_cnt, SHA_buf, sizeof(SHA1FP));

}

void flushAssemblingBuffer(int fd, unsigned char* buf, int len){
    if(write(fd, buf, len) != len){
        std::printf("Restore, write file error!!!\n");
        exit(-1);
    }
    //close(fd);
}

void initChunkingAlgorithm(){
    enum CHUNKING_METHOD cm_type = Config::getInstance().getChunkingMethod();
    if(cm_type == FASTCDC){
        int NC_level = Config::getInstance().getNormalLevel();
        int avg_size = Config::getInstance().getAvgChunkSize();
        fastCDC_init(avg_size, NC_level);

        if(NC_level == 0)
            chunking = FastCDC_without_NC;
        else if(1<=NC_level && NC_level<=3)
            chunking = FastCDC_with_NC;
        else{
            std::printf("Invalid NC level: %d\n", NC_level);
        }
    }else if(cm_type == FSC){
        int avg_size = Config::getInstance().getAvgChunkSize();
        if(avg_size == 4*1024){
            chunking = FSC_4;
        }else if(avg_size == 8*1024){
            chunking = FSC_8;
        }else if(avg_size == 16*1024){
            chunking = FSC_16;
        }else if(avg_size == 512){
            chunking = FSC_512;
        }else{
            std::printf("Invalid fixed size, the support size is 4 or 8 or 16\n");
            exit(-1);
        }
    }else if(cm_type == VECTORCDC){
        chunking = ramcdc_avx_512;
        ramcdc_init(Config::getInstance().getAvgChunkSize());
    }
}

void writeFileNaive(string path){
    // start time
    auto start_time = std::chrono::high_resolution_clock::now();

    log_file << "Start write file: " << path << ", method: naive" << std::endl;

    int idf = open(path.c_str(), O_RDONLY | O_DIRECT);
    if(idf < 0){
        std::printf("open file error, id %d, %s\n", errno, strerror(errno));
        exit(-1);
    }

    std::vector<std::string> file_recipe; // 保存这个文件所有块的指纹

    // metadata entry(except FP)
    uint32_t container_inner_offset = 0;
    uint32_t chunk_length = 0;
    uint16_t container_inner_index = 0;
    unsigned int container_buf_pointer = 0;
    uint32_t file_offset = 0;
    uint32_t n_read = 0;
    struct ENTRY_VALUE tmp_entry_value;
    struct SHA1FP tmp_sha1_fp;

    // 当前文件重删统计
    uint64_t dedup_chunks = 0;
    uint64_t dedup_size = 0;
    uint64_t sum_chunks = 0;
    uint64_t sum_size = 0;
    uint64_t hash_collision_sum = 0;
    set<int> reference_containers;

    bool container_fake_io = Config::getInstance().getContainerFakeIO();
    
    // 普通分块重删，来一个块查寻一次，然后把non-duplicate chunk保存到container去
    for(;;){
        file_offset = 0;

        n_read = read(idf, (void*)file_cache, BLOCK_SIZE);

        if(n_read <= 0){
            break;
        }

        while(file_offset < n_read){  
            // Chunk
            chunk_length = chunking(file_cache + file_offset, n_read - file_offset);
            
            // Hash
            SHA1(file_cache + file_offset, chunk_length, (uint8_t*)&tmp_sha1_fp);

            // Dedup
            LookupResult lookup_result = GlobalMetadataManagerPtr->dedupLookup(tmp_sha1_fp);

            if(!lookup_result.dup){
                if(container_fake_io){
                    if(container_inner_offset + chunk_length > CONTAINER_SIZE){
                        container_index ++;
                        container_inner_offset = 0;
                        container_inner_offset += chunk_length;
                    }else{
                        container_inner_offset += chunk_length;
                    }

                }else{
                    // save chunk itself
                    saveChunkToContainer(container_buf_pointer, container_buffer, 
                                        container_index, container_inner_offset, container_inner_index,
                                        chunk_length, file_offset, file_cache, (void*)&tmp_sha1_fp,
                                        Config::getInstance().getContainersPath().c_str());
                    
                    // conatainer write control
                    container_inner_offset += chunk_length;
                    container_buf_pointer += chunk_length;
                    container_inner_index ++;
                }
                
                // save chunk metadata
                tmp_entry_value.container_number = container_index;
                tmp_entry_value.offset = container_inner_offset;
                tmp_entry_value.chunk_length = chunk_length;
                tmp_entry_value.container_inner_index = container_inner_index;
                tmp_entry_value.ref_cnt = 1;

                GlobalMetadataManagerPtr->addNewEntry(tmp_sha1_fp, tmp_entry_value);
                reference_containers.insert(container_index);

            }else{
                dedup_chunks ++;
                dedup_size += chunk_length;
                reference_containers.insert(lookup_result.container_index);

            }

            // Insert fingerprint into file recipe
            file_recipe.push_back(std::string((char*)&tmp_sha1_fp, sizeof(struct SHA1FP)));

            // Statistic
            sum_chunks ++;
            sum_size += chunk_length;

            file_offset += chunk_length;
        }
    }

    //  flush last container
    if(container_buf_pointer > 0)
        saveContainer(container_index, container_buffer, CONTAINER_SIZE, Config::getInstance().getContainersPath().c_str());
    
    // flush file recipe
    saveFileRecipe(file_recipe, Config::getInstance().getFileRecipesPath().c_str());

    // #th statistic
    log_file << "Sum chunks " << sum_chunks << endl;
    log_file << "Sum size " << sum_size << endl;
    log_file << "Dedup chunks " << dedup_chunks << endl;
    log_file << "Dedup size " << dedup_size << endl;

    float dedup_ratio_1 = double(dedup_size) / double(sum_size);
    float dedup_ratio_2 = double(sum_size) / (double(sum_size) - double(dedup_size));
    log_file << "Dedup ratio 1: " << dedup_ratio_1 << endl;
    log_file << "Dedup ratio 2: " << dedup_ratio_2 << endl;

    float read_amplification = (double(reference_containers.size()) * CONTAINER_SIZE) / double(sum_size);
    log_file << "Read amplification: " << read_amplification << endl;

    // update backup job
    bj.dedup_chunks += dedup_chunks;
    bj.dedup_size += dedup_size;
    bj.sum_chunks += sum_chunks;
    bj.sum_size += sum_size;
    bj.hash_collision_sum += hash_collision_sum;
    bj.file_num++;

    // total statistic
    float actual_dratio_1 = double(bj.dedup_size) / double(bj.sum_size);
    float actual_dratio_2 = double(bj.sum_size) / (double(bj.sum_size) - double(bj.dedup_size));
    log_file << "Actual dedup ratio after backup 1: " << actual_dratio_1 << endl;
    log_file << "Actual dedup ratio after backup 2: " << actual_dratio_2 << endl;
    
    // end time and throughput
    auto end_time = std::chrono::system_clock::now();
    auto total_time = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    float throughput = (float)(sum_size) / MB / ((float)(total_time)/1000000);
    log_file<<"Throughput: "<<throughput<<" MB/s"<<endl;

    // free 
    close(idf);

    log_file << "Finish write file" << endl;
}

void writeFileDedupFirst(string path, int current_version){
    log_file << "Start write file: " << path << ", method: Dedup First" << std::endl;

    int idf = open(path.c_str(), O_RDONLY, 0777);
    if(idf < 0){
        std::printf("open file error, id %d, %s\n", errno, strerror(errno));
        exit(-1);
    }

    unsigned char* file_cache = (unsigned char*)malloc(BLOCK_SIZE);

    std::vector<std::string> file_recipe; // 保存这个文件所有块的指纹

    // metadata entry(except FP)
    uint32_t container_inner_offset = 0;
    uint32_t chunk_length = 0;
    uint16_t container_inner_index = 0;

    unsigned char container_buf[CONTAINER_SIZE]={0};
    unsigned int container_buf_pointer = 0;
    uint32_t file_offset = 0;
    uint32_t n_read = 0;

    struct ENTRY_VALUE tmp_entry_value;
    struct SHA1FP tmp_sha1_fp;

    // 单个备份文件的统计信息
    uint64_t dedup_chunks = 0;
    uint64_t dedup_size = 0;
    uint64_t sum_chunks = 0;
    uint64_t sum_size = 0;
    uint64_t hash_collision_sum = 0;
    set<int> reference_containers;
    
    for(;;){
        file_offset = 0;

        n_read = read(idf, file_cache, BLOCK_SIZE);

        if(n_read <= 0){
            break;
        }

        while(file_offset < n_read){  
            // Chunk
            chunk_length = chunking(file_cache + file_offset, n_read - file_offset);
            
            // Hash
            std::memset(&tmp_sha1_fp, 0, sizeof(struct SHA1FP));
            SHA1(file_cache + file_offset, chunk_length, (uint8_t*)&tmp_sha1_fp);

            // Dedup
            LookupResult lookup_result = GlobalMetadataManagerPtr->dedupLookup(tmp_sha1_fp, 0, current_version);

            if(!lookup_result.dup){
                // save chunk itself
                saveChunkToContainer(container_buf_pointer, container_buf, 
                                    container_index, container_inner_offset, container_inner_index,
                                    chunk_length, file_offset, file_cache, (void*)&tmp_sha1_fp,
                                    Config::getInstance().getContainersPath().c_str());
                
                // save chunk metadata
                tmp_entry_value.container_number = container_index;
                tmp_entry_value.offset = container_inner_offset;
                tmp_entry_value.chunk_length = chunk_length;
                tmp_entry_value.container_inner_index = container_inner_index;
                tmp_entry_value.version = current_version;
                tmp_entry_value.ref_cnt = 1;

                GlobalMetadataManagerPtr->addNewEntry(tmp_sha1_fp, tmp_entry_value, current_version);

                // rev
                container_inner_offset += chunk_length;
                container_buf_pointer += chunk_length;
                container_inner_index ++;
                rev_container_cnt ++;
                reference_containers.insert(container_index);

            }else{
                dedup_chunks ++;
                dedup_size += chunk_length;
                reference_containers.insert(lookup_result.container_index);
            }

            // Insert fingerprint into file recipe
            file_recipe.push_back(std::string((char*)&tmp_sha1_fp, sizeof(struct SHA1FP)));

            // Statistic
            sum_chunks ++;
            sum_size += chunk_length;

            file_offset += chunk_length;
        }
    }

    //  flush最后一个container
    if(container_buf_pointer > 0)
        saveContainer(container_index, container_buf, 
                        container_buf_pointer, Config::getInstance().getContainersPath().c_str());
    
    // flush file_recipe
    saveFileRecipe(file_recipe, Config::getInstance().getFileRecipesPath().c_str());
    
    // #th statistic
    log_file << "Sum chunks " << sum_chunks << endl;
    log_file << "Sum size " << sum_size << endl;
    log_file << "Dedup chunks " << dedup_chunks << endl;
    log_file << "Dedup size " << dedup_size << endl;

    float dedup_ratio_1 = double(dedup_size) / double(sum_size);
    float dedup_ratio_2 = double(sum_size) / (double(sum_size) - double(dedup_size));
    log_file << "Dedup ratio 1: " << dedup_ratio_1 << endl;
    log_file << "Dedup ratio 2: " << dedup_ratio_2 << endl;

    float read_amplification = (double(reference_containers.size()) * CONTAINER_SIZE) / double(sum_size);
    log_file << "Read amplification: " << read_amplification << endl;

    // update backup job
    bj.dedup_chunks += dedup_chunks;
    bj.dedup_size += dedup_size;
    bj.sum_chunks += sum_chunks;
    bj.sum_size += sum_size;
    bj.hash_collision_sum += hash_collision_sum;
    bj.file_num++;

    // total statistic
    float actual_dratio_1 = double(bj.dedup_size) / double(bj.sum_size);
    float actual_dratio_2 = double(bj.sum_size) / (double(bj.sum_size) - double(bj.dedup_size));
    log_file << "Actual dedup ratio after backup 1: " << actual_dratio_1 << endl;
    log_file << "Actual dedup ratio after backup 2: " << actual_dratio_2 << endl;
    
    // free 
    close(idf);
    free(file_cache);

    log_file << "Finish write file" << endl;
}

void writeFileDedupFixedInterval(string path, int current_version, int interval){
    log_file << "Start write file: " << path << ", method: Dedup Interval" << std::endl;

    int idf = open(path.c_str(), O_RDONLY, 0777);
    if(idf < 0){
        std::printf("open file error, id %d, %s\n", errno, strerror(errno));
        exit(-1);
    }

    unsigned char* file_cache = (unsigned char*)malloc(BLOCK_SIZE);
    std::vector<std::string> file_recipe; // 保存这个文件所有块的指纹

    // metadata entry (except FP)
    uint32_t container_inner_offset = 0;
    uint32_t chunk_length = 0;
    uint16_t container_inner_index = 0;
    unsigned char container_buf[CONTAINER_SIZE]={0};
    unsigned int container_buf_pointer = 0;
    uint32_t file_offset = 0;
    uint32_t n_read = 0;

    struct ENTRY_VALUE entry_value;
    struct SHA1FP tmp_sha1_fp;

    // 单个备份文件的统计信息
    uint64_t dedup_chunks = 0;
    uint64_t dedup_size = 0;
    uint64_t sum_chunks = 0;
    uint64_t sum_size = 0;
    uint64_t hash_collision_sum = 0;
    set<int> reference_containers;

    int base_version = current_version / interval * interval;

    for(;;){
        file_offset = 0;

        n_read = read(idf, file_cache, BLOCK_SIZE);

        if(n_read <= 0){
            break;
        }

        while(file_offset < n_read){  
            // Chunk
            chunk_length = chunking(file_cache + file_offset, n_read - file_offset);
            
            // Hash
            std::memset(&tmp_sha1_fp, 0, sizeof(struct SHA1FP));
            SHA1(file_cache + file_offset, chunk_length, (uint8_t*)&tmp_sha1_fp);

            // Dedup
            LookupResult lookup_result = GlobalMetadataManagerPtr->dedupLookup(tmp_sha1_fp, base_version, current_version); 

            // 
            if(!lookup_result.dup){
                // save chunk itself
                saveChunkToContainer(container_buf_pointer, container_buf, 
                                    container_index, container_inner_offset, container_inner_index,
                                    chunk_length, file_offset, file_cache, (void*)&tmp_sha1_fp,
                                    Config::getInstance().getContainersPath().c_str());
                
                // save chunk metadata
                entry_value.container_number = container_index;
                entry_value.offset = container_inner_offset;
                entry_value.chunk_length = chunk_length;
                entry_value.container_inner_index = container_inner_index;
                entry_value.version = current_version;
                entry_value.ref_cnt = 1;
  
                GlobalMetadataManagerPtr->addNewEntry(tmp_sha1_fp, entry_value, current_version);

                // rev
                container_inner_offset += chunk_length;
                container_buf_pointer += chunk_length;
                container_inner_index ++;
                rev_container_cnt ++;

                reference_containers.insert(container_index);

            }else{
                dedup_chunks ++;
                dedup_size += chunk_length;
                reference_containers.insert(lookup_result.container_index);

            }

            // Insert fingerprint into file recipe
            file_recipe.push_back(std::string((char*)&tmp_sha1_fp, sizeof(struct SHA1FP)));

            // Statistic
            sum_chunks ++;
            sum_size += chunk_length;

            file_offset += chunk_length;
        }
    }

    //  flush最后一个container
    if(container_buf_pointer > 0)
        saveContainer(container_index, container_buf, 
                        container_buf_pointer, Config::getInstance().getContainersPath().c_str());
    
    // flush file_recipe
    saveFileRecipe(file_recipe, Config::getInstance().getFileRecipesPath().c_str());

    // #th statistic
    log_file << "Sum chunks " << sum_chunks << endl;
    log_file << "Sum size " << sum_size << endl;
    log_file << "Dedup chunks " << dedup_chunks << endl;
    log_file << "Dedup size " << dedup_size << endl;

    float dedup_ratio_1 = double(dedup_size) / double(sum_size);
    float dedup_ratio_2 = double(sum_size) / (double(sum_size) - double(dedup_size));
    log_file << "Dedup ratio 1: " << dedup_ratio_1 << endl;
    log_file << "Dedup ratio 2: " << dedup_ratio_2 << endl;

    float read_amplification = (double(reference_containers.size()) * CONTAINER_SIZE) / double(sum_size);
    log_file << "Read amplification: " << read_amplification << endl;

    // update backup job
    bj.dedup_chunks += dedup_chunks;
    bj.dedup_size += dedup_size;
    bj.sum_chunks += sum_chunks;
    bj.sum_size += sum_size;
    bj.hash_collision_sum += hash_collision_sum;
    bj.file_num++;

    // total statistic
    float actual_dratio_1 = double(bj.dedup_size) / double(bj.sum_size);
    float actual_dratio_2 = double(bj.sum_size) / (double(bj.sum_size) - double(bj.dedup_size));
    log_file << "Actual dedup ratio after backup 1: " << actual_dratio_1 << endl;
    log_file << "Actual dedup ratio after backup 2: " << actual_dratio_2 << endl;

    // free 
    close(idf);
    free(file_cache);

    log_file << "Finish write file" << endl;
}

void writeFileDedupScode(string path, int current_version){
    struct timeval start_time, end_time;
    gettimeofday(&start_time, NULL);

    log_file << "Start write file: " << path << ", method: Dedup Scode" << std::endl;

    int idf = open(path.c_str(), O_RDONLY | O_DIRECT, 0777);
    if(idf < 0){
        std::printf("open file error, id %d, %s\n", errno, strerror(errno));
        exit(-1);
    }

    std::vector<std::string> file_recipe;

    // container write control
    uint32_t cache_offset = 0;
    uint32_t n_read = 0;
    uint32_t container_inner_offset = 0;
    uint16_t container_inner_index = 0;
    uint32_t container_buf_pointer = 0;
    uint32_t chunk_length = 0;
    struct ENTRY_VALUE tmp_entry_value;
    struct SHA1FP tmp_sha1_fp;

    // 当前文件重删统计
    uint64_t dedup_chunks = 0;
    uint64_t dedup_size = 0;
    uint64_t sum_chunks = 0;
    uint64_t sum_size = 0;
    set<int> reference_containers;

    bool container_fake_io = Config::getInstance().getContainerFakeIO();

    GlobalMetadataManagerPtr->ScodeInitSingleFile();

    /*
        ADRE
        1. sample
        2. dedup against all current base table and find source
        3. ADRE compute
    */
    /*
        sample
    */
    uint64_t file_size = 0;
    struct stat file_stat;
    stat(path.c_str(), &file_stat);
    file_size = file_stat.st_size;

    uint64_t file_block_num = (file_size + BLOCK_SIZE - 1) / BLOCK_SIZE; // 向上取整
    float sample_ratio = GlobalMetadataManagerPtr->getSampleRatio();
    uint64_t sample_block_num = static_cast<uint64_t>(file_block_num * sample_ratio);
    uint64_t sample_size = sample_block_num * BLOCK_SIZE;

    if (sample_block_num == 0){
        std::cerr << "Error: sample block num is 0" << std::endl;
        exit(-1);
    }

    if (sample_block_num > file_block_num){
        std::cerr << "Error: sample block num is larger than file block num" << std::endl;
        exit(-1);
    }

    vector<uint64_t> all_blocks(file_block_num);
    iota(all_blocks.begin(), all_blocks.end(), 0ULL); // 递增填充

    vector<int64_t> sampled_blocks;
    vector<uint32_t> sampled_blocks_size;
    sampled_blocks.reserve(sample_block_num);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::sample(all_blocks.begin(), all_blocks.end(),
                std::back_inserter(sampled_blocks),
                sample_block_num, gen);


    std::sort(sampled_blocks.begin(), sampled_blocks.end());
    sampled_blocks.back() = file_block_num - 1; 

    for (size_t i = 0; i < sampled_blocks.size(); ++i) {
        uint64_t idx = sampled_blocks[i];       
        off_t offset = idx * BLOCK_SIZE;
        size_t read_size = (offset + BLOCK_SIZE > file_size) ? (file_size - offset) : BLOCK_SIZE;

        // 由于sample cache使用direct io分配，所以必须读取块大小倍数的长度；
        ssize_t ret = pread(idf, sample_cache + i * BLOCK_SIZE, BLOCK_SIZE, offset);
        if (ret == -1) {
            perror("sample pread failed"); 
            break;
        }
        sampled_blocks_size.push_back(read_size);
    }

    /*
        ADRE sample dedup
    */
    for(int i=0; i<=sampled_blocks_size.size()-1; i++){
        uint32_t sample_block_offset = 0;
        while(sample_block_offset < sampled_blocks_size[i]){  
            unsigned char* p = sample_cache + i*BLOCK_SIZE + sample_block_offset;
            // Chunk
            chunk_length = chunking(p, sampled_blocks_size[i] - sample_block_offset);
            
            // Hash
            SHA1(p, chunk_length, (uint8_t*)&tmp_sha1_fp);
            
            // 2. Dedup against all current base FP table
            GlobalMetadataManagerPtr->dedupLookupADRE(tmp_sha1_fp, chunk_length);
        
            // write control
            sample_block_offset += chunk_length;
        }
    }

    /*
        3. compute adr and decide which base table
    */
    float estimated_thDR_N = 0;
    GlobalMetadataManagerPtr->ADREFinal(current_version, file_size, estimated_thDR_N);
    
    for(int i=0; i < file_block_num; i++){

        n_read = read(idf, (void*)file_cache, BLOCK_SIZE);

        cache_offset = 0;
        while(cache_offset < n_read){  
            chunk_length = chunking(file_cache + cache_offset, n_read - cache_offset);
            SHA1(file_cache + cache_offset, chunk_length, (uint8_t*)&tmp_sha1_fp);

            // ScoDe Dynamic Scope Fingerprint Indexing
            LookupResult lookup_result = GlobalMetadataManagerPtr->dedupLookupDSFI(tmp_sha1_fp);

            if(!lookup_result.dup){
                if(container_fake_io){
                    if(container_inner_offset + chunk_length > CONTAINER_SIZE){
                        container_index ++;
                        container_inner_offset = 0;
                        container_inner_offset += chunk_length;
                    }else{
                        container_inner_offset += chunk_length;
                    }

                }else{
                    // save chunk itself
                    saveChunkToContainer(container_buf_pointer, container_buffer, 
                                        container_index, container_inner_offset, container_inner_index,
                                        chunk_length, cache_offset, file_cache, (void*)&tmp_sha1_fp,
                                        Config::getInstance().getContainersPath().c_str());
                    
                    // conatainer write control
                    container_inner_offset += chunk_length;
                    container_buf_pointer += chunk_length;
                    container_inner_index ++;
                }

                // save chunk metadata
                tmp_entry_value.container_number = container_index;
                tmp_entry_value.offset = container_inner_offset;
                tmp_entry_value.chunk_length = chunk_length;
                GlobalMetadataManagerPtr->addNewEntryDSFI(tmp_sha1_fp, tmp_entry_value);
                reference_containers.insert(container_index);
            
            }else{
                dedup_chunks ++;
                dedup_size += chunk_length;
                reference_containers.insert(lookup_result.container_index);

            }

            // Insert fingerprint into file recipe
            file_recipe.push_back(std::string((char*)&tmp_sha1_fp, sizeof(struct SHA1FP)));

            // statistic
            sum_chunks ++;
            sum_size += chunk_length;
            
            // write control
            cache_offset += chunk_length;
        }
    }

    //  flush last container
    if(container_buf_pointer > 0)
        saveContainer(container_index, container_buffer, CONTAINER_SIZE, Config::getInstance().getContainersPath().c_str());
    
    // flush file recipe
    saveFileRecipe(file_recipe, Config::getInstance().getFileRecipesPath().c_str());

    // #th statistic
    log_file << "Sum size "
        << sum_size << " B "
        << std::fixed << std::setprecision(2)
        << (float)sum_size / (1024 * 1024) << " MB "
        << (float)sum_size / (1024 * 1024 * 1024) << " GB" << std::endl;
    
    log_file << "Dedup size " << dedup_size << " B " 
            << std::fixed << std::setprecision(2)
            << (float)dedup_size / (1024 * 1024) << " MB " 
            << (float)dedup_size / (1024 * 1024 * 1024) << " GB" << endl;

    // thDR
    float dedup_ratio_1 = double(dedup_size) / double(sum_size);
    float dedup_ratio_2 = double(sum_size) / (double(sum_size) - double(dedup_size));
    log_file << "thDR Percent Form: " << dedup_ratio_1 << endl;
    log_file << "thDR Decimal Form: " << dedup_ratio_2 << endl;
    GlobalMetadataManagerPtr->appendThDR(dedup_ratio_1, current_version);   

    log_file << "estimated thDR N: " << estimated_thDR_N << endl;

    // RA
    float read_amplification = (double(reference_containers.size()) * CONTAINER_SIZE) / double(sum_size);
    log_file << "Read amplification: " << read_amplification << endl;

    // update backup job
    bj.dedup_chunks += dedup_chunks;
    bj.dedup_size += dedup_size;
    bj.sum_chunks += sum_chunks;
    bj.sum_size += sum_size;
    bj.file_num++;

    /* 
        unique chunk size
        对于 delta 版本来说，就是 delta container size；
        对于 base 版本，不用输出，直接输出为0；
    */
    if( !GlobalMetadataManagerPtr->isBaseVersion() ){
        log_file << "delta container size MB: " << double(sum_size - dedup_size) / (1024 * 1024) << endl;
    }else{
        log_file << "delta container size MB: " << 0 << endl;
    }

    struct backupInfo current_backup_info;
    current_backup_info.backup_size = sum_size;

    if(GlobalMetadataManagerPtr->isBaseVersion()){
        current_backup_info.delta_container_size = 0;
        current_backup_info.base_container_size = sum_size - dedup_size;
        current_backup_info.isBaseVersion = true;
    }else{
        current_backup_info.delta_container_size = sum_size - dedup_size;
        current_backup_info.base_container_size = 0;
        current_backup_info.isBaseVersion = false;
    }
    GlobalMetadataManagerPtr->appendBackupInfo(current_backup_info);
    
    // ADR
    float actual_dratio_1 = double(bj.dedup_size) / double(bj.sum_size);
    float actual_dratio_2 = double(bj.sum_size) / (double(bj.sum_size) - double(bj.dedup_size));
    log_file << "multi source ADR Percent Form: " << actual_dratio_1 << endl;
    log_file << "multi source ADR Decimal Form: " << actual_dratio_2 << endl;
    GlobalMetadataManagerPtr->appendADR(sum_size, dedup_size);
    
    gettimeofday(&end_time, NULL);
    // throughput
    float throughput = double(sum_size) / double((end_time.tv_sec - start_time.tv_sec) + (end_time.tv_usec - start_time.tv_usec) / 1000000.0);
    log_file << "Throughput: " << throughput / (1024 * 1024) << " MB/s" << endl;

    // free 
    close(idf);

    log_file << endl;
}

/*
    观察不同interval值，数据集最终的Actual dedup ratio和Read amplification；
    不需要写容器；
    不需要记录文件的recipe；
*/
void writeFileDedupIntervalObservation(string path, int current_version){
    log_file << "Start write file: " << path << ", method: Dedup Interval Observation" << std::endl;

    int idf = open(path.c_str(), O_RDONLY, 0777);
    if(idf < 0){
        std::printf("open file error, id %d, %s\n", errno, strerror(errno));
        exit(-1);
    }

    uint32_t chunk_length = 0;
    uint32_t file_offset = 0;
    uint32_t n_read = 0;
    struct ENTRY_VALUE entry_value;
    struct SHA1FP tmp_sha1_fp;

    vector<uint64_t> single_file_interval_dedup_size(interval_tasks.size(), 0);
    vector<uint64_t> single_file_interval_sum_size(interval_tasks.size(), 0);
    vector<set<int>> single_file_interval_reference_containers(interval_tasks.size());
    vector<int> container_inner_offsets(interval_tasks.size(), 0);

    unsigned char* file_cache = (unsigned char*)malloc(BLOCK_SIZE);
    for(;;){
        file_offset = 0;

        n_read = read(idf, file_cache, BLOCK_SIZE);

        if(n_read <= 0){
            break;
        }

        while(file_offset < n_read){  
            // Chunk
            chunk_length = chunking(file_cache + file_offset, n_read - file_offset);
            
            // Hash
            std::memset(&tmp_sha1_fp, 0, sizeof(struct SHA1FP));
            SHA1(file_cache + file_offset, chunk_length, (uint8_t*)&tmp_sha1_fp);

            // Batch Query
            for(int i=0; i<=interval_tasks.size()-1; i++){
                int current_interval = interval_tasks[i].interval;
                int base_version = current_version / current_interval * current_interval;
                
                LookupResult lookup_result = GlobalMetadataManagerPtr->dedupLookup(tmp_sha1_fp, base_version, current_version, current_interval); 
                if(!lookup_result.dup){
                    // 模拟chunk存储；
                    if(container_inner_offsets[i] + chunk_length > CONTAINER_SIZE){
                        container_indice[i] ++;
                        container_inner_offsets[i] = 0;
                        container_inner_offsets[i] += chunk_length;
                    }else{
                        container_inner_offsets[i] += chunk_length;
                    }

                    entry_value.container_number = container_indice[i];
                    GlobalMetadataManagerPtr->addNewEntry(tmp_sha1_fp, entry_value, current_version, current_interval);
                    single_file_interval_reference_containers[i].insert(container_indice[i]);

                }else{
                    single_file_interval_dedup_size[i] += chunk_length;
                    single_file_interval_reference_containers[i].insert(lookup_result.container_index);

                }

                single_file_interval_sum_size[i] += chunk_length;
            }
            
            file_offset += chunk_length;
        }
    }

    for(int i=0; i<=interval_tasks.size()-1; i++){
        interval_tasks[i].dedup_size += single_file_interval_dedup_size[i];
        interval_tasks[i].sum_size += single_file_interval_sum_size[i];
        interval_tasks[i].total_container_reference += single_file_interval_reference_containers[i].size();
        interval_tasks[i].file_num ++;

    }

    // free 
    close(idf);
    free(file_cache);

    log_file << "Finish write file" << endl;
}

/*
    模拟；
    不写container；
    Dedup First;
    写一个版本对命中的所有window计算；
*/
void writeFileDedupWindowObservation(string path, int current_version){
    log_file << "Start write file: " << path << ", method: Dedup Window Observation" << std::endl;

    int idf = open(path.c_str(), O_RDONLY, 0777);
    if(idf < 0){
        std::printf("open file error, id %d, %s\n", errno, strerror(errno));
        exit(-1);
    }
    
    // control
    uint32_t chunk_length = 0;
    uint32_t file_offset = 0;
    uint32_t n_read = 0;
    struct ENTRY_VALUE tmp_entry_value;
    struct SHA1FP tmp_sha1_fp;

    // 我们是人为指定window，这里检查能框住这个版本的window
    vector<int> windows_start_for_this_version;
    for(int i=0; i<=windows_start.size()-1; i++){
        if(windows_start[i] <= current_version && current_version < windows_start[i] + window_size){
            windows_start_for_this_version.push_back(windows_start[i]);
        }
    }
    int valid_window_num = windows_start_for_this_version.size();

    // statistic
    vector<uint64_t> multi_file_dedup_size(valid_window_num, 0);
    vector<uint64_t> multi_file_sum_size(valid_window_num, 0);

    // control: multi container write 
    vector<int> multi_file_container_inner_offsets(valid_window_num, 0);
    vector<set<int>> multi_file_container_reference(valid_window_num);

    unsigned char* file_cache = (unsigned char*)malloc(BLOCK_SIZE);
    for(;;){
        file_offset = 0;

        n_read = read(idf, file_cache, BLOCK_SIZE);

        if(n_read <= 0){
            break;
        }

        while(file_offset < n_read){  
            // Chunk
            chunk_length = chunking(file_cache + file_offset, n_read - file_offset);
            
            // Hash
            std::memset(&tmp_sha1_fp, 0, sizeof(struct SHA1FP));
            SHA1(file_cache + file_offset, chunk_length, (uint8_t*)&tmp_sha1_fp);

            /*
                multi windows
                dedup first, base = 0;
            */
            for(int i=0; i<=windows_start_for_this_version.size()-1; i++){
                int window_start = windows_start_for_this_version[i];
                LookupResult lookup_result = GlobalMetadataManagerPtr->dedupLookup(tmp_sha1_fp, 0, current_version - window_start, window_start);
                if(!lookup_result.dup){
                    // 模拟chunk存储
                    if(multi_file_container_inner_offsets[i] + chunk_length > CONTAINER_SIZE){
                        window_results[window_start].current_container_index ++;
                        multi_file_container_inner_offsets[i] = 0;
                        multi_file_container_inner_offsets[i] += chunk_length;
                    }else{
                        multi_file_container_inner_offsets[i] += chunk_length;
                    }

                    tmp_entry_value.container_number = window_results[window_start].current_container_index;
                    GlobalMetadataManagerPtr->addNewEntry(tmp_sha1_fp, tmp_entry_value, current_version - window_start, window_start);
                    multi_file_container_reference[i].insert(tmp_entry_value.container_number);

                }else{
                    multi_file_dedup_size[i] += chunk_length;
                    multi_file_container_reference[i].insert(lookup_result.container_index);

                }
            
                // statistic
                multi_file_sum_size[i] += chunk_length; 
            }
            
            // control
            file_offset += chunk_length;
        }
    }

    for(int i=0; i<=valid_window_num-1; i++){
        int window_index = windows_start_for_this_version[i];
        // actual dr
        window_results[window_index].dup_size += multi_file_dedup_size[i];
        window_results[window_index].sum_size += multi_file_sum_size[i];
        window_results[window_index].current_actual_dr = (float)window_results[window_index].dup_size / (float)window_results[window_index].sum_size;
        window_results[window_index].actual_drs.push_back(window_results[window_index].current_actual_dr);
        if(window_results[window_index].current_actual_dr > window_results[window_index].peak_actual_dr){
            window_results[window_index].peak_actual_dr = window_results[window_index].current_actual_dr;
            window_results[window_index].peak_actual_dr_offset = current_version - window_index;
        }

        // read amplification
        float read_amplification = ((float)multi_file_container_reference[i].size() * CONTAINER_SIZE) / (float)multi_file_sum_size[i];
        window_results[window_index].read_amplifications.push_back(read_amplification);
    }

    // free 
    close(idf);
    free(file_cache);

    log_file << "Finish write file" << endl;
}

void writeFileDedupFirstEstimationDR(string path, int current_version){

    int idf = open(path.c_str(), O_RDONLY | O_DIRECT);
    if(idf < 0){
        std::printf("open file error, id %d, %s\n", errno, strerror(errno));
        exit(-1);
    }

    uint64_t file_size = 0;
    struct stat file_stat;
    stat(path.c_str(), &file_stat);
    file_size = file_stat.st_size;

    // 硬编码 sample ratio
    // 第一个文件实际上不用统计DR；
    struct ResultBySampleRatio{
        float sample_ratio;
        float thDR;
        uint64_t sum_size;
        uint64_t dup_size;
    };
    vector<ResultBySampleRatio> results_by_sample_ratio;
    results_by_sample_ratio.push_back({.sample_ratio = 0.05, 0, 0, 0});
    results_by_sample_ratio.push_back({.sample_ratio = 0.1, 0, 0, 0});
    results_by_sample_ratio.push_back({.sample_ratio = 0.2, 0, 0, 0});

    // control
    uint32_t chunk_length = 0;
    uint32_t file_cache_offset = 0;
    uint32_t sample_cache_offset = 0;
    uint32_t n_read = 0;
    struct ENTRY_VALUE tmp_entry_value;
    struct SHA1FP tmp_sha1_fp;

    if(current_version == 0){ // self dedup
        for(;;){
            file_cache_offset = 0;

            n_read = read(idf, file_cache, BLOCK_SIZE);

            if(n_read <= 0){
                break;
            }

            while(file_cache_offset < n_read){  
                chunk_length = chunking(file_cache + file_cache_offset, n_read - file_cache_offset);
                SHA1(file_cache + file_cache_offset, chunk_length, (uint8_t*)&tmp_sha1_fp);
                LookupResult lookup_result = GlobalMetadataManagerPtr->dedupLookupES(tmp_sha1_fp, current_version);
                
                if(!lookup_result.dup){
                    GlobalMetadataManagerPtr->addNewEntryES(tmp_sha1_fp, tmp_entry_value, current_version);
                }else{
                    ;
                }

                file_cache_offset += chunk_length; 
            }

        }

    }else{ 
        for(auto& result : results_by_sample_ratio){
            GlobalMetadataManagerPtr->ESClear();

            // sampling
            uint64_t file_block_num = (file_size + BLOCK_SIZE - 1) / BLOCK_SIZE; // 向上取整
            float sample_ratio = result.sample_ratio;
            uint64_t sample_block_num = static_cast<uint64_t>(file_block_num * sample_ratio);
            uint64_t sample_size = sample_block_num * BLOCK_SIZE;

            if (sample_block_num == 0){
                std::cerr << "Error: sample block num is 0" << std::endl;
                exit(-1);
            }

            if (sample_block_num > file_block_num){
                std::cerr << "Error: sample block num is larger than file block num" << std::endl;
                exit(-1);
            }

            vector<uint64_t> all_blocks(file_block_num);
            iota(all_blocks.begin(), all_blocks.end(), 0ULL); // 递增填充

            vector<int64_t> sampled_blocks;
            vector<uint32_t> sampled_blocks_size;
            sampled_blocks.reserve(sample_block_num);

            std::random_device rd;
            std::mt19937 gen(rd());
            std::sample(all_blocks.begin(), all_blocks.end(),
                        std::back_inserter(sampled_blocks),
                        sample_block_num, gen);

            std::sort(sampled_blocks.begin(), sampled_blocks.end());

            for (size_t i = 0; i < sampled_blocks.size(); ++i) {
                uint64_t idx = sampled_blocks[i];       
                off_t offset = idx * BLOCK_SIZE;
                size_t read_size = (offset + BLOCK_SIZE > file_size) ? (file_size - offset) : BLOCK_SIZE;
                ssize_t ret = pread(idf, sample_cache + i * BLOCK_SIZE, read_size, offset);
                if (ret == -1) {
                    perror("sample pread failed"); 
                    break;
                }
                sampled_blocks_size.push_back(read_size);
            }

            // dedup (sample against first)
            for(int i=0; i<=sampled_blocks_size.size()-1; i++){

                uint32_t sample_block_offset = 0;
                while(sample_block_offset < sampled_blocks_size[i]){  
                    chunk_length = chunking(sample_cache + i*BLOCK_SIZE + sample_block_offset, sampled_blocks_size[i] - sample_block_offset);
                    SHA1(sample_cache + i*BLOCK_SIZE + sample_block_offset, chunk_length, (uint8_t*)&tmp_sha1_fp);
                    LookupResult lookup_result = GlobalMetadataManagerPtr->dedupLookupES(tmp_sha1_fp, current_version);
                    
                    if(!lookup_result.dup){
                        GlobalMetadataManagerPtr->addNewEntryES(tmp_sha1_fp, tmp_entry_value, current_version);
                    }else{
                        result.dup_size += chunk_length;   
                    }

                    result.sum_size += chunk_length;   
                    sample_block_offset += chunk_length; 
                }
            }
        }
    }

    // statistic
    if(current_version != 0){
        log_file << " version: "    << current_version;

        for(auto& result : results_by_sample_ratio){
            result.thDR = result.dup_size * 1.0 / result.sum_size;

            log_file << " sample ratio: " << result.sample_ratio
                     << " thDR: "         << result.thDR;
        }

        log_file << endl;
    }

    close(idf);
}

/*
    默认 FAKE container IO；
    模拟MFDedup流程，主要用于观察数据迁移的量；

    从第二个版本开始，才有数据迁移，第一个版本不会触发数据迁移；
*/
void writeFileObservationMigrationMFDedup(string path, int current_version){
    log_file << "Start write file: " << path << ", method:  Observation Migration MFDedup" << std::endl;

    int idf = open(path.c_str(), O_RDONLY | O_DIRECT);
    if(idf < 0){
        std::printf("open file error, id %d, %s\n", errno, strerror(errno));
        exit(-1);
    }

    // write control
    uint32_t chunk_length = 0;
    uint32_t file_offset = 0;
    uint32_t n_read = 0;
    struct ENTRY_VALUE tmp_entry_value;
    struct SHA1FP tmp_sha1_fp;

    // statistic
    uint64_t single_file_dedup_chunks = 0;
    uint64_t single_file_dedup_size = 0;
    uint64_t single_file_sum_chunks = 0;
    uint64_t single_file_sum_size = 0;

    // write file loop
    GlobalMetadataManagerPtr->MFDedupNewTable();

    for(;;){
        file_offset = 0;

        n_read = read(idf, (void*)file_cache, BLOCK_SIZE);

        if(n_read <= 0){
            break;
        }

        while(file_offset < n_read){  
            // Chunk
            chunk_length = chunking(file_cache + file_offset, n_read - file_offset);
            tmp_entry_value.chunk_length = chunk_length;
            
            // Hash
            SHA1(file_cache + file_offset, chunk_length, (uint8_t*)&tmp_sha1_fp);
            
            // Dedup
            LookupResult lookup_result = GlobalMetadataManagerPtr->dedupLookupMFDedup(tmp_sha1_fp, tmp_entry_value);

            if(lookup_result.dup){
                single_file_dedup_chunks ++;
                single_file_dedup_size += chunk_length;
            }

            // Statistic
            single_file_sum_chunks ++;
            single_file_sum_size += chunk_length;

            file_offset += chunk_length;
        }
    }

    GlobalMetadataManagerPtr->MFDedupMigration();

    // #th statistic
    log_file << "Sum chunks " << single_file_sum_chunks << endl;
    log_file << "Sum size " << single_file_sum_size << endl;
    log_file << "Dedup chunks " << single_file_dedup_chunks << endl;
    log_file << "Dedup size " << single_file_dedup_size << endl;
    
    float thDR = double(single_file_dedup_size) / double(single_file_sum_size);
    log_file << "th DR percentage format: " << thDR << endl;

    // 这里图省事，只用到最后一个；
    vector<uint64_t> th_archive_data_size = GlobalMetadataManagerPtr->getArhiveDataSizeTH();
    vector<uint64_t> th_migrate_data_size = GlobalMetadataManagerPtr->getMigrationDataSizeTH();
    log_file << " th archive data size " << th_archive_data_size.back() << endl;
    log_file << " th migrate data size " << th_migrate_data_size.back() << endl;
    log_file << " th archive data portion " << th_archive_data_size.back() * 1.0 / single_file_sum_size << endl;
    log_file << " th migrate data portion " << th_migrate_data_size.back() * 1.0 / single_file_sum_size << endl;

    // update backup job
    bj.dedup_chunks += single_file_dedup_chunks;
    bj.dedup_size += single_file_dedup_size;
    bj.sum_chunks += single_file_sum_chunks;
    bj.sum_size += single_file_sum_size;
    bj.file_num++;

    // free 
    close(idf);

    log_file << "Finish write file" << endl;
}

std::vector<fs::path> traverseDirectory(const fs::path& directory) {
    try {
        std::vector<fs::path> files;

        // 遍历目录
        for (const auto& entry : fs::directory_iterator(directory)) {
            if (fs::is_regular_file(entry)) {
                files.push_back(entry.path());
            } else if (fs::is_directory(entry)) {
                traverseDirectory(entry.path());
            }
        }

        return files;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << std::endl;
        return std::vector<fs::path>();
    }
}

void traverseFilesList(string files_list) {
    enum DedupType dt = Config::getInstance().getDedupType();

    try {
        // get files path
        std::vector<string> files;
        std::ifstream file(files_list);
        if (file.is_open()) {
            string line;
            while (getline(file, line)) {
                files.push_back(line);
            }
            file.close();
        }

        if(dt == DedupType::DedupNaive){
            for (const auto& path : files){
                writeFileNaive(path);
            } 

        }else if(dt == DedupType::DedupFirst){
            GlobalMetadataManagerPtr->reserveDedupIntervalTable(files.size());
            int current_version = 0;
            for (const auto& path : files){
                writeFileDedupFirst(path, current_version++);
            } 

        }else if(dt == DedupType::DedupFixedInterval){
            GlobalMetadataManagerPtr->reserveDedupIntervalTable(files.size());
            int current_version = 0;
            int interval = Config::getInstance().getInterval();
            for (const auto& path : files){
                writeFileDedupFixedInterval(path, current_version++, interval);
            } 

        }else if(dt == DedupType::DedupScode){
            // write files
            GlobalMetadataManagerPtr->ScodeInit();
            int current_version = 0;
            for (const auto& path : files){
                writeFileDedupScode(path, current_version++);
            } 

            // Data Churn 敏感性测试，模拟删除老旧版本，然后观察actual deduplication raito；
            GlobalMetadataManagerPtr->simulateDataChurn();

            // print statistic
            GlobalMetadataManagerPtr->ScodePrintStatistics(log_file);

        }else{
            std::cerr << "Error: Not support dedup type" << std::endl;
        }

    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << std::endl;
    }
}

void traverseWriteDirectory(const fs::path& directory) {
    enum DedupType dt = Config::getInstance().getDedupType();

    try {
        std::vector<fs::path> files = traverseDirectory(directory);
        std::sort(files.begin(), files.end());

        if(dt == DedupType::DedupNaive){
            for (const auto& path : files){
                writeFileNaive(path);
            } 

        }else if(dt == DedupType::DedupFixedInterval){
            int interval = Config::getInstance().getInterval();
            for (const auto& path : files){
                //writeFileDedupInterval(path);
            } 

        }else if(dt == DedupType::DedupFirst){
            for (const auto& path : files){
                //writeFileDedupFirst(path);
            } 

        }else{
            std::cerr << "Error: Not support dedup type" << std::endl;
        }

    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << std::endl;
    }
}

void taskMemoryAllocation(){
    int ret = posix_memalign((void**)&file_cache, SECTOR_SIZE, BLOCK_SIZE);
    if(ret != 0){
        std::cerr << "Error: posix_memalign file cahce failed" << std::endl;
        exit(-1);
    }

    ret = posix_memalign((void**)&sample_cache, SECTOR_SIZE, SAMPLE_CACHE);
    if(ret != 0){
        std::cerr << "Error: posix_memalign sample failed" << std::endl;
        exit(-1);
    }

    ret = posix_memalign((void**)&container_buffer, SECTOR_SIZE, CONTAINER_SIZE);
    if(ret != 0){
        std::cerr << "Error: posix_memalign container failed" << std::endl;
        exit(-1);
    }
}

std::vector<string> getAllFiles(string files_list_path){
    if (!fs::exists(files_list_path)) {
        std::cerr << "Error: files list path does not exist." << std::endl;
        exit(-1);
    }

    std::vector<string> all_files;
    if (fs::is_directory(files_list_path)) {
        std::cerr << "Error: Input path is a directory." << std::endl;
        exit(-1);
    }else{
        std::ifstream file(files_list_path);
        if (file.is_open()) {
            string line;
            while (getline(file, line)) {
                all_files.push_back(line);
            }
            file.close();
        }
    }

    return all_files;
}

int main(int argc, char** argv){
    // 参数解析
    Config::getInstance().parse_argument(argc, argv);

    // 全局统计信息解析
    GlobalStat::getInstance().parse_arguments(global_stat_path);
    
    GlobalMetadataManagerPtr = new MetadataManager(Config::getInstance().getFingerprintsFilePath().c_str());
    
    initChunkingAlgorithm();
    
    /*
        打开日志文件，并且截断为0；
    */
    dedup_log_path = Config::getInstance().getDedupLogPath();
    log_file.open(dedup_log_path, std::ios::out | std::ios::trunc);
    if(!log_file.is_open()){
        std::cerr << "Error: Open log file failed" << std::endl;
        exit(-1);
    }

    /*
        写任务
        初始化文件cache和container buffer
    */
    taskMemoryAllocation();

    // 不支持断续写入
    if(Config::getInstance().getTaskType() == TASK_WRITE){
        string files_list = Config::getInstance().getInputPath();

        if (!fs::exists(files_list)) {
            std::cerr << "Error: files list does not exist." << std::endl;
            return 1;
        }

        struct timeval backup_time_start, backup_time_end;
        gettimeofday(&backup_time_start, NULL);
        if (fs::is_directory(files_list)) {
            std::cerr << "Error: Input path is a directory." << std::endl;
            exit(-1);
        }else{
            traverseFilesList(files_list);
        }
        gettimeofday(&backup_time_end, NULL);

        // throughput
        uint64_t single_dedup_time_us = (backup_time_end.tv_sec - backup_time_start.tv_sec) * 1000000 + 
                                         backup_time_end.tv_usec - backup_time_start.tv_usec;
        float throughput = (float)(bj.sum_size) / MB / ((float)(single_dedup_time_us)/1000000);

        // 写文件 - 重删统计
        std::printf("-----------------------Dedup statics----------------------\n");
        std::printf("Hash collision num %" PRIu64 "\n",    bj.hash_collision_sum); // should be zero
        std::printf("Sum chunks num %" PRIu64 "\n",       bj.sum_chunks);
        std::printf("Sum data size %" PRIu64 "\n",         bj.sum_size);
        std::printf("Dedup chunks num %" PRIu64 "\n",      bj.dedup_chunks);
        std::printf("Dedup data size %" PRIu64 "\n",       bj.dedup_size);
        std::printf("Average chunk size %" PRIu64 "\n",    bj.sum_size / bj.sum_chunks);
        std::printf("-----------------------statics----------------------\n");
        std::printf("Backup Throughput %.2f MiB/s\n",    throughput);
        std::printf("Dedup Ratio %.2f%%\n",     double(bj.dedup_size) / double(bj.sum_size) *100);

        // 保存全局信息
        GlobalStat::getInstance().update(bj.sum_size, bj.sum_size - bj.dedup_size);
        GlobalStat::getInstance().save_arguments(global_stat_path);
        
        // save metadata entry
        GlobalMetadataManagerPtr->save();

    }else if(Config::getInstance().getTaskType() == TASK_OBSERVATION_INTERVAL){
        // init interval task
        interval_tasks.resize(20);
        for(int i=5; i<=100; i+=5){
            interval_tasks[i/5-1].interval = i;
            interval_tasks[i/5-1].actual_dr = 0;
            interval_tasks[i/5-1].avg_read_amplification = 0;
            interval_tasks[i/5-1].sum_size = 0;
            interval_tasks[i/5-1].dedup_size = 0;
            interval_tasks[i/5-1].total_container_reference = 0;
            interval_tasks[i/5-1].file_num = 0;

            intervals.push_back(i);
            container_indice.push_back(0);
        }

        string files_list = Config::getInstance().getInputPath();
        if (!fs::exists(files_list)) {
            std::cerr << "Error: files list does not exist." << std::endl;
            return 1;
        }

        if (fs::is_directory(files_list)) {
            std::cerr << "Error: Input path is a directory." << std::endl;
            exit(-1);
        }else{
            std::vector<string> files;
            std::ifstream file(files_list);
            if (file.is_open()) {
                string line;
                while (getline(file, line)) {
                    files.push_back(line);
                }
                file.close();
            }

            for(int i=0; i<=interval_tasks.size()-1; i++){
                GlobalMetadataManagerPtr->reserveDedupIntervalTablesByGroup(files.size(), (i+1)*5);
            }

            int current_version = 0;
            for (const auto& path : files){
                writeFileDedupIntervalObservation(path, current_version++);
            } 

            log_file << "-----------------------Dedup statics----------------------\n";
            for(auto& task: interval_tasks){
                task.avg_read_amplification = ((float)task.total_container_reference * CONTAINER_SIZE) / (float)task.sum_size;
                log_file << "interval: " << task.interval << " avg read amplification: " << task.avg_read_amplification << "\n";
            }

            for(auto& task: interval_tasks){
                task.actual_dr = double(task.dedup_size) / double(task.sum_size) * 100;
                log_file << "interval: " << task.interval << " actual_dr: " << task.actual_dr << "\n";
            }
        }

    }else if(Config::getInstance().getTaskType() == TASK_OBSERVATION_WINDOW){
        vector<string> all_files = getAllFiles(Config::getInstance().getInputPath());

        // windows init
        for(int i=0; i<=windows_start.size()-1; i++){
            window_results[windows_start[i]].current_container_index = 0;
            window_results[windows_start[i]].peak_actual_dr = 0;
            window_results[windows_start[i]].peak_actual_dr_offset = 0;
            window_results[windows_start[i]].files.assign(all_files.begin() + i,
                                           all_files.begin() + i + window_size);
        }

        // init global metadata
        for(int i=0; i<=windows_start.size()-1; i++){
            GlobalMetadataManagerPtr->reserveDedupIntervalTablesByGroup(window_size, windows_start[i]);
        }

        for(int i=0; i<=all_files.size()-1; i++){
            writeFileDedupWindowObservation(all_files[i], i);
        }

        log_file << "-----------------------Dedup statics----------------------\n";
        for(auto & window_result: window_results){
            log_file << "window "                  << window_result.first
                     << " peak_actual_dr: "        << window_result.second.peak_actual_dr
                     << " peak_actual_dr_offset: " << window_result.second.peak_actual_dr_offset << "\n";
        }

        // ADR and RA
        for(auto & window_result: window_results){
            log_file << "window " << window_result.first << "\n";
            for(int j=0; j<=window_result.second.actual_drs.size()-1; j++) {
                log_file <<" ADR "<< window_result.second.actual_drs[j] << " ";
                log_file <<" RA "<< window_result.second.read_amplifications[j] << "\n";
            }
        }

    }else if(Config::getInstance().getTaskType() == TASK_DEDUP_FIRST_ESTIMATION){
        vector<string> all_files = getAllFiles(Config::getInstance().getInputPath());
        int version_num = 10; // 硬编码只测试少量版本； 

        for(int i=0; i<version_num ; i++){
            writeFileDedupFirstEstimationDR(all_files[i], i);
        }
    }else if(Config::getInstance().getTaskType() == TASK_OBSERVATION_MIGRATION_MFDedup){
        /*
            指标1 每个版本迁移
            指标2 数据集总迁移
            如果是混合负载，实际上MFDedup不会有任何数据迁移
            GCCDF 的可以不用实现，利用MFDedup反推结果(依据论文的里面写的比例)；
        */

        vector<string> all_files = getAllFiles(Config::getInstance().getInputPath());

        for(int i=0; i<all_files.size() ; i++){
            writeFileObservationMigrationMFDedup(all_files[i], i);
        }
    }
    // }else if(Config::getInstance().getTaskType() == TASK_RESTORE){
    //     // 如果写时使用DeltaDedup，那么恢复时参数也需要指定DeltaDedup
    //     int restore_version = Config::getInstance().getRestoreVersion();
    //     string recipe_path = Config::getInstance().getFileRecipesPath();

    //     int current_version = getFilesNum(recipe_path.c_str());
    //     if(current_version != 0){
    //         if(Config::getInstance().isDeltaDedup()){
    //             GlobalMetadataManagerPtr->load(restore_version);
    //         }else{
    //             GlobalMetadataManagerPtr->load();
    //         }
    //     }

    //     struct timeval restore_time_start, restore_time_end;
    //     gettimeofday(&restore_time_start, NULL);

    //     if(!fileRecipeExist(restore_version, recipe_path.c_str())){
    //         std::printf("Version %d not exist!\n", restore_version);
    //         return 0;
    //     }
        
    //     unsigned char* assembling_buffer = (unsigned char*)malloc(FILE_CACHE);
    //     memset(assembling_buffer, 0, FILE_CACHE);
    //     int write_buffer_offset = 0;
    //     int restored_size = 0;

    //     //recipe
    //     std::vector<std::string> file_recipe = getFileRecipe(Config::getInstance().getRestoreVersion(),
    //                                                          Config::getInstance().getFileRecipesPath().c_str());

    //     //组装
    //     RESTORE_METHOD rm = Config::getInstance().getRestoreMethod();
    //     if(rm == CONTAINER_CACHE || rm == CHUNK_CACHE){
    //         int fd = open(Config::getInstance().getRestorePath().c_str(), O_RDWR | O_CREAT, 0777);
    //         if(fd < 0){
    //             std::printf("无法写文件!!! %s\n", strerror(errno));
    //             exit(-1);
    //         }

    //         Cache* cc;
    //         if(rm == CONTAINER_CACHE){
    //             cc = new ContainerCache(Config::getInstance().getContainersPath().c_str(), 16);
    //         }
            
    //         SHA1FP fp;
    //         ENTRY_VALUE ev;
    //         for(auto &x : file_recipe){
    //             memcpy(&fp, x.data(), sizeof(SHA1FP));
    //             ev = GlobalMetadataManagerPtr->getEntry(fp);
    //             std::string ck_data = cc->getChunkData(ev);

    //             if(write_buffer_offset + ck_data.size() >= FILE_CACHE){
    //                 flushAssemblingBuffer(fd, assembling_buffer, write_buffer_offset);
    //                 write_buffer_offset = 0;
    //             }

    //             memcpy(assembling_buffer + write_buffer_offset, ck_data.data(), ck_data.size());

    //             write_buffer_offset += ev.chunk_length;
    //             restored_size += ev.chunk_length;
    //         }

    //         flushAssemblingBuffer(fd, assembling_buffer, write_buffer_offset);
    //         close(fd);

    //     }else{
    //         std::printf("暂不支持的恢复算法 %d\n", Config::getInstance().getRestoreMethod());
    //         exit(-1);
    //     }

    //     gettimeofday(&restore_time_end, NULL);
    //     uint64_t single_dedup_time_us = (restore_time_end.tv_sec - restore_time_start.tv_sec) * 1000000 + restore_time_end.tv_usec - restore_time_start.tv_usec;
    //     float restore_throughput = (float)(restored_size) / MB / ((float)(single_dedup_time_us)/1000000);
    //     std::printf("-----------------------Restore statics----------------------\n");
    //     std::printf("Restore size %d\n", restored_size);
    //     std::printf("Restore Throughput %.2f MiB/s\n", restore_throughput);

    // }

    return 0;
}