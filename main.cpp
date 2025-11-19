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



char* global_stat_path = "/home/cyf/cDedup/global_stat.json";
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

uint32_t getFilesNum(const char* dirPath){
    int ans = 0;
    DIR *dir = opendir(dirPath);
    if(!dir){
        std::printf("getFilesNum opendir error, id %d, %s, the dir is %s\n", 
        errno, strerror(errno), dirPath);
        closedir(dir);
        exit(-1);
    }
    struct dirent* ptr;
    while(readdir(dir)) ans++;
    closedir(dir);
    return ans-2;
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
    int fd = open(container_name.data(), O_RDWR | O_CREAT, 0777);
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
        saveContainer(container_index, container_buf, container_buf_pointer, containers_path);
        std::memset(container_buf, 0, CONTAINER_SIZE);
        container_index++;
        container_inner_offset = 0;
        container_buf_pointer = 0;
        container_inner_index = 0;

        rev_container_cnt = 0;
    }

    // container buffer
    memcpy(container_buf + container_buf_pointer, file_cache + file_offset, chunk_length);
    memcpy(rev_container_buf + sizeof(SHA1FP)*rev_container_cnt, SHA_buf, sizeof(SHA1FP));

}

void flushAssemblingBuffer(int fd, unsigned char* buf, int len){
    if(write(fd, buf, len) != len){
        std::printf("Restore, write file error!!!\n");
        exit(-1);
    }
    //close(fd);
}

void initChunkingAlgorithm(){
    if(Config::getInstance().getChunkingMethod() == CDC){
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
    }else if(Config::getInstance().getChunkingMethod() == FSC){
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
    }
}

