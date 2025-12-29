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
        void ADREFinal(int current_version_id);
        void appendThDR(float thDR);
        float getSampleRatio();
        void ScodeInit();
        void ScodeInitSingleFile();

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
        */
        // 每个base version对应自身以及附加delta version所有的thDR；
        // 如果该base table closed，直接从map中删除
        using BaseId = int;
        using thDR = float;

        struct SampleResult {
            uint64_t sample_size;
            uint64_t sample_dup_size_against_base;
            float SDR; // thDR
        };
        uint64_t sample_self_dup;
        fpTable sample_self_table;

        struct BaseFPTable{
            fpTable table;
            std::vector<thDR> thDRs;
        };

        std::map<BaseId, SampleResult> sample_results;
        std::map<BaseId, BaseFPTable> current_base_FP_tables; 
        BaseId selected_base_version;
        bool base_table_found;
        bool isCurrentBase;
        fpTable delta_table;
        fpTable* current_fp_indexing_table;
        std::queue<SHA1FP> sample_chunk_fps;
        std::queue<uint32_t> sample_chunk_lens;
        /*
            SDR <>= ldr_ratio * LDR
            ldr_ratio, sample_ratio 需做敏感性测试；
        */
        float ldr_ratio = 0.1; 
        float sample_ratio = 0.05;

        /*
            Estimation: sample ratio sensitivity
        */
       fpTable ES_base_table;
       fpTable ES_delta_table;

};
#endif