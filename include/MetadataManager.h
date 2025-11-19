#ifndef MATADATA_MANAGER_H
#define MATADATA_MANAGER_H

#include <stdio.h>
#include <iostream>
#include <unordered_map>
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
        int load(int restore_version);
        //暂不支持中断打桩写入，只支持目录批量一次性写入，所以没有对应的load函数
        int save(int, int, int);
        LookupResult dedupLookup(SHA1FP sha1);
        LookupResult dedupLookup(SHA1FP sha1, bool);
        int addNewEntry(const SHA1FP sha1, const ENTRY_VALUE value);
        int addNewEntry(const SHA1FP sha1, const ENTRY_VALUE value, bool);
        int addRefCnt(const SHA1FP sha1);
        ENTRY_VALUE getEntry(const SHA1FP sha1);
        std::string genFPname(int version, bool base);
        void loadDeltaDedupFp(std::string fp_name);

    private:
        std::string metadata_file_path;
        // FP-index used for normal deduplication
        std::unordered_map<SHA1FP, ENTRY_VALUE, TupleHasher, TupleEqualer> fp_table_origin;
        std::unordered_map<SHA1FP, ENTRY_VALUE, TupleHasher, TupleEqualer> fp_table_added;

        // used for delta deduplication
        std::unordered_map<SHA1FP, ENTRY_VALUE, TupleHasher, TupleEqualer> fp_table_base;
        std::unordered_map<SHA1FP, ENTRY_VALUE, TupleHasher, TupleEqualer> fp_table_delta;
};
#endif