# cDedup

### Usage

+ Backup a new workload into the system
```
./cDedup --task write --InputFile [backup workload]
```
+ Restore a workload of from the system
```
./cDedup --task restore --RestorePath [path to restore] --RestoreRecipe [which version to restore(1 ~ no. of the last retained version)]
```