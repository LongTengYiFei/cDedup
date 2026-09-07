#include "MetadataManager.h"
#include "config.h"
#include "assert.h" 
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <iostream>
#include <fstream>
#include <unordered_set>

MetadataManager *GlobalMetadataManagerPtr;


// int MetadataManager::save(int current_version, int delta_size, int base_pos){
//     //printf("-----------------------Saving One File FP-index-----------------------\n");
//     std::string fp_name(Config::getInstance().getFpDeltaDedupFolderPath());
//     fp_name.append("/fp_");
//     fp_name.append(std::to_string(current_version));
//     if(current_version == base_pos)
//         fp_name.append("_base");
//     else
//         fp_name.append("_delta");

//     int fd = open(fp_name.c_str(), O_RDWR | O_CREAT, 0777);
//     if(fd < 0){
//         perror("Saving fp index error, the reason is ");
//         exit(-1);
//     }

//     int count = 0;
//     if(current_version == base_pos){
//         for(auto item : this->fp_table_base){
//             write(fd, (uint8_t*)&item.first, sizeof(SHA1FP));
//             write(fd, (uint8_t*)&item.second, sizeof(ENTRY_VALUE));
//             count++;
//         }
//     }else if(current_version <= (base_pos + delta_size)){
//         for(auto item : this->fp_table_delta){
//             int n = 0;
//             write(fd, (uint8_t*)&item.first, sizeof(SHA1FP));
//             write(fd, (uint8_t*)&item.second, sizeof(ENTRY_VALUE));
//             count++;
//         }
//     }else{
//         printf("Saving fp error\n");
//         exit(-1);
//     }

//     if(current_version == (base_pos + delta_size)){
//         fp_table_base.clear(); 
//     }

//     fp_table_delta.clear();

//     //printf("total item %d\n", count);
//     close(fd);
//     return 0;
// }

string MetadataManager::genFPname(int version, bool base){
    std::string fp_name(Config::getInstance().getFpDeltaDedupFolderPath());
    fp_name.append("/fp_");
    fp_name.append(std::to_string(version));
    if(base)
        fp_name.append("_base");
    else
        fp_name.append("_delta");
    return fp_name;
}

int MetadataManager::load(int restore_version){
    // 如果这个版本是base，那么只需加载base的fp
    // 如果这个版本是delta，那么需要加载它前面一个base的fp和它自己的fp
    int delta_num = Config::getInstance().getInterval();

    // 因为现在默认base size是1，所以是delta_num + 1
    if(restore_version % (delta_num + 1) == 0){
        // 仅仅需要加载base fp
        loadDeltaDedupFp(genFPname(restore_version, true));
    }else{
        // 需要加载base 和 delta的fp
        int base_pos = restore_version - (restore_version % (delta_num + 1));
        loadDeltaDedupFp(genFPname(base_pos, true));
        loadDeltaDedupFp(genFPname(restore_version, false));
    }

    return 0;
}

void MetadataManager::loadDeltaDedupFp(std::string fp_name){
    printf("-----------------------Loading FP-index DeltaDedup-----------------------\n");
    printf("Loading index: %s\n", fp_name.c_str());

    unsigned char* metadata_cache = (unsigned char*)malloc(BLOCK_SIZE);
    int fd = open(fp_name.c_str(), O_RDONLY);
    if(fd < 0)
        printf("MetadataManager::load error\n");
    
    // 这里读一次，已经默认了fp的总大小不回超过FILE_CACHE
    int n = read(fd, metadata_cache, BLOCK_SIZE);
    int meta_size = sizeof(SHA1FP) + sizeof(ENTRY_VALUE);
    int entry_count = n/meta_size;
    SHA1FP tmp_fp;
    ENTRY_VALUE tmp_value;

    for(int i=0; i<=entry_count-1; i++){
        memcpy(&tmp_fp, metadata_cache+i*meta_size, sizeof(SHA1FP));
        memcpy(&tmp_value, metadata_cache+i*meta_size + sizeof(SHA1FP), sizeof(ENTRY_VALUE));

        this->fp_table_origin.emplace(tmp_fp, tmp_value);
    }

    close(fd);
    free(metadata_cache);
    printf("metadata table load %d items\n", entry_count);
}

