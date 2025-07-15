#include <vector>
#include <map>
#include <fstream>
#include <iostream>
#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <openssl/sha.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/sendfile.h>
#include <experimental/filesystem>
#include <algorithm>

#include "fastcdc.h"
#include "MetadataManager.h"
#include "ContainerCache.h"
#include "ChunkCache.h"
#include "general.h"
#include "FAA.h"
#include "config.h"
#include "utils/cJSON.h"
#include "compressor.h"
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
    
    uint32_t code_lines;
    uint32_t comment_lines;
    uint32_t blank_lines;
};

namespace fs = std::experimental::filesystem;

const char* global_stat_path = "/home/cyf/cDedup/global_stat.json";
extern MetadataManager *GlobalMetadataManagerPtr;
struct backup_job bj;
int (*chunking) (unsigned char*p, int n);
int cpp_file_num = 0;
int py_file_num = 0;
int fortran_file_num = 0;

// used for normal count
uint64_t global_code_line = 0;
uint64_t global_blank_line = 0;
uint64_t global_comment_line = 0;
uint64_t LOC_time = 0;
uint64_t chunking_time = 0;
struct timeval LOC_time_start, LOC_time_end;
struct timeval chunking_time_start, chunking_time_end;
uint64_t restore_size = 0;
std::unordered_set<SHA1FP, TupleHasher, TupleEqualer> restore_LOC_set;

uint32_t getFilesNum(const char* dirPath) {
    uint32_t ans = 0;
    DIR *dir = opendir(dirPath);
    
    if (!dir) {
        fprintf(stderr, "getFilesNum opendir error, id %d, %s, the dir is %s\n", 
                errno, strerror(errno), dirPath);
        exit(EXIT_FAILURE);
    }

    struct dirent* ptr;
    while (ptr = readdir(dir)) {
        // 跳过 "." 和 ".." 目录
        if (strcmp(ptr->d_name, ".") && strcmp(ptr->d_name, "..")) {
            ans++;
        }
    }

    if (closedir(dir) == -1) {
        fprintf(stderr, "getFilesNum closedir error, id %d, %s\n",
                errno, strerror(errno));
        // 这里不退出，因为已经读取完毕
    }

    return ans;
}

