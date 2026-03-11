#ifndef MATADATA_MANAGER_H
#define MATADATA_MANAGER_H

#include <stdio.h>
#include <iostream>
#include <vector>
#include <map>
#include <unordered_map>
#include <queue>
#include "general.h"

struct LookupResult{
    bool dup;
    uint32_t container_index;
};

struct __attribute__ ((__packed__)) SHA1FP {
    // 20 bytes
    uint64_t fp1;
    uint32_t fp2, fp3, fp4;

    void print() {
        printf("%lu:%d:%d:%d\n", fp1, fp2, fp3, fp4);
    }
};

struct CHUNK{
    SHA1FP sha1;
    uint32_t len;
    unsigned char* data;
};

struct ENTRY_VALUE {
    uint32_t container_number;
    uint32_t offset;
    uint16_t chunk_length;
    uint16_t container_inner_index;
    uint32_t ref_cnt;
    uint32_t version;
};

struct TupleHasher {
    std::size_t operator()(const SHA1FP &key) const {
        return key.fp1;
    }
};

struct TupleEqualer {
    bool operator()(const SHA1FP &lhs, const SHA1FP &rhs) const {
        return lhs.fp1 == rhs.fp1 && lhs.fp2 == rhs.fp2 && lhs.fp3 == rhs.fp3 && lhs.fp4 == rhs.fp4;
    }
};


struct backupInfo{
    uint64_t backup_size;
    uint64_t base_container_size;
    uint64_t delta_container_size;
    bool isBaseVersion;
};

class MetadataManager {
    public:
        MetadataManager(const std::string& file_path) {
            this->metadata_file_path = file_path;

        }

        int save();
        int load();
        //暂不支持中断打桩写入，只支持目录批量一次性写入，所以没有对应的load函数
        int save(int, int, int);
        int load(int restore_version);

        LookupResult dedupLookup(SHA1FP sha1);
        LookupResult dedupLookup(SHA1FP sha1, int base, int delta);
        LookupResult dedupLookup(SHA1FP sha1, int base, int delta, int group_index);
        void dedupLookupADRE(const SHA1FP& chunk_fp, uint32_t chunk_len);
        LookupResult dedupLookupDSFI(const SHA1FP& chunk_fp);
        SHA1FP popSampleChunkFP();
        uint32_t popSampleChunkLen();
        void ADREFinal(int, uint64_t file_size, float & estimated_thDR_N);
        void appendThDR(float, int version);
        void appendADR(uint64_t single_file_size, uint64_t single_file_dup_size);
        float getSampleRatio();
        void ScodeInit();
        void ScodePrintStatistics(std::ofstream &log_file);
        void ScodeInitSingleFile();
        bool isBaseVersion();

        int addNewEntry(const SHA1FP sha1, const ENTRY_VALUE value);
        int addNewEntry(const SHA1FP sha1, const ENTRY_VALUE value, int version);
        int addNewEntry(const SHA1FP sha1, const ENTRY_VALUE value, int version, int group_index);
        void addNewEntryDSFI(const SHA1FP& sha1, const ENTRY_VALUE& value);

        // estimation sensitiy
        LookupResult dedupLookupES(SHA1FP sha1, int version);
        int addNewEntryES(const SHA1FP sha1, const ENTRY_VALUE value, int version);
        void ESClear();

        int addRefCnt(const SHA1FP sha1);
        ENTRY_VALUE getEntry(const SHA1FP sha1);
        std::string genFPname(int version, bool base);
        void loadDeltaDedupFp(std::string fp_name);
        void reserveDedupIntervalTable(int n);
        void reserveDedupIntervalTablesByGroup(int n, int group_index);
        void clearDedupIntervalTable();

        // MFDedup observation
        std::vector<uint64_t> getMigrationDataSizeTH();
        std::vector<uint64_t> getArhiveDataSizeTH();
        LookupResult dedupLookupMFDedup(const SHA1FP& sha1, const ENTRY_VALUE& ev);
        void MFDedupNewTable();
        void MFDedupMigration();

        // data churn simulation
        void simulateDataChurn();
        void appendBackupInfo(const backupInfo& info);

    private:
        using fpTable = std::unordered_map<SHA1FP, ENTRY_VALUE, TupleHasher, TupleEqualer>;
        std::string metadata_file_path;

        // dedup naive
        fpTable fp_table_origin;
        fpTable fp_table_added;

        // dedup first and interval
        std::vector<fpTable> fp_tables_interval; // 同时包含base table和delta table

        /*
            用法1：interval -> table(base) table table .... table(base) table table
            用法2：window_start -> tables 
        */
        std::map<int, std::vector<fpTable>> fp_tables_multi_group;

        /*
            ScoDe
             每个base version对应自身以及附加delta version所有的thDR；
            如果该base table closed，直接从map中删除
        */
        using BaseId = int;
        using SourceId = int;
        using thDR = float;
        using ADR = float;

        struct SampleResult {
            uint64_t sample_size;
            uint64_t sample_dup_size_against_base;
            float SDR; // thDR
        };
        uint64_t sample_self_dup;
        fpTable sample_self_table;

        struct BaseFPTable{
            SourceId source_id;
            fpTable table;
            std::vector<thDR> thDRs;
            std::vector<int> version_numbers;
        };

        struct SourceInfo{
            uint64_t sum_size;
            uint64_t dup_size;
            std::vector<float> ADRs;
        };

        SourceId source_incremental = 0;
        SourceId selected_source;
        std::map<SourceId, SourceInfo> source_infos;
        
        BaseId selected_base_version;
        std::map<BaseId, BaseFPTable> current_base_FP_tables; 

        std::map<BaseId, SampleResult> sample_results;
        bool base_table_found;
        bool isCurrentBase;
        fpTable delta_table;
        std::queue<SHA1FP> sample_chunk_fps;
        std::queue<uint32_t> sample_chunk_lens;

        std::vector<backupInfo> backup_infos;

        struct simulatedDataChurnResult{
            uint64_t data_size_after_churn_origin;
            uint64_t data_size_after_churn_stored;
            float actual_dedup_ratio_after_churn;
        };
        std::vector<simulatedDataChurnResult> simulated_data_churn_results;

        /*
            SDR <>= ldr_ratio * LDR
            ldr_ratio, sample_ratio 需做敏感性测试；
        */
        float ldr_ratio = 0.5; 
        float sample_ratio = 0.10;

        /*
            Estimation: sample ratio sensitivity
        */
       fpTable ES_base_table;
       fpTable ES_delta_table;

       // MFDedup data migration observation
       std::vector<uint64_t> th_migrate_data_size;
       std::vector<uint64_t> th_archive_data_size;
       std::vector<fpTable> MFDedup_self_tables;
       std::queue<fpTable> active_cat;

};
#endif