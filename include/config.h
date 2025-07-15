#ifndef __CONFIG_H__
#define __CONFIG_H__
#include<iostream>
#include <string.h>
#include"cJSON.h"
using namespace std;

enum TASK_TYPE{
    TASK_RESTORE,
    TASK_WRITE,
    TASK_LOC,
    TASK_WRITE_PIPELINE,
    TASK_DELETE,
    NOT_CHOOSED
};

enum CHUNKING_METHOD{
    CDC,
    FSC,
    FULL_FILE
};

enum RESTORE_METHOD{
    NAIVE_RESTORE,  //无缓冲模式   
    CONTAINER_CACHE,//基于容器的缓冲
    CHUNK_CACHE,    //基于数据块的缓冲
    FAA_FIXED,      //一次性发送FAA
    FAA_ROLLING,    //FAA环形缓冲区
};

enum LANG{
    LANG_PYTHON = 0,
    LANG_CPP,
    LANG_C,
    LANG_JAVA,
    LANG_CSHARP,
    LANG_JAVASCRIPT,
    LANG_GO,
    LANG_VB,
    LANG_DELPHI,
    LANG_FORTRAN
};

enum ClocMethod{
    NON_CLOC=0,
    NAIVE_CLOC ,
    DC_NON_ALIGN,
    DC_NEWLINE,
    DC_NEW_MULTI,
    DC_OFFLINE
};

class Config{
    public:
        static Config& getInstance() {
          static Config instance;
          return instance;
        }

        // getters
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

        string getFingerprintsFilePath(){return this->fingerprints_file_path;}
        string getFileRecipesPath(){return this->file_recipe_path;}        
        string getContainersPath(){return this->container_path;}     

        int getBackwardScanScope(){return this->backward_scan_scope;}
        enum LANG getLanugage(){return this->language;}
        enum ClocMethod getClocMethod(){return this->cloc_method;}

        // setters
        void setTask(char* s){this->tt = taskTypeTrans(s);}
        void setInputFile(char* s){this->input_path = s;}
        void setRestorePath(char* s){this->restore_path = s;}
        void setRestoreVersion(int n){this->restore_version = n;}
        void setChunkingMethod(char* s){this->cm = cmTypeTrans(s);}
        void setRestoreId(int n){this->restore_id = n;}
        void setDeleteId(int n){this->delete_id = n;}
        void setSize(int n){this->avg_chunk_size = n;}
        void setNormal(int n){this->normal_level = n;}
        void setRestoreMethod(char* s){this->rm = restoreMethodTrans(s);}

        void setFingerprintsFilePath(char* s){this->fingerprints_file_path = s;}
        void setFileRecipesPath(char* s){this->file_recipe_path = s;}        
        void setContainersPath(char* s){this->container_path = s;}     

        void setBackwardScanScope(int n){this->backward_scan_scope = n;}
        void setLanguage(char* s){this->language = languageTrans(s);}
        void setClocMethod(char* s){this->cloc_method = clocMethodTrans(s);}

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
                } else if (strcmp(name, "InputFile") == 0) {
                    Config::getInstance().setInputFile(valuestring);
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
                }
                //2. metadata configurations
                else if (strcmp(name, "fingerprintsFilePath") == 0) {
                    Config::getInstance().setFingerprintsFilePath(valuestring);
                } else if (strcmp(name, "fileRecipesPath") == 0) {
                    Config::getInstance().setFileRecipesPath(valuestring);
                } else if (strcmp(name, "containersPath") == 0) {
                    Config::getInstance().setContainersPath(valuestring);
                }

                // cloc
                else if(strcmp(name, "BackwardScanScope") == 0) {
                    Config::getInstance().setBackwardScanScope(val_int);
                } else if(strcmp(name, "language") == 0) {
                    Config::getInstance().setLanguage(valuestring);
                } else if(strcmp(name, "ClocMethod") == 0) {
                    Config::getInstance().setClocMethod(valuestring);
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
        string fingerprints_file_path; // 也可用作merkle tree L0
        string file_recipe_path;
        string container_path;

        // cloc
        int backward_scan_scope;
        enum LANG language;
        enum ClocMethod cloc_method;

        Config() {
            avg_chunk_size = 4096;
            normal_level = 2;
        }

        enum TASK_TYPE taskTypeTrans(char* s){
            if(strcmp(s, "write") == 0){
                return TASK_WRITE;
            }else if(strcmp(s, "cloc") == 0){
                return TASK_LOC;
            }else if (strcmp(s, "write_pipeline") == 0){
                return TASK_WRITE_PIPELINE;
            }else if (strcmp(s, "restore") == 0){
                return TASK_RESTORE;
            }else if (strcmp(s, "delete") == 0){
                return TASK_DELETE;
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
            }else if (strcmp(s, "file") == 0){
                return FULL_FILE;
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

        enum LANG languageTrans(char* s){
            if(strcmp(s, "python") == 0){
                return LANG_PYTHON;
            } else if (strcmp(s, "c++") == 0){
                return LANG_CPP;
            } else if (strcmp(s, "c") == 0){
                return LANG_C;
            } else if (strcmp(s, "java") == 0){
                return LANG_JAVA;
            } else if (strcmp(s, "c#") == 0){
                return LANG_CSHARP;
            } else if (strcmp(s, "javascript") == 0){
                return LANG_JAVASCRIPT;
            } else if (strcmp(s, "go") == 0){
                return LANG_GO;
            } else if (strcmp(s, "visual basic") == 0){
                return LANG_VB;
            } else if (strcmp(s, "delphi") == 0){
                return LANG_DELPHI;
            } else if (strcmp(s, "fortran") == 0){
                return LANG_FORTRAN;
            }   
            // should not reach here
            printf("语言转换失败\n");
            return LANG_CPP;   
        }

        enum ClocMethod clocMethodTrans(char* s){
            if(strcmp(s,"non-cloc") == 0){
                return NON_CLOC;
            }else if(strcmp(s, "naive-cloc") == 0){
                return NAIVE_CLOC;
            } else if(strcmp(s, "non-align") == 0){
                return DC_NON_ALIGN;
            } else if (strcmp(s, "newline") == 0){
                return DC_NEWLINE;
            } else if (strcmp(s, "new+multi") == 0){
                return DC_NEW_MULTI;
            } else if (strcmp(s, "dedup-cloc-offline") == 0){
                return DC_OFFLINE;
            }
            // should not reach here
            printf("cloc方法转换失败\n");
            return NAIVE_CLOC;   
        }

        RESTORE_METHOD restoreMethodTrans(char* s){
            if(strcmp(s, "naive") == 0){
                return NAIVE_RESTORE;
            }else if (strcmp(s, "container") == 0){
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