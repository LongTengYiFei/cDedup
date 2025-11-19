#!/bin/bash
# 这里的配置需要和参数json一致
rm -fr /home/cyf/ssd1/working/

mkdir /home/cyf/ssd1/working/
mkdir /home/cyf/ssd1/working/restoreFolder
mkdir /home/cyf/ssd1/working/Containers
mkdir /home/cyf/ssd1/working/metadata
mkdir /home/cyf/ssd1/working/metadata/FileRecipes
mkdir /home/cyf/ssd1/working/metadata/fingerprintsDeltaDedup

touch /home/cyf/ssd1/working/metadata/fingerprints.meta

source ./scripts/clear_global_stat.sh