int MetadataManager::load(){
    printf("-----------------------Loading FP-index-----------------------\n");
    printf("Loading index..\n");

    unsigned char* metadata_cache = (unsigned char*)malloc(BLOCK_SIZE);
    int fd = open(this->metadata_file_path.c_str(), O_RDONLY);
    if(fd < 0)
        printf("MetadataManager::load error\n");
    int n = read(fd, metadata_cache, BLOCK_SIZE);
    int meta_size = sizeof(SHA1FP) + sizeof(ENTRY_VALUE);
    int entry_count = n/meta_size;
    SHA1FP tmp_fp;
    ENTRY_VALUE tmp_value;

    for(int i=0; i<=entry_count-1; i++){
        memcpy(&tmp_fp, metadata_cache+i*meta_size, sizeof(SHA1FP));
        memcpy(&tmp_value, metadata_cache+i*meta_size + sizeof(SHA1FP), sizeof(ENTRY_VALUE));

        this->fp_table_origin.emplace(tmp_fp, tmp_value);
    }

    close(fd);
    free(metadata_cache);
    printf("metadata table load %d items\n", entry_count);

    return 0;
}

int MetadataManager::save(){
    printf("-----------------------Saving FP-index-----------------------\n");
    int fd = open(this->metadata_file_path.c_str(), O_WRONLY | O_CREAT, 0777);
    if(fd < 0){
        perror("Saving fp index error, the reason is ");
        exit(-1);
    }
    lseek(fd, 0, SEEK_SET);

    int ret = ftruncate(fd,0);
    if(ret < 0){
        perror("ftruncate error, the reason is ");
        exit(-1);
    }

    int count = 0;
    
    for(auto item : this->fp_table_added){
        int n = write(fd, (uint8_t*)&item.first, sizeof(SHA1FP));
        if(n < 0){
            perror("Saving fp index error, the reason is ");
            exit(-1);
        }

        n = write(fd, (uint8_t*)&item.second, sizeof(ENTRY_VALUE));
        if(n < 0){
            perror("Saving fp index error, the reason is ");
            exit(-1);
        }
        count++;
    }
    printf("New added item %d\n", count);

    for(auto item : this->fp_table_origin){
        int n = write(fd, (uint8_t*)&item.first, sizeof(SHA1FP));
        if(n < 0){
            perror("Saving fp index error, the reason is ");
            exit(-1);
        }

        n = write(fd, (uint8_t*)&item.second, sizeof(ENTRY_VALUE));
        if(n < 0){
            perror("Saving fp index error, the reason is ");
            exit(-1);
        }
        count++;
    }
    printf("total item %d\n", count);

    close(fd);

    return 0;
}


LookupResult MetadataManager::dedupLookup(SHA1FP sha1){
    auto dedupIter = this->fp_table_origin.find(sha1);
    if(dedupIter != this->fp_table_origin.end()){
        return LookupResult{true, dedupIter->second.container_number};
    }

    dedupIter = this->fp_table_added.find(sha1);
    if(dedupIter != this->fp_table_added.end()){
        return LookupResult{true, dedupIter->second.container_number};
    }

    return LookupResult{false, 0};
}

LookupResult MetadataManager::dedupLookup(SHA1FP sha1, int base, int delta){
    auto dedupIter = this->fp_tables_interval[base].find(sha1);
    if(dedupIter != this->fp_tables_interval[base].end())
        return LookupResult{true, dedupIter->second.container_number};

    dedupIter = this->fp_tables_interval[delta].find(sha1);
    if(dedupIter != this->fp_tables_interval[delta].end())
        return LookupResult{true, dedupIter->second.container_number};
    
    return LookupResult{false, 0};
}

LookupResult MetadataManager::dedupLookup(SHA1FP sha1, int base, int delta, int group_index){
    auto dedupIter = this->fp_tables_multi_group[group_index][base].find(sha1);
    if(dedupIter != this->fp_tables_multi_group[group_index][base].end())
        return LookupResult{true, dedupIter->second.container_number};

    dedupIter = this->fp_tables_multi_group[group_index][delta].find(sha1);
    if(dedupIter != this->fp_tables_multi_group[group_index][delta].end())
        return LookupResult{true, dedupIter->second.container_number};
    
    return LookupResult{false, 0};
}

LookupResult MetadataManager::dedupLookupES(SHA1FP sha1, int version){
    if(version == 0){
        auto dedupIter = this->ES_base_table.find(sha1);
        if(dedupIter != this->ES_base_table.end())
            // 这里的container number没用；
            return LookupResult{true, 0};
    }else{
        auto dedupIter = this->ES_base_table.find(sha1);
        if(dedupIter != this->ES_base_table.end())
            return LookupResult{true, 0};

        dedupIter = this->ES_delta_table.find(sha1);
        if(dedupIter != this->ES_delta_table.end())
            return LookupResult{true, 0};
    }

    return LookupResult{false, 0};
}