void writeFileNaive(string path){
    log_file << "Start write file: " << path << ", method: naive" << std::endl;

    int idf = open(path.c_str(), O_RDONLY);
    if(idf < 0){
        std::printf("open file error, id %d, %s\n", errno, strerror(errno));
        exit(-1);
    }

    unsigned char* file_cache = (unsigned char*)malloc(FILE_CACHE);
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

    // 当前文件重删统计
    uint64_t dedup_chunks = 0;
    uint64_t dedup_size = 0;
    uint64_t sum_chunks = 0;
    uint64_t sum_size = 0;
    uint64_t hash_collision_sum = 0;
    set<int> reference_containers;
    
    // 普通分块重删，来一个块查寻一次，然后把non-duplicate chunk保存到container去
    for(;;){
        file_offset = 0;

        n_read = read(idf, file_cache, FILE_CACHE);

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
                tmp_entry_value.ref_cnt = 1;

                GlobalMetadataManagerPtr->addNewEntry(tmp_sha1_fp, tmp_entry_value);
                reference_containers.insert(container_index);

                // rev
                container_inner_offset += chunk_length;
                container_buf_pointer += chunk_length;
                container_inner_index ++;
                rev_container_cnt ++;

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

void writeFileDedupFirst(string path){
    log_file << "Start write file: " << path << ", method: Dedup First" << std::endl;

    int idf = open(path.c_str(), O_RDONLY, 0777);
    if(idf < 0){
        std::printf("open file error, id %d, %s\n", errno, strerror(errno));
        exit(-1);
    }

    unsigned char* file_cache = (unsigned char*)malloc(FILE_CACHE);

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

    uint32_t current_version = getFilesNum(Config::getInstance().getFileRecipesPath().c_str());
    
    for(;;){
        file_offset = 0;

        n_read = read(idf, file_cache, FILE_CACHE);

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
            LookupResult lookup_result = GlobalMetadataManagerPtr->dedupLookupDedupFirst(tmp_sha1_fp, current_version);

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

// void writeFileDedupInterval(string path){
//     int idf = open(path.c_str(), O_RDONLY, 0777);
//     if(idf < 0){
//         std::printf("open file error, id %d, %s\n", errno, strerror(errno));
//         exit(-1);
//     }

//     unsigned char* file_cache = (unsigned char*)malloc(FILE_CACHE);
//     struct SHA1FP tmp_sha1_fp;
//     std::vector<std::string> file_recipe; // 保存这个文件所有块的指纹

//     // metadata entry(except FP)
//     uint32_t container_index = getFilesNum(Config::getInstance().getContainersPath().c_str());
//     uint32_t container_inner_offset = 0;
//     uint32_t chunk_length = 0;
//     uint16_t container_inner_index = 0;

//     unsigned char container_buf[CONTAINER_SIZE]={0};
//     unsigned int container_buf_pointer = 0;
//     uint32_t file_offset = 0;
//     uint32_t n_read = 0;

//     struct ENTRY_VALUE entry_value;

//     // 重删统计
//     uint64_t dedup_chunks = 0;
//     uint64_t dedup_size = 0;
//     uint64_t sum_chunks = 0;
//     uint64_t sum_size = 0;
//     uint64_t hash_collision_sum = 0;

//     // delta重删
//     uint32_t current_version = getFilesNum(Config::getInstance().getFileRecipesPath().c_str());
//     uint32_t base_size = Config::getInstance().getBaseSize();
//     uint32_t delta_num = Config::getInstance().getDeltaNum();
//     uint32_t min_destination_base = current_version -  current_version % (base_size + delta_num);
//     uint32_t max_destination_base = min_destination_base + base_size - 1;
//     bool in_delta = (current_version % (base_size + delta_num)) > (base_size-1);
    
//     // 普通分块重删，来一个块查寻一次，然后把non-duplicate chunk保存到container去
//     for(;;){
//         file_offset = 0;

//         n_read = read(idf, file_cache, FILE_CACHE);

//         if(n_read <= 0){
//             break;
//         }

//         while(file_offset < n_read){  
//             // Chunk
//             chunk_length = chunking(file_cache + file_offset, n_read - file_offset);
            
//             // Hash
//             std::memset(&tmp_sha1_fp, 0, sizeof(struct SHA1FP));
//             SHA1(file_cache + file_offset, chunk_length, (uint8_t*)&tmp_sha1_fp);

//             // Dedup
//             LookupResult lookup_result;
            
//             if(dd)
//                 lookup_result = GlobalMetadataManagerPtr->dedupLookup(tmp_sha1_fp, in_delta); 
//             else
//                 lookup_result = GlobalMetadataManagerPtr->dedupLookup(tmp_sha1_fp);

//             // 
//             if(lookup_result == Unique){
//                 // save chunk itself
//                 saveChunkToContainer(container_buf_pointer, container_buf, 
//                                     container_index, container_inner_offset, container_inner_index,
//                                     chunk_length, file_offset, file_cache, (void*)&tmp_sha1_fp,
//                                     Config::getInstance().getContainersPath().c_str());
                
//                 // save chunk metadata
//                 entry_value.container_number = container_index;
//                 entry_value.offset = container_inner_offset;
//                 entry_value.chunk_length = chunk_length;
//                 entry_value.container_inner_index = container_inner_index;
//                 entry_value.version = current_version;
//                 entry_value.ref_cnt = 1;

//                 if(dd){
//                     GlobalMetadataManagerPtr->addNewEntry(tmp_sha1_fp, entry_value, in_delta);
//                 }else{
//                     GlobalMetadataManagerPtr->addNewEntry(tmp_sha1_fp, entry_value);
//                 }

//                 // rev
//                 container_inner_offset += chunk_length;
//                 container_buf_pointer += chunk_length;
//                 container_inner_index ++;
//                 rev_container_cnt ++;

//             }else if(lookup_result == Dedup){
//                 dedup_chunks ++;
//                 dedup_size += chunk_length;
//             }

//             // Insert fingerprint into file recipe
//             file_recipe.push_back(std::string((char*)&tmp_sha1_fp, sizeof(struct SHA1FP)));

//             // Statistic
//             sum_chunks ++;
//             sum_size += chunk_length;

//             file_offset += chunk_length;
//         }
//     }

//     //  flush最后一个container
//     if(container_buf_pointer > 0)
//         saveContainer(container_index, container_buf, 
//                         container_buf_pointer, Config::getInstance().getContainersPath().c_str());
    
//     // flush file_recipe
//     saveFileRecipe(file_recipe, Config::getInstance().getFileRecipesPath().c_str());
//     // std::printf("Sum chunks %" PRIu64 "\n",    sum_chunks);
//     // std::printf("Sum size %" PRIu64 "\n",    sum_size);
//     // std::printf("Unique chunks %" PRIu64 "\n",    sum_chunks - dedup_chunks);
//     std::printf("%.2f%\n",     double(dedup_size) / double(sum_size) *100);

//     // update backup job
//     bj.dedup_chunks += dedup_chunks;
//     bj.dedup_size += dedup_size;
//     bj.sum_chunks += sum_chunks;
//     bj.sum_size += sum_size;
//     bj.hash_collision_sum += hash_collision_sum;
//     bj.file_num++;

//     if(dd)
//         GlobalMetadataManagerPtr->save(current_version, delta_num, min_destination_base);

//     // free 
//     close(idf);
//     free(file_cache);
// }

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

        if(dt == DedupType::Naive){
            for (const auto& path : files){
                writeFileNaive(path);
            } 

        }else if(dt == DedupType::DedupInterval){
            int interval = Config::getInstance().getDeltaNum();
            for (const auto& path : files){
                //writeFileDedupInterval(path);
            } 

        }else if(dt == DedupType::DedupFirst){
            GlobalMetadataManagerPtr->reserveDedupFirstDeltaTable(files.size());
            for (const auto& path : files){
                writeFileDedupFirst(path);
            } 

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

        if(dt == DedupType::Naive){
            for (const auto& path : files){
                writeFileNaive(path);
            } 

        }else if(dt == DedupType::DedupInterval){
            int interval = Config::getInstance().getDeltaNum();
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

int main(int argc, char** argv){
    // 超级权限
    setuid(0);

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
        std::printf("Sum chunks num % " PRIu64 "\n",       bj.sum_chunks);
        std::printf("Sum data size %" PRIu64 "\n",         bj.sum_size);
        std::printf("Dedup chunks num %" PRIu64 "\n",      bj.dedup_chunks);
        std::printf("Dedup data size %" PRIu64 "\n",       bj.dedup_size);
        std::printf("Average chunk size %" PRIu64 "\n",    bj.sum_size / bj.sum_chunks);
        std::printf("-----------------------statics----------------------\n");
        std::printf("Backup Throughput %.2f MiB/s\n",    throughput);
        std::printf("Dedup Ratio %.2f%\n",     double(bj.dedup_size) / double(bj.sum_size) *100);

        // 保存全局信息
        GlobalStat::getInstance().update(bj.sum_size, bj.sum_size - bj.dedup_size);
        GlobalStat::getInstance().save_arguments(global_stat_path);
        
        // save metadata entry
        GlobalMetadataManagerPtr->save();
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