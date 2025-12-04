import redis
import secrets
import subprocess
import os

# 配置
rdb_dir = "/home/cyf/ssd0/RDB"
key_num = 30_000
key_num_delta = key_num // 100  # 用整数除法
delta_num = 9
key_size = 25
value_size = 50000

os.makedirs(rdb_dir, exist_ok=True)
r = redis.Redis(host='127.0.0.1', port=6379)

# ✅ 清空当前数据库（db=0），确保实验从干净状态开始
print("Flushing database 0...")
r.flushdb()

# Warmup 阶段（第 0 次）
print("Warmup: inserting", key_num, "keys")
for _ in range(key_num):
    key = secrets.token_hex(key_size)
    value = secrets.token_hex(value_size)
    r.set(key, value)

# 保存第 0 个快照
rdb_path = os.path.join(rdb_dir, f"dump_step_0.rdb")
print("Saving RDB to", rdb_path)
subprocess.run(["redis-cli", "--rdb", rdb_path], check=True)

# 增量插入并保存（第 1 ~ 10 次）
for i in range(1, delta_num + 1):
    print(f"Inserting {key_num_delta} keys (step {i})")
    for _ in range(key_num_delta):
        key = secrets.token_hex(key_size)
        value = secrets.token_hex(value_size)
        r.set(key, value)
    
    rdb_path = os.path.join(rdb_dir, f"dump_step_{i}.rdb")
    print("Saving RDB to", rdb_path)
    subprocess.run(["redis-cli", "--rdb", rdb_path], check=True)

print("Done.")