void MetadataManager::ESClear(){
    ES_delta_table.clear();
}

void MetadataManager::dedupLookupADRE(const SHA1FP& chunk_fp, uint32_t chunk_len){
    // dedup against all current base FP table
    for(auto &x: this->current_base_FP_tables){
        BaseId bid = x.first;
        BaseFPTable& btable = x.second;
        if(btable.table.find(chunk_fp) == btable.table.end()){ // unique
            sample_results[bid].sample_size += chunk_len;
        }else{ // dup
            sample_results[bid].sample_size += chunk_len;
            sample_results[bid].sample_dup_size_against_base += chunk_len;
        }
    }

    // self dup
    if(sample_self_table.find(chunk_fp) != sample_self_table.end()){ // self dup
        sample_self_dup += chunk_len;
    }else{
        sample_self_table.insert({chunk_fp, ENTRY_VALUE()});
    }

    // save
    sample_chunk_fps.push(chunk_fp);
    sample_chunk_lens.push(chunk_len);
}

LookupResult MetadataManager::dedupLookupDSFI(const SHA1FP& chunk_fp){
    if(isCurrentBase){
        // 该版本是 base version；
        auto dedupIter = this->current_base_FP_tables[selected_base_version].table.find(chunk_fp);
        if(dedupIter != this->current_base_FP_tables[selected_base_version].table.end())
            return LookupResult{true, dedupIter->second.container_number};

        return LookupResult{false, 0};

    }else{
        // 先查 base table
        auto dedupIter = this->current_base_FP_tables[selected_base_version].table.find(chunk_fp);
        if(dedupIter != this->current_base_FP_tables[selected_base_version].table.end())
            return LookupResult{true, dedupIter->second.container_number};

        // 再查唯一 delta table
        dedupIter = this->delta_table.find(chunk_fp);
        if(dedupIter != this->delta_table.end())
            return LookupResult{true, dedupIter->second.container_number};
        
        return LookupResult{false, 0};
    }
}

SHA1FP MetadataManager::popSampleChunkFP(){
    SHA1FP ans = sample_chunk_fps.front();
    sample_chunk_fps.pop();
    return ans;
}

uint32_t MetadataManager::popSampleChunkLen(){
    uint32_t ans = sample_chunk_lens.front();
    sample_chunk_lens.pop();
    return ans;
}

void MetadataManager::ADREFinal(int current_version_id, uint64_t file_size, float & estimated_thDR_N){
    // first case
    if(current_version_id == 0){
        // 首个版本无需检测 sample
        selected_base_version = current_version_id;
        selected_source = source_incremental++;
        isCurrentBase = true;

        BaseFPTable new_table;
        this->current_base_FP_tables[current_version_id] = new_table;
        this->current_base_FP_tables[current_version_id].source_id = selected_source;
        return ;
    }

    // thDR of sample against each base FP table
    selected_base_version = -1;
    base_table_found = false;
    estimated_thDR_N = 0;
    float estimated_ADR_N = 0;
    float ADR_N_sub_1 = 0;
    for(auto &x: sample_results){
        BaseId bid = x.first;
        x.second.SDR = ((float)x.second.sample_dup_size_against_base + (float)sample_self_dup) / (float)x.second.sample_size;
        
        if(x.second.SDR >= (ldr_ratio * current_base_FP_tables[bid].thDRs.back()) && x.second.SDR >= 0.1){   
            // control
            base_table_found = true;
            selected_base_version = bid; // set base, may be change later;
            selected_source = current_base_FP_tables[selected_base_version].source_id; // will not change;

            // compute
            estimated_thDR_N = x.second.SDR;
            uint64_t source_sum_size = source_infos[selected_source].sum_size;
            uint64_t source_dup_size = source_infos[selected_source].dup_size;
            estimated_ADR_N = (float)(file_size * estimated_thDR_N + source_dup_size) / (float)(source_sum_size + file_size);
            
            // get
            ADR_N_sub_1 = source_infos[selected_source].ADRs.back();
        }
    }

    // case judge
    if(base_table_found){
        if(estimated_ADR_N < ADR_N_sub_1){
            // Case1:  base table found ADR N < ADR N-1
            BaseFPTable new_table;
            this->current_base_FP_tables[current_version_id] = new_table;
            this->current_base_FP_tables[current_version_id].source_id = selected_source;
            isCurrentBase = true;

            /*
                change base to new base
                base 会改，但是 source 不会改；
            */
            selected_base_version = current_version_id; 

        }else{
            // Case2:  base table found ADR N >= ADR N-1
            delta_table.clear();
            isCurrentBase = false;
        }

    }else{
        // Case3: base table not found
        selected_base_version = current_version_id; // 没found，所以就设置当前version为base；
        selected_source = source_incremental++; // 没found，列为新source；

        BaseFPTable new_table;
        this->current_base_FP_tables[current_version_id] = new_table;
        this->current_base_FP_tables[current_version_id].source_id = selected_source;
        isCurrentBase = true;
    }
    return ;
}

