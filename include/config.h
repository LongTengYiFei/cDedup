#ifndef __CONFIG_H__
#define __CONFIG_H__
#include<iostream>
#include <string.h>
#include"cJSON.h"
using namespace std;

enum DedupType{
    Naive,
    DedupInterval,
    DedupFirst
};

enum TASK_TYPE{
    TASK_RESTORE,
    TASK_WRITE,
    TASK_WRITE_PIPELINE,
    TASK_DELETE,
    TASK_INTERVAL_OBSERVATION,
    NOT_CHOOSED
};

enum CHUNKING_METHOD{
    CDC,
    FSC,
};

enum RESTORE_METHOD{  
    CONTAINER_CACHE,//基于容器的缓冲
    CHUNK_CACHE,    //基于数据块的缓冲
    FAA_FIXED,      //一次性发送FAA
    FAA_ROLLING,    //FAA环形缓冲区
};

class Config{
    public:
        static Config& getInstance() {
          static Config instance;
          return instance;
        }

        // Getters
        enum TASK_TYPE getTaskType(){return this->tt;}
        string getInputPath(){return this->input_path;}
        string getRestorePath(){return this->restore_path;}
        int getRestoreVersion(){return this->restore_version;}
        enum CHUNKING_METHOD getChunkingMethod(){return this->cm;}
        int getRestoreId(){return this->restore_id;}
        int getDeleteId(){return this->delete_id;}
        int getAvgChunkSize(){return this->avg_chunk_size;}
        int getNormalLevel(){return this->normal_level;}
        enum RESTORE_METHOD getRestoreMethod(){return this->rm;}
        string getFpDeltaDedupFolderPath(){return this->fp_DeltaDedup_folder_path;}
        string getFingerprintsFilePath(){return this->fingerprints_file_path;}
        string getFileRecipesPath(){return this->file_recipe_path;}        
        string getContainersPath(){return this->container_path;}     
        string getBaseContainersPath(){return this->base_container_path;}     
        string getDeltaContainersPath(){return this->delta_container_path;}     
        int getBaseSize(){return this->base_size;}
        int getInterval(){return this->interval;}
        enum DedupType getDedupType(){return this->dedup_type;}
        string getDedupLogPath(){return this->log_file_path;}   

        // Setters
        void setTask(char* s){this->tt = taskTypeTrans(s);}
        void setInputFilesList(char* s){this->input_path = s;}
        void setRestorePath(char* s){this->restore_path = s;}
        void setRestoreVersion(int n){this->restore_version = n;}
        void setChunkingMethod(char* s){this->cm = cmTypeTrans(s);}
        void setRestoreId(int n){this->restore_id = n;}
        void setDeleteId(int n){this->delete_id = n;}
        void setSize(int n){this->avg_chunk_size = n;}
        void setNormal(int n){this->normal_level = n;}
        void setRestoreMethod(char* s){this->rm = restoreMethodTrans(s);}

        void setFpDeltaDedupFolder(char* s){this->fp_DeltaDedup_folder_path = s;}
        void setFingerprintsFilePath(char* s){this->fingerprints_file_path = s;}
        void setFileRecipesPath(char* s){this->file_recipe_path = s;}        
        void setContainersPath(char* s){this->container_path = s;}     
        void setBaseContainersPath(char* s){this->base_container_path = s;}
        void setDeltaContainersPath(char* s){this->delta_container_path = s;}
        void setBaseSize(int n){this->base_size = n;};
        void setInterval(int n){this->interval = n;};
        void setLogFilePath(char* s){this->log_file_path = s;}
        void setDedupType(char* s){this->dedup_type = dedupTypeTrans(s);};