void saveFileRecipe(std::vector<std::string> file_recipe, const char* fileRecipesPath){
    int n_version = getFilesNum(fileRecipesPath);
    std::string recipe_name(fileRecipesPath);
    recipe_name.append("/recipe");
    recipe_name.append(std::to_string(n_version));
    int fd = open(recipe_name.data(), O_RDWR | O_CREAT, 0777);
    if(fd < 0){ 
        printf("saveFileRecipe open error, id %d, %s\n", errno, strerror(errno)); 
        exit(-1);
    }
    for(auto x : file_recipe){
        if(write(fd, x.data(), SHA_DIGEST_LENGTH) < 0)
            printf("save recipe write error\n");
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
        printf("Restore version %d not exist!!!\n", restore_version); 
        exit(-1);
    }

    std::string recipe_name = getRecipeNameFromVersion(restore_version, file_recipe_path);
    int fd = open(recipe_name.data(), O_RDONLY, 0777);
    if(fd < 0){
        printf("getFileRecipe open error, %s, %s\n", strerror(errno), recipe_name.data());
        exit(-1);
    }
    char fp_buf[SHA_DIGEST_LENGTH]={0};
    std::vector<std::string> ans;

    while(1){
        int n = read(fd, fp_buf, SHA_DIGEST_LENGTH);
        if(n == 0){
            break;
        }else if(n < 0){
            printf("getFileRecipe error, %s, %s\n", strerror(errno), recipe_name.data());
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
    int n = write(fd, container_buf, CONTAINER_SIZE);
    close(fd);
}

void saveChunkToContainer(unsigned int& container_buf_pointer, unsigned char* container_buf, 
                          uint32_t& container_index, uint32_t& container_inner_offset, uint16_t& container_inner_index,
                          int chunk_length, int file_offset, unsigned char* file_cache, void* SHA_buf,
                          const char* containers_path){
    // flush
    if(container_buf_pointer + chunk_length >= CONTAINER_SIZE){
        saveContainer(container_index, container_buf, container_buf_pointer, containers_path);
        memset(container_buf, 0, CONTAINER_SIZE);
        container_index++;
        container_inner_offset = 0;
        container_buf_pointer = 0;
        container_inner_index = 0;
    }

    // container buffer
    memcpy(container_buf + container_buf_pointer, file_cache + file_offset, chunk_length);
}

void flushAssemblingBuffer(int fd, unsigned char* buf, int len){
    if(write(fd, buf, len) != len){
        printf("Restore, write file error!!!\n");
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
            printf("Invalid NC level: %d\n", NC_level);
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
            printf("Invalid fixed size, the support size is 4 or 8 or 16\n");
            exit(-1);
        }
    }
}

/*
    代码行必须有回车才能算作一行，如果最后一行代码没回车，那么不被算作一行；
*/
void countLinesCPP(uint8_t *c, uint32_t length,
                uint64_t& code_lines, uint64_t& comment_lines, uint64_t& blank_lines) {
    enum { CODE, BLOCK_COMMENT, LINE_COMMENT } state = CODE;
    bool line_first_char = true;
    for (size_t i = 0; i < length; i++) {
        unsigned char currentChar = c[i];
        unsigned char nextChar = (i < length - 1) ? c[i + 1] : '\0';

        if (state == CODE) {
            if (currentChar == '/' && nextChar == '*') {
                state = BLOCK_COMMENT;
                line_first_char = false;
                i+=2;
            } else if (currentChar == '/' && nextChar == '/') {
                state = LINE_COMMENT;
                line_first_char = false;
                i+=2;
                comment_lines++;
            } else if (currentChar == '\n') {
                if(line_first_char)
                    blank_lines ++;
                else
                    code_lines++;
                line_first_char = true;
            }else{
                line_first_char = false;
            }
        } else if (state == BLOCK_COMMENT) {
            if (currentChar == '*' && nextChar == '/') {
                state = CODE;
                line_first_char = false;
                i+=2;
                comment_lines++;
            }else if (currentChar == '\n') {
                line_first_char = true;
                comment_lines++;
            }
        } else if (state == LINE_COMMENT) {
            if (currentChar == '\n') {
                line_first_char = true;
                state = CODE;
                comment_lines++;
            }
        }
    }
}

void countLinesPython(uint8_t *c, uint32_t length,
                uint64_t& code_lines, uint64_t& comment_lines, uint64_t& blank_lines) {
    int single_comment_delimiter_size_python = 1;
    int multi_comment_delimiter_size_python = 3;

    enum { CODE, BLOCK_COMMENT, LINE_COMMENT } state = CODE;
    bool line_first_char = true;

    for (size_t i = 0; i < length; i++) {
        unsigned char currentChar = c[i];
        unsigned char nextChar = (i < length - 1) ? c[i + 1] : '\0';
        unsigned char nextNextChar = (i < length - 2) ? c[i + 2] : '\0';

        if (state == CODE) {
            if (currentChar == '\'' && nextChar == '\'' && nextChar == '\'') {
                state = BLOCK_COMMENT;
                line_first_char = false;
                i+=multi_comment_delimiter_size_python;
            }else if (currentChar == '\"' && nextChar == '\"' && nextChar == '\"') {
                state = BLOCK_COMMENT;
                line_first_char = false;
                i+=multi_comment_delimiter_size_python;
            }else if (currentChar == '#') {
                state = LINE_COMMENT;
                line_first_char = false;
                i+=single_comment_delimiter_size_python;
                comment_lines++;
            } else if (currentChar == '\n') {
                if(line_first_char)
                    blank_lines ++;
                else
                    code_lines++;
                line_first_char = true;
            }else{
                line_first_char = false;
            }
        } else if (state == BLOCK_COMMENT) {
            if (currentChar == '\'' && nextChar == '\'' && nextChar == '\'') {
                state = CODE;
                line_first_char = false;
                i+=multi_comment_delimiter_size_python;
                comment_lines++;
            }else if (currentChar == '\"' && nextChar == '\"' && nextChar == '\"') {
                state = CODE;
                line_first_char = false;
                i+=multi_comment_delimiter_size_python;
                comment_lines++;
            }else if (currentChar == '\n') {
                line_first_char = true;
                comment_lines++;
            }
        } else if (state == LINE_COMMENT) {
            if (currentChar == '\n') {
                line_first_char = true;
                state = CODE;
                comment_lines++;
            }
        }
    }
}

// 无多行注释支持
// 单行注释一个单引号或者rem或者REM
void countLinesVisualBasic(uint8_t *c, uint32_t length,
                uint64_t& code_lines, uint64_t& comment_lines, uint64_t& blank_lines) {
    enum { CODE, BLOCK_COMMENT, LINE_COMMENT } state = CODE;
    bool line_first_char = true;
    for (size_t i = 0; i < length; i++) {
        unsigned char currentChar = c[i];
        unsigned char nextChar = (i < length - 1) ? c[i + 1] : '\0';
        unsigned char nextNextChar = (i < length - 2) ? c[i + 2] : '\0';

        if (state == CODE) {
            if (currentChar == '\'') {
                state = LINE_COMMENT;
                line_first_char = false;
                i+=1;
                comment_lines++;
            } else if (currentChar == 'r' && nextChar == 'e' && nextNextChar == 'm') {
                state = LINE_COMMENT;
                line_first_char = false;
                i+=3;
                comment_lines++;
            } else if (currentChar == 'R' && nextChar == 'E' && nextNextChar == 'M') {
                state = LINE_COMMENT;
                line_first_char = false;
                i+=3;
                comment_lines++;
            } else if (currentChar == '\n') {
                if(line_first_char)
                    blank_lines ++;
                else
                    code_lines++;
                line_first_char = true;
            }else{
                line_first_char = false;
            }
        } else if (state == LINE_COMMENT) {
            if (currentChar == '\n') {
                line_first_char = true;
                state = CODE;
                comment_lines++;
            }
        }
    }
}


// 单行注释两个斜杠
// 多行注释{}或者(**)
void countLinesDelphi(uint8_t *c, uint32_t length,
                uint64_t& code_lines, uint64_t& comment_lines, uint64_t& blank_lines) {
    enum { CODE, BLOCK_COMMENT, LINE_COMMENT } state = CODE;
    bool line_first_char = true;
    for (size_t i = 0; i < length; i++) {
        unsigned char currentChar = c[i];
        unsigned char nextChar = (i < length - 1) ? c[i + 1] : '\0';

        if (state == CODE) {
            if (currentChar == '/' && nextChar == '/') {
                state = LINE_COMMENT;
                line_first_char = false;
                i+=2;
                comment_lines++;
            } else if (currentChar == '(' && nextChar == '*') {
                state = BLOCK_COMMENT;
                line_first_char = false;
                i+=2;
            } else if (currentChar == '{') {
                state = BLOCK_COMMENT;
                line_first_char = false;
                i+=1;
            } else if (currentChar == '\n') {
                if(line_first_char)
                    blank_lines ++;
                else
                    code_lines++;
                line_first_char = true;
            }else{
                line_first_char = false;
            }
        } else if (state == BLOCK_COMMENT) {
            if (currentChar == '*' && nextChar == ')') {
                state = CODE;
                line_first_char = false;
                i+=2;
                comment_lines++;
            }else if (currentChar == '}') {
                state = CODE;
                line_first_char = false;
                i+=1;
                comment_lines++;
            } else if (currentChar == '\n') {
                line_first_char = true;
                comment_lines++;
            }
        } else if (state == LINE_COMMENT) {
            if (currentChar == '\n') {
                line_first_char = true;
                state = CODE;
                comment_lines++;
            }
        }
    }
}

// no multi line comments support
void countLinesFortran(uint8_t *c, uint32_t length,
                uint64_t& code_lines, uint64_t& comment_lines, uint64_t& blank_lines) {
    int single_comment_delimiter_size_python = 1;

    enum { CODE, BLOCK_COMMENT, LINE_COMMENT } state = CODE;
    bool line_first_char = true;

    for (size_t i = 0; i < length; i++) {
        unsigned char currentChar = c[i];

        if (state == CODE) {
            if (currentChar == '#') {
                state = LINE_COMMENT;
                line_first_char = false;
                i+=single_comment_delimiter_size_python;
                comment_lines++;
            } else if (currentChar == '\n') {
                if(line_first_char)
                    blank_lines ++;
                else
                    code_lines++;
                line_first_char = true;
            }else{
                line_first_char = false;
            }
        } else if (state == LINE_COMMENT) {
            if (currentChar == '\n') {
                line_first_char = true;
                state = CODE;
                comment_lines++;
            }
        }
    }
}

void (*countLines)(uint8_t *c, uint32_t length,uint64_t& code_lines, uint64_t& comment_lines, uint64_t& blank_lines);

void initCountLines(enum LANG lang){
    if(lang == LANG_PYTHON){
        countLines = countLinesPython;
    }else if(lang == LANG_CPP){
        countLines = countLinesCPP;
    }else if(lang == LANG_C){
        countLines = countLinesCPP;
    }else if(lang == LANG_JAVA){
        countLines = countLinesCPP;
    }else if(lang == LANG_CSHARP){
        countLines = countLinesCPP;
    }else if(lang == LANG_JAVASCRIPT){
        countLines = countLinesCPP;
    }else if(lang == LANG_GO){
        countLines = countLinesCPP;
    }else if(lang == LANG_VB){
        countLines = countLinesVisualBasic;
    }else if(lang == LANG_DELPHI){
        countLines = countLinesDelphi;
    }else if(lang == LANG_FORTRAN){
        countLines = countLinesFortran;
    }else{
        exit(-1);
        printf("Not support language");
    }
}

void writeFile(string path){
    int idf = open(path.c_str(), O_RDONLY | O_DIRECT, 0777);
    if(idf < 0){
        printf("open file error, id %d, %s\n", errno, strerror(errno));
        exit(-1);
    }

    unsigned char* file_cache = nullptr;
    int result1 = posix_memalign((void**)&file_cache, 512, FILE_CACHE);
    if (result1 != 0 || file_cache == nullptr) {
        fprintf(stderr, "Failed to allocate aligned memory: %s\n", strerror(result1));
        exit(EXIT_FAILURE);  // 或者抛出异常 throw std::bad_alloc();
    }

    struct SHA1FP sha1_fp;
    std::vector<std::string> file_recipe; // 保存这个文件所有块的指纹

    // metadata entry(except FP)
    uint32_t container_index = getFilesNum(Config::getInstance().getContainersPath().c_str());
    uint32_t container_inner_offset = 0;
    uint32_t chunk_length = 0;
    uint16_t container_inner_index = 0;

    unsigned char* container_buf = nullptr;
    int result2 = posix_memalign((void**)&container_buf, 512, CONTAINER_SIZE);
    if (result2 != 0 || container_buf == nullptr) {
        // 内存分配失败处理
        fprintf(stderr, "Memory allocation failed: %s\n", strerror(result2));
        exit(EXIT_FAILURE); // 或抛出异常 throw std::bad_alloc();
    }

    unsigned int container_buf_pointer = 0;
    uint32_t file_offset = 0;
    uint32_t n_read = 0;

    struct ENTRY_VALUE entry_value;

    // 重删统计
    uint64_t dedup_chunks = 0;
    uint64_t dedup_size = 0;
    uint64_t sum_chunks = 0;
    uint64_t sum_size = 0;
    uint64_t hash_collision_sum = 0;
    uint64_t file_code_lines = 0;
    uint64_t file_comment_lines = 0;
    uint64_t file_blank_lines = 0;

    uint64_t chunk_code_lines = 0;
    uint64_t chunk_comment_lines = 0;
    uint64_t chunk_blank_lines = 0;

    int scan_scope = Config::getInstance().getBackwardScanScope();
    enum LANG lang = Config::getInstance().getLanugage();
    enum ClocMethod cloc_method = Config::getInstance().getClocMethod();
    
    // 普通分块重删，来一个块查寻一次，然后把non-duplicate chunk保存到container去
    for(;;){
        file_offset = 0;

        n_read = read(idf, file_cache, FILE_CACHE);

        if(n_read <= 0){
            break;
        }

        while(file_offset < n_read){  
            gettimeofday(&chunking_time_start, NULL);
            if(cloc_method == NON_CLOC ||cloc_method == DC_NON_ALIGN || cloc_method == NAIVE_CLOC){
                chunk_length = chunking(file_cache + file_offset, n_read - file_offset);
            }else if(cloc_method == DC_NEWLINE){
                chunk_length = chunking(file_cache + file_offset, n_read - file_offset);
                chunk_length += align_chunk_to_enterSymbol(file_cache + file_offset, n_read - file_offset, chunk_length);
            }else if(cloc_method == DC_NEW_MULTI){
                chunk_length = chunking(file_cache + file_offset, n_read - file_offset);
                chunk_length += align_chunk_to_enterSymbol(file_cache + file_offset, n_read - file_offset, chunk_length);
                chunk_length += align_chunk_to_multilineEndDelimiter(file_cache + file_offset, n_read - file_offset, chunk_length, 
                                                                 scan_scope, lang);
            }
            gettimeofday(&chunking_time_end, NULL);
                chunking_time += (chunking_time_end.tv_sec - chunking_time_start.tv_sec) * 1000000 + 
                                            chunking_time_end.tv_usec - chunking_time_start.tv_usec;
            

            // Hash
            memset(&sha1_fp, 0, sizeof(struct SHA1FP));
            SHA1(file_cache + file_offset, chunk_length, (uint8_t*)&sha1_fp);

            // Dedup
            LookupResult lookup_result;
            lookup_result = GlobalMetadataManagerPtr->dedupLookup(sha1_fp);

            if(cloc_method == NAIVE_CLOC){
                gettimeofday(&LOC_time_start, NULL);
                countLines(file_cache + file_offset, chunk_length, chunk_code_lines, chunk_comment_lines, chunk_blank_lines);
                gettimeofday(&LOC_time_end, NULL);
                LOC_time += (LOC_time_end.tv_sec - LOC_time_start.tv_sec) * 1000000 + 
                                            LOC_time_end.tv_usec - LOC_time_start.tv_usec;
            }

            if(lookup_result == Unique){
                // save chunk itself
                saveChunkToContainer(container_buf_pointer, container_buf, 
                                    container_index, container_inner_offset, container_inner_index,
                                    chunk_length, file_offset, file_cache, (void*)&sha1_fp,
                                    Config::getInstance().getContainersPath().c_str());
                
                // 唯一块需要扫描cloc
                if(cloc_method != NAIVE_CLOC&&cloc_method!=NON_CLOC){
                    gettimeofday(&LOC_time_start, NULL);
                    countLines(file_cache + file_offset, chunk_length, chunk_code_lines, chunk_comment_lines, chunk_blank_lines);
                    gettimeofday(&LOC_time_end, NULL);
                    LOC_time += (LOC_time_end.tv_sec - LOC_time_start.tv_sec) * 1000000 + 
                                                LOC_time_end.tv_usec - LOC_time_start.tv_usec;
                }
                
                // save chunk metadata
                entry_value.container_number = container_index;
                entry_value.offset = container_inner_offset;
                entry_value.chunk_length = chunk_length;
                entry_value.container_inner_index = container_inner_index;

                entry_value.code_lines = chunk_code_lines;
                entry_value.comment_lines = chunk_comment_lines;
                entry_value.blank_lines = chunk_blank_lines;
                GlobalMetadataManagerPtr->addNewEntry(sha1_fp, entry_value);

                container_inner_offset += chunk_length;
                container_buf_pointer += chunk_length;
                container_inner_index ++;

                file_code_lines += chunk_code_lines;
                file_comment_lines += chunk_comment_lines;
                file_blank_lines += chunk_blank_lines;

            }else if(lookup_result == Dedup){
                // 重删统计
               dedup_chunks ++;
               dedup_size += chunk_length;

               // 重复块直接查表cloc
               ENTRY_VALUE tmp_ev = GlobalMetadataManagerPtr->getAddedEntry(sha1_fp);
               file_code_lines += tmp_ev.code_lines;
               file_comment_lines += tmp_ev.comment_lines;
               file_blank_lines += tmp_ev.blank_lines;
            }

            // Insert fingerprint into file recipe
            file_recipe.push_back(std::string((char*)&sha1_fp, sizeof(struct SHA1FP)));

            // Statistic
            sum_chunks ++;
            sum_size += chunk_length;

            file_offset += chunk_length;

            chunk_code_lines = 0;
            chunk_comment_lines = 0;
            chunk_blank_lines = 0;
        }
    }

    //  flush最后一个container
    if(container_buf_pointer > 0)
        saveContainer(container_index, container_buf, 
                        container_buf_pointer, Config::getInstance().getContainersPath().c_str());
    
    // flush file_recipe
    saveFileRecipe(file_recipe, Config::getInstance().getFileRecipesPath().c_str());

    // update backup job
    bj.dedup_chunks += dedup_chunks;
    bj.dedup_size += dedup_size;
    bj.sum_chunks += sum_chunks;
    bj.sum_size += sum_size;
    bj.hash_collision_sum += hash_collision_sum;
    bj.file_num++;

    bj.code_lines += file_code_lines;
    bj.comment_lines += file_comment_lines;
    bj.blank_lines += file_blank_lines;

    // free 
    close(idf);
    free(file_cache);
}

std::string getExtension(const std::string& filename) {
    size_t pos = filename.find_last_of(".");
    if (pos != std::string::npos && pos < filename.size() - 1) {
        return filename.substr(pos + 1);
    }
    return ""; // 如果没有找到点或者点后面没有字符，则返回空字符串
}

std::vector<fs::path> traverseDirectory(const fs::path& directory) {
    std::vector<fs::path> files;

    try {
        // 遍历目录
        for (const auto& entry : fs::directory_iterator(directory)) {
            try {
                if (fs::is_regular_file(entry)) {
                    files.push_back(entry.path());
                } else if (fs::is_directory(entry)) {
                    auto sub_files = traverseDirectory(entry.path());
                    files.insert(files.end(), sub_files.begin(), sub_files.end());
                }
            } catch (const std::exception& ex) {
                std::cerr << "Error processing " << entry.path() << ": " << ex.what() << std::endl;
                continue; // 继续处理其他文件
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "Directory traversal error: " << ex.what() << std::endl;
    }

    return files; // 确保所有路径都返回
}

void traverseWriteDirectory(const fs::path& directory) {
    try {
        // 遍历目录
        std::vector<fs::path> files = traverseDirectory(directory);
        printf("Found %zu files in %s\n", files.size(), directory.string().c_str());

        // 对文件名进行排序
        std::sort(files.begin(), files.end());

        for (const auto& path : files){
            //cout << path <<endl;
            writeFile(path);
        }
            
        
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << std::endl;
    }
}

void countFile(string file_name){
    int fd = open(file_name.c_str(), O_RDONLY, 0777);
    unsigned char* file_cache = (unsigned char*)malloc(FILE_CACHE);

    int n_read = read(fd, file_cache, FILE_CACHE);
    countLines(file_cache, n_read, global_code_line, global_comment_line, global_blank_line);
}


void traverseCountDirectory(const fs::path& directory) {
    try {
        // 遍历目录
        std::vector<fs::path> files = traverseDirectory(directory);
        printf("Found %zu files in %s\n", files.size(), directory.string().c_str());

        // 对文件名进行排序
        std::sort(files.begin(), files.end());

        for (const auto& path : files){
            countFile(path);
        }
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << std::endl;
    }
}

void restoreVersion(int version){
    if(!fileRecipeExist(version, Config::getInstance().getFileRecipesPath().c_str())){
        printf("Version %d not exist!\n", version);
        return ;
    }
    
    unsigned char* assembling_buffer = (unsigned char*)malloc(FILE_CACHE);
    memset(assembling_buffer, 0, FILE_CACHE);
    int write_buffer_offset = 0;

    //recipe
    std::vector<std::string> file_recipe = getFileRecipe(version,
                                                            Config::getInstance().getFileRecipesPath().c_str());

    //组装
    if(Config::getInstance().getRestoreMethod() == CONTAINER_CACHE ||
        Config::getInstance().getRestoreMethod() == CHUNK_CACHE){
        int fd = open((Config::getInstance().getRestorePath() + to_string(version)).c_str(), O_RDWR | O_CREAT, 0777);
        if(fd < 0){
            printf("无法写文件!!! %s\n", strerror(errno));
            exit(-1);
        }

        Cache* cc;
        if(Config::getInstance().getRestoreMethod() == CONTAINER_CACHE){
            cc = new ContainerCache(Config::getInstance().getContainersPath().c_str(), 32);
        }else if(Config::getInstance().getRestoreMethod() == CHUNK_CACHE){
            cc = new ChunkCache(Config::getInstance().getContainersPath().c_str(), 16*1024);
        }

        // code line
        uint64_t chunk_code_lines = 0;
        uint64_t chunk_comment_lines = 0;
        uint64_t chunk_blank_lines = 0;

        enum ClocMethod cloc_method = Config::getInstance().getClocMethod();
        
        for(auto &x : file_recipe){
            SHA1FP fp;
            memcpy(&fp, x.data(), sizeof(SHA1FP));
            LookupResult res = GlobalMetadataManagerPtr->dedupLookup(fp);

            if(res == Unique){
                printf("Fatal error!!!\n");
                exit(-1);
            }

            ENTRY_VALUE ev = GlobalMetadataManagerPtr->getEntry(fp);
            std::string ck_data = cc->getChunkData(ev);

            if(cloc_method == NAIVE_CLOC){
                gettimeofday(&LOC_time_start, NULL);
                countLines((uint8_t*)ck_data.data(), ck_data.size(), 
                chunk_code_lines, chunk_comment_lines, chunk_blank_lines);
                gettimeofday(&LOC_time_end, NULL);
                LOC_time += (LOC_time_end.tv_sec - LOC_time_start.tv_sec) * 1000000 + 
                                            LOC_time_end.tv_usec - LOC_time_start.tv_usec;
            }else if(cloc_method == DC_OFFLINE){
                if(restore_LOC_set.find(fp) == restore_LOC_set.end()){
                    gettimeofday(&LOC_time_start, NULL);
                    countLines((uint8_t*)ck_data.data(), ck_data.size(), 
                    chunk_code_lines, chunk_comment_lines, chunk_blank_lines);
                    gettimeofday(&LOC_time_end, NULL);
                    LOC_time += (LOC_time_end.tv_sec - LOC_time_start.tv_sec) * 1000000 + 
                                            LOC_time_end.tv_usec - LOC_time_start.tv_usec;
                    restore_LOC_set.insert(fp);
                }
            }else if(cloc_method == NON_CLOC){
                ;
            }else{
                printf("不正确的恢复cloc模式");
                exit(-1);
            }
        
            if(ck_data.size() != ev.chunk_length){
                printf("Fatal error size different!!!\n");
                exit(-1);
            }

            if(write_buffer_offset + ck_data.size() >= FILE_CACHE){
                flushAssemblingBuffer(fd, assembling_buffer, write_buffer_offset);
                write_buffer_offset = 0;
            }

            memcpy(assembling_buffer + write_buffer_offset, 
                ck_data.data(), ck_data.size());
            write_buffer_offset += ev.chunk_length;

            restore_size += ev.chunk_length;
        }

        flushAssemblingBuffer(fd, assembling_buffer, write_buffer_offset);
        close(fd);
    }else{
        printf("暂不支持的恢复算法 %d\n", Config::getInstance().getRestoreMethod());
        exit(-1);
    }

    free(assembling_buffer);
}

int main(int argc, char** argv){
    // // 超级权限
    // if (setuid(0) == -1) {
    //     perror("Failed to setuid(0)");
    //     exit(EXIT_FAILURE);
    // }

    // 参数解析
    Config::getInstance().parse_argument(argc, argv);

    // 全局统计信息解析
    GlobalStat::getInstance().parse_arguments(global_stat_path);
    
    GlobalMetadataManagerPtr = new MetadataManager(Config::getInstance().getFingerprintsFilePath().c_str());
    
    enum LANG language_type = Config::getInstance().getLanugage();    
    initCountLines(language_type);

    if(Config::getInstance().getTaskType() == TASK_LOC){
        string input_path = Config::getInstance().getInputPath();

        if (!fs::exists(input_path)) {
            std::cerr << "Error: Input path does not exist." << std::endl;
            return 1;
        }

        if (!fs::is_directory(input_path)) {
            countFile(input_path);
        }else{
            traverseCountDirectory(input_path);
        }

        printf("-----------------------Code line statics----------------------\n");
        printf("Code lines %" PRIu64 "\n",            global_code_line);
        printf("Comment lines %" PRIu64 "\n",         global_comment_line);
        printf("Blank lines %" PRIu64 "\n",           global_blank_line);

    }else if(Config::getInstance().getTaskType() == TASK_WRITE){
        // Inline counting
        
        int current_version = getFilesNum(Config::getInstance().getFileRecipesPath().c_str());
        if(current_version != 0){
            GlobalMetadataManagerPtr->load();
        }

        initChunkingAlgorithm();

        string input_path = Config::getInstance().getInputPath();

        if (!fs::exists(input_path)) {
            std::cerr << "Error: Input path does not exist." << std::endl;
            return 1;
        }

        struct timeval backup_time_start, backup_time_end;
        gettimeofday(&backup_time_start, NULL);

        if (!fs::is_directory(input_path)) {
            writeFile(input_path);
        }else{
            traverseWriteDirectory(input_path);
        }

        gettimeofday(&backup_time_end, NULL);

        // throughput
        uint64_t single_dedup_time_us = (backup_time_end.tv_sec - backup_time_start.tv_sec) * 1000000 + 
                                         backup_time_end.tv_usec - backup_time_start.tv_usec;
        float throughput = (float)(bj.sum_size) / MB / ((float)(single_dedup_time_us)/1000000);

        float LOC_time_percetage = float(LOC_time) / (float)single_dedup_time_us * 100;
        float chunking_speed = (float)(bj.sum_size) / MB / ((float)(chunking_time)/1000000);

        // 写文件 - 重删统计
        printf("-----------------------Dedup statics----------------------\n");
        printf("Hash collision num %" PRIu64 "\n",    bj.hash_collision_sum); // should be zero
        printf("Sum chunks num %" PRIu64 "\n",       bj.sum_chunks);
        printf("Sum data size %" PRIu64 "\n",         bj.sum_size);
        printf("Average chunk size %" PRIu64 "\n",    bj.sum_size / bj.sum_chunks);
        printf("Dedup chunks num %" PRIu64 "\n",      bj.dedup_chunks);
        printf("Dedup data size %" PRIu64 "\n",       bj.dedup_size);
        printf("Chunking Speed %.2f MiB/s\n",         chunking_speed);
        printf("-----------------------Code line statics----------------------\n");
        printf("Code lines %" PRIu32 "\n", bj.code_lines);
        printf("Comment lines %" PRIu32 "\n", bj.comment_lines);
        printf("Blank lines %" PRIu32 "\n", bj.blank_lines);
        printf("-----------------------statics----------------------\n");
        printf("LOC time percentage %.2f%%\n", LOC_time_percetage);
        printf("Throughput %.2f MiB/s\n",    throughput);
        printf("Dedup Ratio %.2f%%\n",     double(bj.dedup_size) / double(bj.sum_size) *100);

        // 保存全局信息
        GlobalStat::getInstance().update(bj.sum_size, bj.sum_size - bj.dedup_size);
        GlobalStat::getInstance().save_arguments(global_stat_path);

        // save metadata entry
        GlobalMetadataManagerPtr->save();

    }else if(Config::getInstance().getTaskType() == TASK_RESTORE){
        // Offline counting

        int current_version = getFilesNum(Config::getInstance().getFileRecipesPath().c_str());
        if(current_version != 0){
            GlobalMetadataManagerPtr->load();
        }

        struct timeval restore_time_start, restore_time_end;
        gettimeofday(&restore_time_start, NULL);
        for(int i=0; i<=9; i++)
            restoreVersion(i);
        gettimeofday(&restore_time_end, NULL);

        uint64_t single_dedup_time_us = (restore_time_end.tv_sec - restore_time_start.tv_sec) * 1000000 + restore_time_end.tv_usec - restore_time_start.tv_usec;
        float restore_throughput = (float)(restore_size) / MB / ((float)(single_dedup_time_us)/1000000);
        float LOC_time_percetage = float(LOC_time) / (float)single_dedup_time_us * 100; 

        printf("-----------------------Restore statics----------------------\n");
        printf("LOC time percentage %.2f%%\n", LOC_time_percetage);
        printf("Restore size %ld\n", restore_size);
        printf("Restore Throughput %.2f MiB/s\n", restore_throughput);

    }
    return 0;
}