void MetadataManager::appendThDR(float thDR, int version){
    current_base_FP_tables[selected_base_version].thDRs.push_back(thDR);
    current_base_FP_tables[selected_base_version].version_numbers.push_back(version);
}

void MetadataManager::appendADR(uint64_t single_file_size, uint64_t single_file_dup_size){
    // compute
    source_infos[selected_source].sum_size += single_file_size;
    source_infos[selected_source].dup_size += single_file_dup_size;
    float updated_ADR = (float)source_infos[selected_source].dup_size / (float)source_infos[selected_source].sum_size;

    // append
    source_infos[selected_source].ADRs.push_back(updated_ADR);
}

float MetadataManager::getSampleRatio(){
    return sample_ratio;
}

void MetadataManager::ScodeInitSingleFile(){
    if(!sample_chunk_fps.empty()){
        sample_chunk_fps.pop();
    }
    
    if(!sample_chunk_lens.empty()){
        sample_chunk_lens.pop();
    }

    sample_results.clear();
    sample_self_dup = 0;
    sample_self_table.clear();
}

void MetadataManager::ScodeInit(){
    /*
        ScoDe
        初始化第一个version
    */
    this->current_base_FP_tables[0] = BaseFPTable();
}

bool MetadataManager::isBaseVersion(){
    return isCurrentBase;
}

void MetadataManager::printStatisticsScode(std::ofstream &log_file){
    log_file << "--- --- --- ScoDe statistics --- --- ---" << std::endl;
    log_file << "Base IDs: " << std::endl;
    for(auto& table: this->current_base_FP_tables){
        BaseId base_id = table.first;
    }

    log_file << std::endl;
    for(auto& table: this->current_base_FP_tables){
        BaseId base_id = table.first;
        BaseFPTable& base_table = table.second;
        log_file << "Base ID: " << base_id << std::endl;
        log_file << "thDRs: " << std::endl;
        for(auto& thDR: base_table.thDRs){
            log_file << thDR << " ";
        }
        log_file << std::endl;
    }

    log_file << "Source Infos: " << std::endl;
    for(auto& source: this->source_infos){
        SourceId source_id = source.first;
        SourceInfo& source_info = source.second;
        log_file << "Source ID: " << source_id << std::endl;
        log_file << "ADRs: " << std::endl;
        for(auto& ADR: source_info.ADRs){
            log_file << ADR << " ";
        }
        log_file << std::endl;
    }

    log_file << "Data Churn Simulation Results: " << std::endl;
    for(int i=0; i<simulated_data_churn_results.size(); i++){
        log_file << "Churn num: " << (i+20) << std::endl;
        log_file << "Data size after churn origin: " << simulated_data_churn_results[i].data_size_after_churn_origin << std::endl;
        log_file << "Data size after churn stored: " << simulated_data_churn_results[i].data_size_after_churn_stored << std::endl;
        log_file << "Actual dedup ratio after churn: " << simulated_data_churn_results[i].actual_dedup_ratio_after_churn << std::endl;
    }
}

void MetadataManager::printStatisticsNaive(std::ofstream &log_file){
    log_file << "--- --- --- Naive statistics --- --- ---" << std::endl;
    log_file << "Data Churn Simulation Results: " << std::endl;
    for(int i=0; i<simulated_data_churn_results.size(); i++){
        log_file << "Churn num: " << (i+20) << std::endl;
        log_file << "Data size after churn origin: " << simulated_data_churn_results[i].data_size_after_churn_origin << std::endl;
        log_file << "Data size after churn stored: " << simulated_data_churn_results[i].data_size_after_churn_stored << std::endl;
        log_file << "Actual dedup ratio after churn: " << simulated_data_churn_results[i].actual_dedup_ratio_after_churn << std::endl;
    }
}