        // you know
        void parse_argument(int argc, char **argv)
        {
            char source[2000 + 1];
            FILE *fp = fopen(argv[1], "r");
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
                char *name = param->string;
                char *valuestring = param->valuestring;
                int val_int = param->valueint;

                //1. User indicate configurations
                if (strcmp(name, "Task") == 0) {
                    Config::getInstance().setTask(valuestring);
                } else if (strcmp(name, "InputFilesList") == 0) {
                    Config::getInstance().setInputFilesList(valuestring);
                } else if (strcmp(name, "RestorePath") == 0) {
                    Config::getInstance().setRestorePath(valuestring);
                } else if (strcmp(name, "RestoreVersion") == 0) {
                    Config::getInstance().setRestoreVersion(val_int);
                } else if (strcmp(name, "ChunkingMethod") == 0) {
                    Config::getInstance().setChunkingMethod(valuestring);
                } else if (strcmp(name, "RestoreId") == 0) {
                    Config::getInstance().setRestoreId(val_int);
                } else if (strcmp(name, "DeleteId") == 0) {
                    Config::getInstance().setDeleteId(val_int);
                }else if (strcmp(name, "Size") == 0) {
                    Config::getInstance().setSize(val_int);
                }else if (strcmp(name, "Normal") == 0) {
                    Config::getInstance().setNormal(val_int);
                }else if (strcmp(name, "RestoreMethod") == 0) {
                    Config::getInstance().setRestoreMethod(valuestring);
                }else if (strcmp(name, "fingerprintsDeltaDedupFolder") == 0) {
                    Config::getInstance().setFpDeltaDedupFolder(valuestring);
                }else if (strcmp(name, "fingerprintsFilePath") == 0) {
                    Config::getInstance().setFingerprintsFilePath(valuestring);
                }else if (strcmp(name, "fileRecipesPath") == 0) {
                    Config::getInstance().setFileRecipesPath(valuestring);
                }else if (strcmp(name, "containersPath") == 0) {
                    Config::getInstance().setContainersPath(valuestring);
                }else if (strcmp(name, "baseContainersPath") == 0) {
                    Config::getInstance().setBaseContainersPath(valuestring);
                }else if (strcmp(name, "deltaContainersPath") == 0) {
                    Config::getInstance().setDeltaContainersPath(valuestring);
                }else if (strcmp(name, "base_size") == 0) {
                    Config::getInstance().setBaseSize(val_int);
                }else if (strcmp(name, "Interval") == 0) {
                    Config::getInstance().setInterval(val_int);
                }else if(strcmp(name, "LogFilePath") == 0){
                    Config::getInstance().setLogFilePath(valuestring);
                }else if(strcmp(name, "DedupType") == 0){
                    Config::setDedupType(valuestring);
                }
            }
        }

    private:
        // 读写任务参数
        enum TASK_TYPE tt;
        enum CHUNKING_METHOD cm;
        enum RESTORE_METHOD rm;
        string input_path;
        string restore_path;
        int restore_version;    // used for cdc
        int restore_id;         // used for full file dedup
        int delete_id;
        int avg_chunk_size;     // unit KiB
        int normal_level;

        // 元数据相关参数
        string fp_DeltaDedup_folder_path;
        string fingerprints_file_path; // 也可用作merkle tree L0
        string file_recipe_path;
        string container_path;
        string base_container_path;
        string delta_container_path;

        int base_size;

        string log_file_path;
        enum DedupType dedup_type;
        int interval;

        Config() {
            avg_chunk_size = 4096;
            normal_level = 2;
        }

        enum DedupType dedupTypeTrans(char* s){
            if(strcmp(s, "naive") == 0){
                return Naive;
            }else if (strcmp(s, "dedup_interval") == 0){
                return DedupInterval;
            }else if (strcmp(s, "dedup_first") == 0){
                return DedupFirst; 
            }else{
                printf("Not support dedup type:%s\n", s);
                exit(-1);
            }
        }

        enum TASK_TYPE taskTypeTrans(char* s){
            if(strcmp(s, "write") == 0){
                return TASK_WRITE;
            }else if (strcmp(s, "write_pipeline") == 0){
                return TASK_WRITE_PIPELINE;
            }else if (strcmp(s, "restore") == 0){
                return TASK_RESTORE;
            }else if (strcmp(s, "delete") == 0){
                return TASK_DELETE;
            }else if(strcmp(s, "interval_observation") == 0){
                return TASK_INTERVAL_OBSERVATION;
            }else{
                printf("Not support task type:%s\n", s);
                exit(-1);
            }
        }

        enum CHUNKING_METHOD cmTypeTrans(char* s){
            if(strcmp(s, "cdc") == 0){
                return CDC;
            }else if (strcmp(s, "fsc") == 0){
                return FSC;
            }else{
                printf("Not support chunking method type:%s\n", s);
                exit(-1);
            }
        }

        bool yesNoTrans(char* s){
            if(strcmp(s, "yes") == 0){
                return true;
            }else if (strcmp(s, "no") == 0){
                return false;
            }else{
                printf("Not support yes no type:%s\n", s);
                exit(-1);
            }
        }

        RESTORE_METHOD restoreMethodTrans(char* s){
            if (strcmp(s, "container") == 0){
                return CONTAINER_CACHE;
            }else if (strcmp(s, "chunk") == 0){
                return CHUNK_CACHE;
            }else if (strcmp(s, "faa_fixed") == 0){
                return FAA_FIXED;
            }else if (strcmp(s, "faa_rolling") == 0){
                return FAA_ROLLING;
            }else{
                printf("Not support restore method:%s\n", s);
                exit(-1);
            }
        }
};
#endif