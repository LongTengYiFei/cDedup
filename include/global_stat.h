#ifndef __GLOBALSTAT_H__
#define __GLOBALSTAT_H__
#include<iostream>
#include <string.h>
#include"cJSON.h"
#include <unistd.h>
using namespace std;

// json好像不支持long long
// 每次重删任务前请手动将json item置0
class GlobalStat{
public:
    static GlobalStat& getInstance() {
        static GlobalStat instance;
        return instance;
    }

    // getters
    uint64_t getLogicalSize(){return this->logical_size;}
    uint64_t getPhysicalSize(){return this->physical_size;}
    double getDR(){return this->DR;}

    void parse_arguments(const char * json_path)
    {
        char source[2000 + 1];
        FILE *fp = fopen(json_path, "r");
        if (fp != NULL) {
            size_t newLen = fread(source, sizeof(char), 1001, fp);
            if ( ferror( fp ) != 0 ) {
                fputs("Error reading file", stderr);
            } else {
                source[newLen++] = '\0'; /* Just to be safe. */
            }

            fclose(fp);
        }else{
            perror("open json file failed");
        }

        cJSON *config = cJSON_Parse(source);
        for (cJSON *param = config->child; param != nullptr; param = param->next) {
            char *item_name = param->string;

            char* val_string = param->valuestring;
            double val_dol = param->valuedouble;

            //1. User indicate configurations
            if (strcmp(item_name, "LogicalSize") == 0) {
                GlobalStat::getInstance().setLogicalSize(val_string);
            } else if (strcmp(item_name, "PhysicalSize") == 0) {
                GlobalStat::getInstance().setPhysicalSize(val_string);
            } else if (strcmp(item_name, "DeduplicationRatio") == 0) {
                GlobalStat::getInstance().setDR(val_dol);
            }
        }

        //show
        printf("-------Parsing Old Global Arguments-------\n");
        printf("Init Logical Size %" PRIu64 "\n", GlobalStat::getInstance().getLogicalSize());
        printf("Init Physical Size %" PRIu64 "\n", GlobalStat::getInstance().getPhysicalSize());
        printf("Init DR %.2f\n", GlobalStat::getInstance().getDR());
    }

    void save_arguments(const char * json_path)
    {
        //Parsing路径和Saving路径务必一致
        char source[2000 + 1]={0};
        FILE *fp = fopen(json_path, "r+");
        if (fp != NULL) {
            size_t newLen = fread(source, sizeof(char), 1001, fp);
            if ( ferror( fp ) != 0 ) {
                fputs("Error reading file", stderr);
            } else {
                source[newLen++] = '\0'; /* Just to be safe. */
            }
        }else{
            perror("open json file failed");
        }

        cJSON *config = cJSON_Parse(source);
        string ls = to_string(GlobalStat::getInstance().getLogicalSize());
        string ps = to_string(GlobalStat::getInstance().getPhysicalSize());
        cJSON_ReplaceItemInObject(config, "LogicalSize", cJSON_CreateString(ls.c_str()));
        cJSON_ReplaceItemInObject(config, "PhysicalSize", cJSON_CreateString(ps.c_str()));
        cJSON_ReplaceItemInObject(config, "DeduplicationRatio", cJSON_CreateNumber(GlobalStat::getInstance().getDR()));
        
        char *cjValue = cJSON_Print(config);

        int ret = ftruncate(fileno(fp), 0);
        if (ret < 0) {
            perror("ftruncate error, the reason is ");
            exit(-1);
        }

        fseek(fp, 0, SEEK_SET);

        ret = fwrite((void*)cjValue, sizeof(char), strlen(cjValue), fp);
        if (ret < 0) {
            perror("write json file error!\n");
        }

        fclose(fp);
        free(cjValue);
        cJSON_Delete(config);

        //show
        printf("-------Saving New Global Arguments-------\n");
        printf("Logical Size %" PRIu64 "\n", GlobalStat::getInstance().getLogicalSize());
        printf("Physical Size %" PRIu64 "\n", GlobalStat::getInstance().getPhysicalSize());
        printf("DR %.2f\n", GlobalStat::getInstance().getDR());
    }

    void update(uint64_t backup_logical_size, uint64_t backup_physical_size){
        this->logical_size += backup_logical_size;
        this->physical_size += backup_physical_size;

        this->DR = 1 - (double)this->physical_size / (double)this->logical_size;
    }

private:
    // setters
    void setLogicalSize(string n){this->logical_size = std::stoull(n);}
    void setPhysicalSize(string n){this->physical_size = std::stoull(n);}
    void setDR(double n){this->DR = n;}

    // 全局重删压缩信息
    uint64_t logical_size;
    uint64_t physical_size;
    double DR;
};

#endif