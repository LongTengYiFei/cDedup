#!/bin/bash
# 这里的配置需要和参数json一致
rm -fr /home/cyf/raid0/working/

mkdir /home/cyf/raid0/working/
mkdir /home/cyf/raid0/working/restoreFolder
mkdir /home/cyf/raid0/working/Containers
mkdir /home/cyf/raid0/working/metadata
mkdir /home/cyf/raid0/working/metadata/FileRecipes
mkdir /home/cyf/raid0/working/metadata/fingerprintsDeltaDedup

touch /home/cyf/raid0/working/metadata/fingerprints.meta

source ./scripts/clear_global_stat.sh