void MetadataManager::clearDedupIntervalTable(){
    for(auto& table: this->fp_tables_interval){
        table.clear();
    }
}

void MetadataManager::reserveDedupIntervalTable(int n){
    this->fp_tables_interval.resize(n);
}

void MetadataManager::reserveDedupIntervalTablesByGroup(int n, int group_index){
    this->fp_tables_multi_group[group_index].resize(n);
}

int MetadataManager::addNewEntry(SHA1FP sha1, ENTRY_VALUE value){
    this->fp_table_added.emplace(sha1, value);
    return 0;
}

int MetadataManager::addNewEntry(SHA1FP sha1, ENTRY_VALUE value, int version){
    this->fp_tables_interval[version].emplace(sha1, value);
    
    return 0;
}

int MetadataManager::addNewEntry(SHA1FP sha1, ENTRY_VALUE value, int version, int group_index){
    this->fp_tables_multi_group[group_index][version].emplace(sha1, value);
    
    return 0;
}

int MetadataManager::addNewEntryES(SHA1FP sha1, ENTRY_VALUE value, int version){
    if(version == 0){
        this->ES_base_table.emplace(sha1, value);
    }else{
        this->ES_delta_table.emplace(sha1, value);
    }

    return 0;
}

#define unlikely(x) __builtin_expect(!!(x), 0)
void MetadataManager::addNewEntryDSFI(const SHA1FP& sha1, const ENTRY_VALUE& value){
    if(unlikely(isCurrentBase)){
        this->current_base_FP_tables[selected_base_version].table.emplace(sha1, value);
    }else{
        this->delta_table.emplace(sha1, value);
    }
}

int MetadataManager::addRefCnt(const SHA1FP sha1){
    auto dedupIter = this->fp_table_added.find(sha1);
    if(dedupIter != this->fp_table_added.end())
        return ++dedupIter->second.ref_cnt;
    dedupIter = this->fp_table_origin.find(sha1);
    if(dedupIter != this->fp_table_added.end())
        return ++dedupIter->second.ref_cnt;
    printf("addRefCnt: did not find\n");

    return 0;
}

ENTRY_VALUE MetadataManager::getEntry(const SHA1FP sha1){
    return this->fp_table_origin[sha1];
}


/* 
    MFDedup observation
*/

std::vector<uint64_t> MetadataManager::getMigrationDataSizeTH(){
    return this->th_migrate_data_size;
}

std::vector<uint64_t> MetadataManager::getArhiveDataSizeTH(){
    return this->th_archive_data_size;
}

LookupResult MetadataManager::dedupLookupMFDedup(const SHA1FP& sha1, const ENTRY_VALUE& ev){

    // 自查；
    auto dedupIter = this->MFDedup_self_tables.back().find(sha1);
    if(dedupIter != this->MFDedup_self_tables.back().end()){
        return LookupResult{true, 0};
    }else{
        this->MFDedup_self_tables.back()[sha1] = ev;
    }

    if(MFDedup_self_tables.size() > 1){
        // 查上一个table；
        dedupIter = this->MFDedup_self_tables[MFDedup_self_tables.size() - 2].find(sha1);
        if(dedupIter != this->MFDedup_self_tables[MFDedup_self_tables.size() - 2].end()){
            return LookupResult{true, 0};
        }
    }

    return LookupResult{false, 0};
}

void MetadataManager::MFDedupNewTable(){
    MFDedup_self_tables.emplace_back();
}

void MetadataManager::MFDedupMigration(){
    if(MFDedup_self_tables.size() <= 1){
        // 为下一次migration准备active cat；
        this->active_cat.push(this->MFDedup_self_tables.back());
        th_migrate_data_size.push_back(0);
        th_archive_data_size.push_back(0);
        return ;

    }else{
        th_migrate_data_size.push_back(0);
        th_archive_data_size.push_back(0);

        // 迁移；
        // 我们将论文中的Archive视为迁移的数据，并统计；
        int current_version = MFDedup_self_tables.size();
        int active_queue_size = this->active_cat.size();

        for(int i=0; i<= active_queue_size-1; i++){
            // 队尾创建空cat
            active_cat.emplace(); 

            // 遍历队头cat
            for(auto& x: this->active_cat.front()){
                const SHA1FP & sha1 = x.first;
                auto it = this->MFDedup_self_tables[current_version-1].find(sha1);

                if(it != this->MFDedup_self_tables[current_version-1].end()){
                    // 如果找到，那么则迁移至队尾cat
                    // migrate
                    this->active_cat.back().emplace(sha1, it->second);
                    
                    // 只有队尾才是自身需要迁移的，其他的都是前面版本的，都已经迁移过了，重复计算会导致迁移数量巨大；
                    if(i == active_queue_size-1)
                        this->th_migrate_data_size.back() += x.second.chunk_length;
                }else{
                    // archive
                    this->th_archive_data_size.back() += x.second.chunk_length;
                }
                    
            }

            active_cat.pop(); // 队头出队已经被migrate active cat
        }

        // 最新入队
        this->active_cat.push(this->MFDedup_self_tables.back());
    }
}

/*
    硬编码。MFDedup，HAR采用了保留最新20政策，GCCDF采用了保留最新100政策；
    我们观察变化保留数从20-100，ADR会如何变化，作为敏感性测试；
*/
void MetadataManager::simulateDataChurn(){
    int data_churn_min_num = 20;
    int data_churn_max_num = 100;
    int backup_num = backup_infos.size();

    // 正确性检查
    if(backup_num < data_churn_max_num){
        printf("simulateDataChurn error: backup num %d < data churn max num %d\n", backup_num, data_churn_max_num);
        return ;
    }

    for(int churn_num = data_churn_min_num; churn_num <= data_churn_max_num; churn_num++){
        uint64_t data_size_after_churn_origin = 0;
        uint64_t data_size_after_churn_stored = 0;
        int delte_num = backup_num - churn_num;
        
        // 寻找哪些base应该保留；
        std::unordered_set<int> retain_bases;  
        for(int i=0; i<=delte_num-1; i++){
            if(backup_infos[i].isBaseVersion){
               // 如果这个base的最后一个delta版本都被删掉了，那么这个base也要删掉，否则保留；
               if(current_base_FP_tables[i].version_numbers.back() <= delte_num-1){
                    ;
               }else{
                    retain_bases.insert(i);
               }
            }
        }

        cout<< "churn num: " << churn_num << ", delete num: " << delte_num << ", retain bases: ";
        for(auto& base: retain_bases){
            cout << base << " ";
        }
        cout << endl;

        for(int i=0; i<=delte_num-1; i++){
            if(backup_infos[i].isBaseVersion && retain_bases.find(i) != retain_bases.end()){
                // 有些本应该删掉的数据，但因为是base version，所以被保留了；
                data_size_after_churn_stored += backup_infos[i].base_container_size; 
            }
        }

        for(int i=delte_num; i<=backup_num-1; i++){
            data_size_after_churn_origin += backup_infos[i].backup_size;
            
            // 实际上这这两个值只有一个不是0；
            data_size_after_churn_stored += backup_infos[i].base_container_size;
            data_size_after_churn_stored += backup_infos[i].delta_container_size;
        }

        float actual_dedup_ratio_after_churn =  ((float)data_size_after_churn_origin -  (float)data_size_after_churn_stored)
                                                 / (float)data_size_after_churn_origin;
        simulatedDataChurnResult result{data_size_after_churn_origin, data_size_after_churn_stored, actual_dedup_ratio_after_churn};
        simulated_data_churn_results.push_back(result);
    }
}

void MetadataManager::appendBackupInfo(const backupInfo& info){
    backup_infos.push_back(info);
}

void MetadataManager::simulateDataChurnNaive(){
    int data_churn_min_num = 20;
    int data_churn_max_num = 100;
    int backup_num = backup_infos.size();

    // 正确性检查
    if(backup_num < data_churn_max_num){
        printf("simulateDataChurn error: backup num %d < data churn max num %d\n", backup_num, data_churn_max_num);
        return ;
    }

    for(int churn_num = data_churn_min_num; churn_num <= data_churn_max_num; churn_num++){

        uint64_t data_size_after_churn_origin = 0;
        uint64_t data_size_after_churn_stored = 0;
        int delte_num = backup_num - churn_num;

        for(int i=delte_num; i<=backup_num-1; i++){
            data_size_after_churn_origin += backup_infos[i].backup_size;
            data_size_after_churn_stored += backup_infos[i].unique_data_size;

        }

        float actual_dedup_ratio_after_churn =  ((float)data_size_after_churn_origin - (float)data_size_after_churn_stored) 
                                    / (float)data_size_after_churn_origin;

        simulatedDataChurnResult result{data_size_after_churn_origin, data_size_after_churn_stored, actual_dedup_ratio_after_churn};
        simulated_data_churn_results.push_back(result);
    }
}